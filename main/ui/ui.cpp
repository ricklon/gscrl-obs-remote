#include "ui.h"
#include "board.h"

#include <string.h>
#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_touch.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "lvgl.h"

static const char *TAG = "ui";

// ── Constants ──────────────────────────────────────────────────────────────

#define LCD_W            BOARD_LCD_W
#define LCD_H            BOARD_LCD_H
#define BUF_LINES        40            // lines per draw buffer
#define LVGL_TASK_STACK  8192
#define LVGL_TASK_PRIO   4
#define SCENE_MAX        24
#define SCENE_NAME_MAX   64
#define STATUS_BAR_H     48
#define ACTION_BAR_H     60
#define SCENE_BTN_H      68
#define SCENE_BTN_PAD    6

// Colour palette — matches the web flasher dark theme
#define C_BG       lv_color_hex(0x0f1117)
#define C_SURFACE  lv_color_hex(0x1a1d27)
#define C_BORDER   lv_color_hex(0x2a2d3a)
#define C_ACCENT   lv_color_hex(0x7c6af7)
#define C_ACCENT2  lv_color_hex(0x5de0c5)
#define C_DANGER   lv_color_hex(0xf75e5e)
#define C_TEXT     lv_color_hex(0xe2e4ef)
#define C_MUTED    lv_color_hex(0x6b7080)
#define C_WHITE    lv_color_hex(0xffffff)
#define C_BLACK    lv_color_hex(0x000000)

// ── State ──────────────────────────────────────────────────────────────────

static SemaphoreHandle_t    s_mutex;
static esp_lcd_panel_handle_t s_panel;
static esp_lcd_touch_handle_t s_touch;
static lv_display_t        *s_disp;
static ui_action_callbacks_t s_actions;

// Scene storage (stable memory for LVGL event user_data pointers)
static char      s_scene_names[SCENE_MAX][SCENE_NAME_MAX];
static int       s_scene_count  = 0;
static char      s_active_scene[SCENE_NAME_MAX];

// Widgets we update dynamically
static lv_obj_t *s_screen_main;
static lv_obj_t *s_screen_setup;
static lv_obj_t *s_screen_connecting;

static lv_obj_t *s_scene_list;      // scrollable container
static lv_obj_t *s_scene_btns[SCENE_MAX];

static lv_obj_t *s_live_dot;
static lv_obj_t *s_live_label;
static lv_obj_t *s_rec_dot;
static lv_obj_t *s_rec_label;
static lv_obj_t *s_stream_btn;
static lv_obj_t *s_stream_label;
static lv_obj_t *s_rec_btn;
static lv_obj_t *s_rec_btn_label;
static lv_obj_t *s_conn_dot;

static bool s_stream_live = false;
static bool s_recording   = false;

// ── Mutex helpers ──────────────────────────────────────────────────────────

static inline void ui_lock(void)   { xSemaphoreTake(s_mutex, portMAX_DELAY); }
static inline void ui_unlock(void) { xSemaphoreGive(s_mutex); }

// ── LVGL display flush ─────────────────────────────────────────────────────

static void flush_cb(lv_display_t *disp, const lv_area_t *area, uint8_t *px_map)
{
    esp_lcd_panel_draw_bitmap(s_panel,
                               area->x1, area->y1,
                               area->x2 + 1, area->y2 + 1,
                               px_map);
    lv_display_flush_ready(disp);
}

// ── LVGL touch read ────────────────────────────────────────────────────────

static void touch_read_cb(lv_indev_t *indev, lv_indev_data_t *data)
{
    uint16_t x, y, strength;
    uint8_t  count = 0;
    esp_lcd_touch_read_data(s_touch);
    bool touched = esp_lcd_touch_get_coordinates(s_touch, &x, &y, &strength, &count, 1)
                   && count > 0;
    if (touched) {
        data->point.x = (lv_coord_t)x;
        data->point.y = (lv_coord_t)y;
        data->state   = LV_INDEV_STATE_PRESSED;
    } else {
        data->state = LV_INDEV_STATE_RELEASED;
    }
}

// ── LVGL tick timer ────────────────────────────────────────────────────────

static void lvgl_tick_cb(void *arg) { lv_tick_inc(1); }

// ── LVGL task ─────────────────────────────────────────────────────────────

