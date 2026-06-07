#pragma once

#include "config.h"
#include <stdbool.h>

// Callbacks fired from the WebSocket task — keep handlers short.
typedef struct {
    void (*on_connected)(void);
    void (*on_disconnected)(void);
    // Scene list received after initial connect or after SceneListChanged event.
    // `names` is only valid for the duration of the callback.
    void (*on_scene_list)(const char **names, int count, const char *active);
    // Active scene switched (from OBS or from this device).
    void (*on_scene_changed)(const char *scene_name);
    // Stream / recording output state.
    void (*on_stream_state)(bool live, bool recording);
} obs_callbacks_t;

// Register UI callbacks. Call before obs_client_start.
void obs_client_set_callbacks(const obs_callbacks_t *cb);

// Connect (or reconnect) using config from NVS.
void obs_client_start(const app_config_t *cfg);

// Disconnect and destroy the WebSocket client.
void obs_client_stop(void);

// --- Requests (no-ops if not yet identified) ---

void obs_client_set_scene(const char *scene_name);
void obs_client_toggle_stream(void);
void obs_client_toggle_recording(void);
