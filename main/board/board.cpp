#include "board.h"

#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "driver/i2c.h"
#include "driver/spi_master.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_touch_cst816s.h"
#include "esp_log.h"

static const char *TAG = "board";

// ── RM67162 QSPI command encoding ─────────────────────────────────────────
//
// The RM67162 uses a 4-byte QSPI prefix before each command:
//   [0x02][0x00][0x00][cmd]  — parameter write (1-line address, 4-line data)
//   [0x32][0x00][0x00][0x2C] — memory write    (all quad, fast)
//
// With lcd_cmd_bits=32 each value below is sent as one 32-bit SPI word.
//
#define RM_CMD(c)    (0x02000000UL | ((uint32_t)(c)))
#define RM_RAMWR     (0x32000000UL | 0x2CUL)   // memory write, quad prefix

// RM67162 init sequence ─────────────────────────────────────────────────────
// Each entry: { command_32bit, {params...}, param_len, delay_ms }
typedef struct {
    uint32_t cmd;
    uint8_t  data[16];
    uint8_t  len;
    uint8_t  delay_ms;
} lcd_init_cmd_t;

static const lcd_init_cmd_t k_rm67162_init[] = {
    // Extended page 1 — power / timing registers
    { RM_CMD(0xFE), {0x04}, 1, 0 },
    { RM_CMD(0x4E), {0x02}, 1, 0 },
    { RM_CMD(0x4F), {0x02}, 1, 0 },
    { RM_CMD(0x50), {0x02}, 1, 0 },
    { RM_CMD(0x51), {0x02}, 1, 0 },
    // Extended page 0
    { RM_CMD(0xFE), {0x00}, 1, 0 },
    // Brightness to max
    { RM_CMD(0x51), {0xFF}, 1, 0 },
    // Pixel format: 0x55 = RGB565 (16bpp)
    { RM_CMD(0x3A), {0x55}, 1, 0 },
    // Tearing effect line: vertical blanking only
    { RM_CMD(0x35), {0x00}, 1, 0 },
    // Sleep out — must wait ≥120ms before sending DISPON
    { RM_CMD(0x11), {}, 0, 120 },
    // Display on
    { RM_CMD(0x29), {}, 0, 20  },
};

// ── Custom RM67162 panel driver ────────────────────────────────────────────
// Wraps esp_lcd_panel_io and implements the esp_lcd_panel_t interface.

typedef struct {
    esp_lcd_panel_t          base;  // MUST be first — cast relies on this
    esp_lcd_panel_io_handle_t io;
    int x_gap, y_gap;
} rm67162_t;

static esp_err_t rm67162_del(esp_lcd_panel_t *panel) {
    free(panel);
    return ESP_OK;
}

static esp_err_t rm67162_reset(esp_lcd_panel_t *panel) {
    gpio_set_level(BOARD_LCD_RST, 0); vTaskDelay(pdMS_TO_TICKS(10));
    gpio_set_level(BOARD_LCD_RST, 1); vTaskDelay(pdMS_TO_TICKS(120));
    return ESP_OK;
}

static esp_err_t rm67162_init(esp_lcd_panel_t *panel) {
    rm67162_t *p = (rm67162_t *)panel;
    for (int i = 0; i < (int)(sizeof(k_rm67162_init) / sizeof(k_rm67162_init[0])); i++) {
        const lcd_init_cmd_t *c = &k_rm67162_init[i];
        ESP_ERROR_CHECK(esp_lcd_panel_io_tx_param(p->io, c->cmd,
                        c->len ? c->data : NULL, c->len));
        if (c->delay_ms) vTaskDelay(pdMS_TO_TICKS(c->delay_ms));
    }
    return ESP_OK;
}