static void lvgl_task(void *arg)
{
    while (1) {
        ui_lock();
        uint32_t delay_ms = lv_timer_handler();
        ui_unlock();
        vTaskDelay(pdMS_TO_TICKS(delay_ms > 0 && delay_ms < 50 ? delay_ms : 5));
    }
}

// ── Style helpers ──────────────────────────────────────────────────────────

static void style_plain(lv_obj_t *obj, lv_color_t bg, lv_color_t border, int radius)
{
    lv_obj_set_style_bg_color(obj, bg, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(obj, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_color(obj, border, LV_PART_MAIN);
    lv_obj_set_style_border_width(obj, !lv_color_eq(border, C_BG) ? 1 : 0, LV_PART_MAIN);
    lv_obj_set_style_radius(obj, radius, LV_PART_MAIN);
    lv_obj_set_style_pad_all(obj, 0, LV_PART_MAIN);
}

static lv_obj_t *make_label(lv_obj_t *parent, const char *text,
                              const lv_font_t *font, lv_color_t color)
{
    lv_obj_t *l = lv_label_create(parent);
    lv_label_set_text(l, text);
    lv_obj_set_style_text_font(l, font, LV_PART_MAIN);
    lv_obj_set_style_text_color(l, color, LV_PART_MAIN);
    return l;
}

static lv_obj_t *make_dot(lv_obj_t *parent, lv_color_t color, int size)
{
    lv_obj_t *d = lv_obj_create(parent);
    lv_obj_set_size(d, size, size);
    style_plain(d, color, C_BG, size / 2);
    return d;
}

// ── Event handlers ─────────────────────────────────────────────────────────

static void scene_btn_clicked(lv_event_t *e)
{
    const char *name = (const char *)lv_event_get_user_data(e);
    if (s_actions.on_scene_select && name) s_actions.on_scene_select(name);
}

static void stream_btn_clicked(lv_event_t *e)
{
    if (s_actions.on_toggle_stream) s_actions.on_toggle_stream();
}

static void rec_btn_clicked(lv_event_t *e)
{
    if (s_actions.on_toggle_recording) s_actions.on_toggle_recording();
}

// ── Status indicator update (internal, lock already held) ─────────────────

static void apply_indicator(lv_obj_t *dot, lv_obj_t *label,
                              bool active, lv_color_t on_color,
                              const char *text_on, const char *text_off)
{
    lv_color_t col = active ? on_color : C_MUTED;
    lv_obj_set_style_bg_color(dot, col, LV_PART_MAIN);
    lv_obj_set_style_text_color(label, col, LV_PART_MAIN);
    lv_label_set_text(label, active ? text_on : text_off);
}

static void apply_action_btn(lv_obj_t *btn, lv_obj_t *label,
                               bool active, lv_color_t on_color,
                               const char *text)
{
    lv_color_t bg = active ? on_color : C_SURFACE;
    lv_obj_set_style_bg_color(btn, bg, LV_PART_MAIN);
    lv_obj_set_style_text_color(label, active ? C_WHITE : C_MUTED, LV_PART_MAIN);
    lv_label_set_text(label, text);
}

// ── Build: main stream-deck screen ────────────────────────────────────────

static void build_main_screen(void)
{
    lv_obj_t *scr = lv_obj_create(NULL);
    style_plain(scr, C_BG, C_BG, 0);
    lv_obj_set_size(scr, LCD_W, LCD_H);
    s_screen_main = scr;

    // ── Status bar ─────────────────────────────────────────────────────────
    lv_obj_t *bar = lv_obj_create(scr);
    lv_obj_set_size(bar, LCD_W, STATUS_BAR_H);
    lv_obj_align(bar, LV_ALIGN_TOP_LEFT, 0, 0);
    style_plain(bar, C_SURFACE, C_BORDER, 0);
    lv_obj_set_style_border_side(bar, LV_BORDER_SIDE_BOTTOM, LV_PART_MAIN);
    lv_obj_set_style_border_width(bar, 1, LV_PART_MAIN);
    lv_obj_set_style_pad_left(bar,  12, LV_PART_MAIN);
    lv_obj_set_style_pad_right(bar, 12, LV_PART_MAIN);

    // LIVE indicator
    s_live_dot = make_dot(bar, C_MUTED, 10);
    lv_obj_align(s_live_dot, LV_ALIGN_LEFT_MID, 0, 0);

    s_live_label = make_label(bar, "LIVE", &lv_font_montserrat_14, C_MUTED);
    lv_obj_align_to(s_live_label, s_live_dot, LV_ALIGN_OUT_RIGHT_MID, 6, 0);

    // REC indicator
    s_rec_dot = make_dot(bar, C_MUTED, 10);
    lv_obj_align_to(s_rec_dot, s_live_label, LV_ALIGN_OUT_RIGHT_MID, 16, 0);

    s_rec_label = make_label(bar, "REC", &lv_font_montserrat_14, C_MUTED);
    lv_obj_align_to(s_rec_label, s_rec_dot, LV_ALIGN_OUT_RIGHT_MID, 6, 0);

    // Title — centred
    lv_obj_t *title = make_label(bar, "OBS Remote", &lv_font_montserrat_14, C_MUTED);
    lv_obj_align(title, LV_ALIGN_CENTER, 0, 0);

    // Connection dot (right side)
    s_conn_dot = make_dot(bar, C_ACCENT2, 8);
    lv_obj_align(s_conn_dot, LV_ALIGN_RIGHT_MID, 0, 0);

    // ── Scene list (scrollable, fills between bars) ────────────────────────
    int list_y = STATUS_BAR_H;
    int list_h = LCD_H - STATUS_BAR_H - ACTION_BAR_H;

    s_scene_list = lv_obj_create(scr);
    lv_obj_set_pos(s_scene_list, 0, list_y);
    lv_obj_set_size(s_scene_list, LCD_W, list_h);
    style_plain(s_scene_list, C_BG, C_BG, 0);
    lv_obj_set_style_pad_ver(s_scene_list, SCENE_BTN_PAD, LV_PART_MAIN);
    lv_obj_set_style_pad_hor(s_scene_list, SCENE_BTN_PAD, LV_PART_MAIN);
    lv_obj_set_scroll_dir(s_scene_list, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(s_scene_list, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_flex_flow(s_scene_list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(s_scene_list,
                           LV_FLEX_ALIGN_START,
                           LV_FLEX_ALIGN_CENTER,
                           LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(s_scene_list, SCENE_BTN_PAD, LV_PART_MAIN);

    // ── Action bar (bottom) ────────────────────────────────────────────────
    lv_obj_t *abar = lv_obj_create(scr);
    lv_obj_set_size(abar, LCD_W, ACTION_BAR_H);
    lv_obj_align(abar, LV_ALIGN_BOTTOM_LEFT, 0, 0);
    style_plain(abar, C_SURFACE, C_BORDER, 0);
    lv_obj_set_style_border_side(abar, LV_BORDER_SIDE_TOP, LV_PART_MAIN);
    lv_obj_set_style_border_width(abar, 1, LV_PART_MAIN);
    lv_obj_set_style_pad_all(abar, 8, LV_PART_MAIN);
    lv_obj_set_flex_flow(abar, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(abar,
                           LV_FLEX_ALIGN_SPACE_EVENLY,
                           LV_FLEX_ALIGN_CENTER,
                           LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(abar, 8, LV_PART_MAIN);

    // Stream button
    int btn_w = (LCD_W - 32) / 2;
    int btn_h = ACTION_BAR_H - 16;

    s_stream_btn = lv_obj_create(abar);
    lv_obj_set_size(s_stream_btn, btn_w, btn_h);
    style_plain(s_stream_btn, C_SURFACE, C_BORDER, 8);
    lv_obj_add_event_cb(s_stream_btn, stream_btn_clicked, LV_EVENT_CLICKED, NULL);

    s_stream_label = make_label(s_stream_btn, "STREAM", &lv_font_montserrat_14, C_MUTED);
    lv_obj_center(s_stream_label);

    // Record button
    s_rec_btn = lv_obj_create(abar);
    lv_obj_set_size(s_rec_btn, btn_w, btn_h);
    style_plain(s_rec_btn, C_SURFACE, C_BORDER, 8);
    lv_obj_add_event_cb(s_rec_btn, rec_btn_clicked, LV_EVENT_CLICKED, NULL);

    s_rec_btn_label = make_label(s_rec_btn, "REC", &lv_font_montserrat_14, C_MUTED);
    lv_obj_center(s_rec_btn_label);
}

// ── Build: setup / connecting screens ─────────────────────────────────────

static lv_obj_t *build_info_screen(const char *title, const char *body)
{
    lv_obj_t *scr = lv_obj_create(NULL);
    style_plain(scr, C_BG, C_BG, 0);
    lv_obj_set_size(scr, LCD_W, LCD_H);

    lv_obj_t *icon = lv_label_create(scr);
    lv_label_set_text(icon, LV_SYMBOL_VIDEO);
    lv_obj_set_style_text_font(icon, &lv_font_montserrat_20, LV_PART_MAIN);
    lv_obj_set_style_text_color(icon, C_ACCENT, LV_PART_MAIN);
    lv_obj_align(icon, LV_ALIGN_CENTER, 0, -40);

    lv_obj_t *t = make_label(scr, title, &lv_font_montserrat_20, C_TEXT);
    lv_obj_align(t, LV_ALIGN_CENTER, 0, -8);

    lv_obj_t *b = make_label(scr, body, &lv_font_montserrat_14, C_MUTED);
    lv_obj_set_style_text_align(b, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    lv_label_set_long_mode(b, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(b, LCD_W - 32);
    lv_obj_align(b, LV_ALIGN_CENTER, 0, 32);

    return scr;
}

// ── Internal: rebuild scene buttons (call with lock held) ─────────────────

static void rebuild_scene_buttons(void)
{
    // Remove old buttons
    lv_obj_clean(s_scene_list);
    memset(s_scene_btns, 0, sizeof(s_scene_btns));

    int btn_w = LCD_W - SCENE_BTN_PAD * 2;

    for (int i = 0; i < s_scene_count; i++) {
        bool is_active = strcmp(s_scene_names[i], s_active_scene) == 0;

        lv_obj_t *btn = lv_obj_create(s_scene_list);
        lv_obj_set_size(btn, btn_w, SCENE_BTN_H);
        lv_obj_set_style_pad_all(btn, 0, LV_PART_MAIN);
        lv_obj_set_style_radius(btn, 10, LV_PART_MAIN);
        lv_obj_set_style_border_width(btn, 1, LV_PART_MAIN);

        if (is_active) {
            lv_obj_set_style_bg_color(btn, C_ACCENT, LV_PART_MAIN);
            lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, LV_PART_MAIN);
            lv_obj_set_style_border_color(btn, C_ACCENT, LV_PART_MAIN);
        } else {
            lv_obj_set_style_bg_color(btn, C_SURFACE, LV_PART_MAIN);
            lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, LV_PART_MAIN);
            lv_obj_set_style_border_color(btn, C_BORDER, LV_PART_MAIN);
        }

        // Scene name label — left-aligned with padding
        lv_obj_t *label = make_label(btn, s_scene_names[i],
                                      &lv_font_montserrat_16,
                                      is_active ? C_WHITE : C_TEXT);
        lv_obj_align(label, LV_ALIGN_LEFT_MID, 16, 0);
        lv_label_set_long_mode(label, LV_LABEL_LONG_DOT);
        lv_obj_set_width(label, btn_w - 32);

        // Active marker (right side)
        if (is_active) {
            lv_obj_t *mark = make_label(btn, LV_SYMBOL_OK,
                                         &lv_font_montserrat_14, C_WHITE);
            lv_obj_align(mark, LV_ALIGN_RIGHT_MID, -16, 0);
        }

        lv_obj_add_event_cb(btn, scene_btn_clicked, LV_EVENT_CLICKED,
                             (void *)s_scene_names[i]);

        // Pressed state feedback
        lv_obj_set_style_bg_color(btn, is_active ? C_ACCENT : C_BORDER,
                                   LV_STATE_PRESSED);

        s_scene_btns[i] = btn;
    }
}

// ── Public API ─────────────────────────────────────────────────────────────

void ui_set_action_callbacks(const ui_action_callbacks_t *cb)
{
    s_actions = *cb;
}

void ui_init(void)
{
    s_mutex = xSemaphoreCreateMutex();

    // Hardware: display + touch
    ESP_ERROR_CHECK(board_init(&s_panel, &s_touch));

    // LVGL init
    lv_init();

    // Draw buffers — allocate in PSRAM when available
    size_t buf_sz = (size_t)LCD_W * BUF_LINES * sizeof(lv_color_t);
    void *buf1 = heap_caps_malloc(buf_sz, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    void *buf2 = heap_caps_malloc(buf_sz, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!buf1 || !buf2) {
        // Fall back to internal heap
        free(buf1); free(buf2);
        buf1 = heap_caps_malloc(buf_sz, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
        buf2 = heap_caps_malloc(buf_sz, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
        ESP_LOGW(TAG, "using internal RAM for draw buffers");
    }
    assert(buf1 && buf2);

    // Create LVGL display
    s_disp = lv_display_create(LCD_W, LCD_H);
    lv_display_set_flush_cb(s_disp, flush_cb);
    lv_display_set_buffers(s_disp, buf1, buf2, buf_sz, LV_DISPLAY_RENDER_MODE_PARTIAL);

    // Create touch input device
    lv_indev_t *indev = lv_indev_create();
    lv_indev_set_type(indev, LV_INDEV_TYPE_POINTER);
    lv_indev_set_read_cb(indev, touch_read_cb);

    // 1ms tick timer
    static esp_timer_handle_t tick_timer;
    esp_timer_create_args_t ta = {
        .callback = lvgl_tick_cb,
        .name     = "lvgl_tick",
    };
    ESP_ERROR_CHECK(esp_timer_create(&ta, &tick_timer));
    ESP_ERROR_CHECK(esp_timer_start_periodic(tick_timer, 1000)); // 1ms

    // Build all screens while we have the lock
    ui_lock();
    build_main_screen();
    s_screen_setup      = build_info_screen("OBS Remote",
                                             "Connect via USB serial\nand run !wifi / !server");
    s_screen_connecting = build_info_screen("Connecting...",
                                             "Waiting for OBS\nWebSocket");
    ui_unlock();

    // LVGL task
    xTaskCreate(lvgl_task, "lvgl", LVGL_TASK_STACK, NULL, LVGL_TASK_PRIO, NULL);
    ESP_LOGI(TAG, "UI initialised");
}

void ui_show_setup_screen(void)
{
    ui_lock();
    lv_screen_load(s_screen_setup);
    ui_unlock();
}

void ui_show_connecting_screen(void)
{
    ui_lock();
    // Grey out the connection dot on the main screen too
    if (s_conn_dot) lv_obj_set_style_bg_color(s_conn_dot, C_MUTED, LV_PART_MAIN);
    lv_screen_load(s_screen_connecting);
    ui_unlock();
}

void ui_set_scenes(const char **names, int count, const char *active)
{
    ui_lock();

    s_scene_count = count < SCENE_MAX ? count : SCENE_MAX;
    for (int i = 0; i < s_scene_count; i++) {
        strlcpy(s_scene_names[i], names[i] ? names[i] : "", SCENE_NAME_MAX);
    }
    if (active) strlcpy(s_active_scene, active, SCENE_NAME_MAX);

    rebuild_scene_buttons();

    // Switch to main screen (covers connecting screen)
    lv_screen_load(s_screen_main);
    if (s_conn_dot) lv_obj_set_style_bg_color(s_conn_dot, C_ACCENT2, LV_PART_MAIN);

    ui_unlock();
}

void ui_set_active_scene(const char *name)
{
    if (!name) return;
    ui_lock();
    strlcpy(s_active_scene, name, SCENE_NAME_MAX);
    rebuild_scene_buttons();
    ui_unlock();
}

void ui_set_stream_state(bool live, bool recording)
{
    ui_lock();
    s_stream_live = live;
    s_recording   = recording;

    if (s_live_dot) {
        apply_indicator(s_live_dot, s_live_label, live, C_DANGER, "LIVE", "LIVE");
    }
    if (s_rec_dot) {
        apply_indicator(s_rec_dot, s_rec_label, recording, C_DANGER, "REC", "REC");
    }
    if (s_stream_btn) {
        apply_action_btn(s_stream_btn, s_stream_label, live, C_DANGER,
                          live ? LV_SYMBOL_STOP " STOP" : LV_SYMBOL_PLAY " STREAM");
    }
    if (s_rec_btn) {
        apply_action_btn(s_rec_btn, s_rec_btn_label, recording, C_DANGER,
                          recording ? LV_SYMBOL_STOP " STOP" : LV_SYMBOL_EYE_OPEN " REC");
    }

    ui_unlock();
}
