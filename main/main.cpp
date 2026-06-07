#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs_flash.h"
#include "esp_log.h"

#include "config.h"
#include "obs_client.h"
#include "ui.h"

static const char *TAG = "main";

// ── obs_client → ui ────────────────────────────────────────────────────────

static void on_obs_connected(void)    { /* scene list arrives next */ }

static void on_obs_disconnected(void) { ui_show_connecting_screen(); }

static void on_scene_list(const char **names, int count, const char *active)
{
    ui_set_scenes(names, count, active);
}

static void on_scene_changed(const char *name) { ui_set_active_scene(name); }

static void on_stream_state(bool live, bool recording)
{
    ui_set_stream_state(live, recording);
}

// ── config change (serial !server / !wifi) ─────────────────────────────────

static void on_config_change(const app_config_t *cfg)
{
    ESP_LOGI(TAG, "config updated — reconnecting");
    ui_show_connecting_screen();
    obs_client_stop();
    obs_client_start(cfg);
}

// ── app_main ───────────────────────────────────────────────────────────────

extern "C" void app_main(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ESP_ERROR_CHECK(nvs_flash_init());
    }

    // Action callbacks: UI button presses → OBS requests
    static const ui_action_callbacks_t ui_actions = {
        .on_scene_select     = obs_client_set_scene,
        .on_toggle_stream    = obs_client_toggle_stream,
        .on_toggle_recording = obs_client_toggle_recording,
    };
    ui_set_action_callbacks(&ui_actions);

    // Starts LVGL, display driver, touch driver
    ui_init();

    // OBS event callbacks: OBS state → UI updates
    static const obs_callbacks_t obs_cb = {
        .on_connected     = on_obs_connected,
        .on_disconnected  = on_obs_disconnected,
        .on_scene_list    = on_scene_list,
        .on_scene_changed = on_scene_changed,
        .on_stream_state  = on_stream_state,
    };
    obs_client_set_callbacks(&obs_cb);

    // Serial command listener for !wifi / !server — always running
    config_start_serial_task(on_config_change);

    app_config_t cfg;
    if (!config_load(&cfg)) {
        ESP_LOGW(TAG, "no config — use !wifi / !server via serial");
        ui_show_setup_screen();
        return;
    }

    ui_show_connecting_screen();
    obs_client_start(&cfg);
}
