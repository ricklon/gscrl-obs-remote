#include "config.h"

#include <string.h>
#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "esp_log.h"
#include "driver/uart.h"

static const char *TAG = "config";
static void (*s_on_change)(const app_config_t *) = nullptr;

// ---------------------------------------------------------------------------
// NVS helpers
// ---------------------------------------------------------------------------

static void nvs_get(nvs_handle_t h, const char *key, char *out, size_t len) {
    size_t sz = len;
    if (nvs_get_str(h, key, out, &sz) != ESP_OK)
        out[0] = '\0';
}

bool config_load(app_config_t *dest) {
    nvs_handle_t h;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &h) != ESP_OK) {
        memset(dest, 0, sizeof(*dest));
        dest->obs_port = OBS_DEFAULT_PORT;
        return false;
    }

    nvs_get(h, "wifi_ssid", dest->wifi_ssid, sizeof(dest->wifi_ssid));
    nvs_get(h, "wifi_pass", dest->wifi_pass, sizeof(dest->wifi_pass));
    nvs_get(h, "obs_host",  dest->obs_host,  sizeof(dest->obs_host));
    nvs_get(h, "obs_pass",  dest->obs_pass,  sizeof(dest->obs_pass));

    int32_t port = OBS_DEFAULT_PORT;
    nvs_get_i32(h, "obs_port", &port);
    dest->obs_port = (int)port;

    nvs_close(h);

    return dest->wifi_ssid[0] && dest->obs_host[0] && dest->obs_pass[0];
}

void config_save(const app_config_t *cfg) {
    nvs_handle_t h;
    ESP_ERROR_CHECK(nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h));
    nvs_set_str(h, "wifi_ssid", cfg->wifi_ssid);
    nvs_set_str(h, "wifi_pass", cfg->wifi_pass);
    nvs_set_str(h, "obs_host",  cfg->obs_host);
    nvs_set_str(h, "obs_pass",  cfg->obs_pass);
    nvs_set_i32(h, "obs_port",  (int32_t)cfg->obs_port);
    nvs_commit(h);
    nvs_close(h);
    ESP_LOGI(TAG, "config saved");
}

void config_clear(void) {
    nvs_handle_t h;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h) == ESP_OK) {
        nvs_erase_all(h);
        nvs_commit(h);
        nvs_close(h);
    }
    ESP_LOGI(TAG, "config cleared");
}

void config_print(const app_config_t *cfg) {
    printf("  wifi_ssid : %s\n",  cfg->wifi_ssid[0] ? cfg->wifi_ssid : "(unset)");
    printf("  wifi_pass : %s\n",  cfg->wifi_pass[0] ? "********" : "(unset)");
    printf("  obs_host  : %s\n",  cfg->obs_host[0]  ? cfg->obs_host  : "(unset)");
    printf("  obs_port  : %d\n",  cfg->obs_port);
    printf("  obs_pass  : %s\n",  cfg->obs_pass[0]  ? "********" : "(unset)");
}

// ---------------------------------------------------------------------------
// Serial command parser
//
// Commands (newline-terminated, sent via USB-CDC / UART0):
//
//   !wifi <ssid> <password>
//   !server <host> <password>
//   !server <host> <port> <password>
//   !status
//   !clear
//   !help
// ---------------------------------------------------------------------------

static void parse_command(const char *line, app_config_t *cfg) {
    char cmd[16], a[128], b[128], c[128];
    int n = sscanf(line, "%15s %127s %127s %127s", cmd, a, b, c);

    if (strcmp(cmd, "!wifi") == 0 && n >= 3) {
        strlcpy(cfg->wifi_ssid, a, sizeof(cfg->wifi_ssid));
        strlcpy(cfg->wifi_pass, b, sizeof(cfg->wifi_pass));
        config_save(cfg);
        printf("OK: wifi updated\n");
        if (s_on_change) s_on_change(cfg);

    } else if (strcmp(cmd, "!server") == 0 && n == 3) {
        // !server <host> <password>  (port stays as-is or default)
        strlcpy(cfg->obs_host, a, sizeof(cfg->obs_host));
        strlcpy(cfg->obs_pass, b, sizeof(cfg->obs_pass));
        config_save(cfg);
        printf("OK: server updated (port %d)\n", cfg->obs_port);
        if (s_on_change) s_on_change(cfg);

    } else if (strcmp(cmd, "!server") == 0 && n == 4) {
        // !server <host> <port> <password>
        strlcpy(cfg->obs_host, a, sizeof(cfg->obs_host));
        cfg->obs_port = atoi(b);
        strlcpy(cfg->obs_pass, c, sizeof(cfg->obs_pass));
        config_save(cfg);
        printf("OK: server updated\n");
        if (s_on_change) s_on_change(cfg);

    } else if (strcmp(cmd, "!status") == 0) {
        config_print(cfg);

    } else if (strcmp(cmd, "!clear") == 0) {
        config_clear(cfg);
        memset(cfg, 0, sizeof(*cfg));
        cfg->obs_port = OBS_DEFAULT_PORT;
        printf("OK: config cleared\n");

    } else if (strcmp(cmd, "!help") == 0) {
        printf("Commands:\n");
        printf("  !wifi <ssid> <password>\n");
        printf("  !server <host> <password>\n");
        printf("  !server <host> <port> <password>\n");
        printf("  !status\n");
        printf("  !clear\n");

    } else {
        printf("ERR: unknown command (try !help)\n");
    }
}

static void serial_task(void *arg) {
    app_config_t cfg;
    config_load(&cfg);

    char line[256];
    int  pos = 0;

    printf("\nOBS Remote ready. Type !help for commands.\n");

    while (true) {
        uint8_t ch;
        int len = uart_read_bytes(UART_NUM_0, &ch, 1, pdMS_TO_TICKS(20));
        if (len <= 0) continue;

        if (ch == '\r') continue;

        if (ch == '\n') {
            line[pos] = '\0';
            pos = 0;
            if (line[0] == '!') {
                parse_command(line, &cfg);
            }
        } else if (pos < (int)sizeof(line) - 1) {
            line[pos++] = (char)ch;
        }
    }
}

void config_start_serial_task(void (*on_change)(const app_config_t *)) {
    s_on_change = on_change;

    uart_config_t uart_cfg = {
        .baud_rate  = 115200,
        .data_bits  = UART_DATA_8_BITS,
        .parity     = UART_PARITY_DISABLE,
        .stop_bits  = UART_STOP_BITS_1,
        .flow_ctrl  = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };
    uart_driver_install(UART_NUM_0, 512, 0, 0, nullptr, 0);
    uart_param_config(UART_NUM_0, &uart_cfg);

    xTaskCreate(serial_task, "serial_cmd", 4096, nullptr, 5, nullptr);
}