static esp_err_t rm67162_draw_bitmap(esp_lcd_panel_t *panel,
                                      int x1, int y1, int x2, int y2,
                                      const void *color_data)
{
    rm67162_t *p = (rm67162_t *)panel;

    // Column address (CASET 0x2A): x1..x2 inclusive → x1, x2+1 exclusive
    uint8_t caset[4] = {
        (uint8_t)((x1 + p->x_gap) >> 8), (uint8_t)(x1 + p->x_gap),
        (uint8_t)((x2 + p->x_gap) >> 8), (uint8_t)(x2 + p->x_gap),
    };
    ESP_ERROR_CHECK(esp_lcd_panel_io_tx_param(p->io, RM_CMD(0x2A), caset, 4));

    // Row address (RASET 0x2B)
    uint8_t raset[4] = {
        (uint8_t)((y1 + p->y_gap) >> 8), (uint8_t)(y1 + p->y_gap),
        (uint8_t)((y2 + p->y_gap) >> 8), (uint8_t)(y2 + p->y_gap),
    };
    ESP_ERROR_CHECK(esp_lcd_panel_io_tx_param(p->io, RM_CMD(0x2B), raset, 4));

    // Memory write (uses 0x32 quad-write prefix)
    size_t px = (size_t)(x2 - x1 + 1) * (size_t)(y2 - y1 + 1);
    ESP_ERROR_CHECK(esp_lcd_panel_io_tx_param(p->io, RM_RAMWR, color_data, px * 2));

    return ESP_OK;
}

// Stubs for operations we don't need at runtime
static esp_err_t rm67162_mirror(esp_lcd_panel_t *, bool, bool) { return ESP_OK; }
static esp_err_t rm67162_swap_xy(esp_lcd_panel_t *, bool)       { return ESP_OK; }
static esp_err_t rm67162_set_gap(esp_lcd_panel_t *panel, int x, int y) {
    ((rm67162_t *)panel)->x_gap = x;
    ((rm67162_t *)panel)->y_gap = y;
    return ESP_OK;
}
static esp_err_t rm67162_invert_color(esp_lcd_panel_t *p, bool inv) {
    rm67162_t *rm = (rm67162_t *)p;
    return esp_lcd_panel_io_tx_param(rm->io, RM_CMD(inv ? 0x21 : 0x20), NULL, 0);
}
static esp_err_t rm67162_disp_on_off(esp_lcd_panel_t *p, bool on) {
    rm67162_t *rm = (rm67162_t *)p;
    return esp_lcd_panel_io_tx_param(rm->io, RM_CMD(on ? 0x29 : 0x28), NULL, 0);
}

static esp_err_t new_rm67162_panel(esp_lcd_panel_io_handle_t io,
                                    esp_lcd_panel_handle_t *out_panel)
{
    rm67162_t *p = (rm67162_t *)calloc(1, sizeof(rm67162_t));
    if (!p) return ESP_ERR_NO_MEM;

    p->io = io;
    p->base.del          = rm67162_del;
    p->base.reset        = rm67162_reset;
    p->base.init         = rm67162_init;
    p->base.draw_bitmap  = rm67162_draw_bitmap;
    p->base.mirror       = rm67162_mirror;
    p->base.swap_xy      = rm67162_swap_xy;
    p->base.set_gap      = rm67162_set_gap;
    p->base.invert_color = rm67162_invert_color;
    p->base.disp_on_off  = rm67162_disp_on_off;

    *out_panel = &p->base;
    return ESP_OK;
}

// ── Public board init ──────────────────────────────────────────────────────

