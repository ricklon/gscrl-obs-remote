#pragma once

#include "esp_lcd_panel_ops.h"
#include "esp_lcd_touch.h"

// ── Pin definitions (Waveshare ESP32-S3-Touch-AMOLED-1.8") ────────────────
// Adjust these for other board variants.

// RM67162 AMOLED — QSPI
#define BOARD_LCD_QSPI_HOST   SPI2_HOST
#define BOARD_LCD_CS          GPIO_NUM_6
#define BOARD_LCD_CLK         GPIO_NUM_47
#define BOARD_LCD_D0          GPIO_NUM_18
#define BOARD_LCD_D1          GPIO_NUM_7
#define BOARD_LCD_D2          GPIO_NUM_48
#define BOARD_LCD_D3          GPIO_NUM_5
#define BOARD_LCD_RST         GPIO_NUM_17
#define BOARD_LCD_TE          GPIO_NUM_9   // tearing-effect pin (optional)
#define BOARD_LCD_QSPI_HZ     (80 * 1000 * 1000)

// CST816S capacitive touch — I2C
#define BOARD_I2C_HOST        I2C_NUM_0
#define BOARD_TOUCH_SDA       GPIO_NUM_3
#define BOARD_TOUCH_SCL       GPIO_NUM_2
#define BOARD_TOUCH_INT       GPIO_NUM_21
#define BOARD_TOUCH_RST       GPIO_NUM_16
#define BOARD_TOUCH_I2C_HZ    (400 * 1000)

// Display resolution
#define BOARD_LCD_W  368
#define BOARD_LCD_H  448

// Initialize display and touch. Must be called before LVGL init.
esp_err_t board_init(esp_lcd_panel_handle_t *out_panel,
                     esp_lcd_touch_handle_t  *out_touch);
