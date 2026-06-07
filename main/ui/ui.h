#pragma once
#include <stdbool.h>

// Action callbacks — wired up by main.cpp to obs_client functions.
// Keeps ui.cpp independent of obs_client.cpp.
typedef struct {
    void (*on_scene_select)(const char *scene_name);
    void (*on_toggle_stream)(void);
    void (*on_toggle_recording)(void);
} ui_action_callbacks_t;

// Call once at startup, before ui_init.
void ui_set_action_callbacks(const ui_action_callbacks_t *cb);

// Initialize LVGL, display driver and touch. Starts the LVGL task.
void ui_init(void);

// Screens
void ui_show_setup_screen(void);
void ui_show_connecting_screen(void);

// Called by obs_client callbacks — thread-safe (takes internal mutex).
void ui_set_scenes(const char **names, int count, const char *active);
void ui_set_active_scene(const char *scene_name);
void ui_set_stream_state(bool live, bool recording);