esp_err_t board_init(esp_lcd_panel_handle_t *out_panel,
                     esp_lcd_touch_handle_t  *out_touch)
{
    // ── GPIO for RST
    gpio_config_t rst_cfg = {
        .pin_bit_mask = 1ULL << BOARD_LCD_RST,
        .mode         = GPIO_MODE_OUTPUT,
    };
    ESP_ERROR_CHECK(gpio_config(&rst_cfg));

    // ── SPI bus (QSPI — 4 data lines)
    spi_bus_config_t bus_cfg = {};
    bus_cfg.data0_io_num     = BOARD_LCD_D0;
    bus_cfg.data1_io_num     = BOARD_LCD_D1;
    bus_cfg.data2_io_num     = BOARD_LCD_D2;
    bus_cfg.data3_io_num     = BOARD_LCD_D3;
    bus_cfg.sclk_io_num      = BOARD_LCD_CLK;
    bus_cfg.max_transfer_sz  = BOARD_LCD_W * BOARD_LCD_H * 2 + 8;
    ESP_ERROR_CHECK(spi_bus_initialize(BOARD_LCD_QSPI_HOST, &bus_cfg, SPI_DMA_CH_AUTO));

    // ── Panel IO (QSPI, 32-bit commands)
    esp_lcd_panel_io_handle_t io;
    esp_lcd_panel_io_spi_config_t io_cfg = {};
    io_cfg.cs_gpio_num       = BOARD_LCD_CS;
    io_cfg.dc_gpio_num       = -1;   // no DC line in QSPI mode
    io_cfg.spi_mode          = 0;
    io_cfg.pclk_hz           = BOARD_LCD_QSPI_HZ;
    io_cfg.trans_queue_depth = 10;
    io_cfg.lcd_cmd_bits      = 32;   // 4-byte RM67162 QSPI prefix
    io_cfg.lcd_param_bits    = 8;
    io_cfg.flags.quad_mode   = true; // data lines in QSPI
    ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi(
        (esp_lcd_spi_bus_handle_t)BOARD_LCD_QSPI_HOST, &io_cfg, &io));

    // ── RM67162 panel
    ESP_ERROR_CHECK(new_rm67162_panel(io, out_panel));
    ESP_ERROR_CHECK(esp_lcd_panel_reset(*out_panel));
    ESP_ERROR_CHECK(esp_lcd_panel_init(*out_panel));
    ESP_LOGI(TAG, "LCD ready (%dx%d)", BOARD_LCD_W, BOARD_LCD_H);

    // ── I2C bus for touch
    i2c_config_t i2c_cfg = {};
    i2c_cfg.mode             = I2C_MODE_MASTER;
    i2c_cfg.sda_io_num       = BOARD_TOUCH_SDA;
    i2c_cfg.scl_io_num       = BOARD_TOUCH_SCL;
    i2c_cfg.sda_pullup_en    = GPIO_PULLUP_ENABLE;
    i2c_cfg.scl_pullup_en    = GPIO_PULLUP_ENABLE;
    i2c_cfg.master.clk_speed = BOARD_TOUCH_I2C_HZ;
    ESP_ERROR_CHECK(i2c_param_config(BOARD_I2C_HOST, &i2c_cfg));
    ESP_ERROR_CHECK(i2c_driver_install(BOARD_I2C_HOST, I2C_MODE_MASTER, 0, 0, 0));

    // ── CST816S touch
    esp_lcd_panel_io_handle_t touch_io;
    esp_lcd_panel_io_i2c_config_t touch_io_cfg = {};
    touch_io_cfg.dev_addr       = ESP_LCD_TOUCH_IO_I2C_CST816S_ADDRESS;
    touch_io_cfg.control_phase_bytes = 1;
    touch_io_cfg.dc_bit_offset  = 0;
    touch_io_cfg.lcd_cmd_bits   = 8;
    touch_io_cfg.lcd_param_bits = 8;
    touch_io_cfg.flags.disable_control_phase = true;
    ESP_ERROR_CHECK(esp_lcd_new_panel_io_i2c(
        (esp_lcd_i2c_bus_handle_t)BOARD_I2C_HOST, &touch_io_cfg, &touch_io));

    esp_lcd_touch_config_t touch_cfg = {};
    touch_cfg.x_max        = BOARD_LCD_W;
    touch_cfg.y_max        = BOARD_LCD_H;
    touch_cfg.rst_gpio_num = BOARD_TOUCH_RST;
    touch_cfg.int_gpio_num = BOARD_TOUCH_INT;
    touch_cfg.levels.reset = 0;
    touch_cfg.flags.swap_xy  = false;
    touch_cfg.flags.mirror_x = false;
    touch_cfg.flags.mirror_y = false;
    ESP_ERROR_CHECK(esp_lcd_touch_new_i2c_cst816s(touch_io, &touch_cfg, out_touch));
    ESP_LOGI(TAG, "touch ready (CST816S)");

    return ESP_OK;
}
