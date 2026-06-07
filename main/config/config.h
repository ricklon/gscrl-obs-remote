#pragma once

#include <stdbool.h>

#define OBS_DEFAULT_PORT 4455
#define NVS_NAMESPACE    "obs_remote"

typedef struct {
    char wifi_ssid[64];
    char wifi_pass[64];
    char obs_host[64];
    int  obs_port;
    char obs_pass[128];
} app_config_t;

// Load config from NVS into dest. Returns false if any required field is missing.
bool config_load(app_config_t *dest);

// Persist config to NVS.
void config_save(const app_config_t *cfg);

// Wipe all keys in the NVS namespace.
void config_clear(void);

// Start the serial command listener task.
// Calls config_on_change() when credentials are updated.
void config_start_serial_task(void (*on_change)(const app_config_t *));

// Print current config to console (password masked).
void config_print(const app_config_t *cfg);
