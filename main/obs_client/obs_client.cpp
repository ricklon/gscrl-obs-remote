#include "obs_client.h"

#include <string.h>
#include <stdio.h>
#include <stdlib.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_websocket_client.h"
#include "esp_log.h"
#include "cJSON.h"
#include "mbedtls/sha256.h"
#include "mbedtls/base64.h"

static const char *TAG = "obs_client";

// ---------------------------------------------------------------------------
// obs-websocket v5 protocol constants
// ---------------------------------------------------------------------------

// OpCodes
#define OP_HELLO       0   // server → client: version + auth challenge
#define OP_IDENTIFY    1   // client → server: auth response
#define OP_IDENTIFIED  2   // server → client: auth accepted
#define OP_EVENT       5   // server → client: OBS event
#define OP_REQUEST     6   // client → server: request
#define OP_RESPONSE    7   // server → client: response to a request

// EventSubscription bitmask (what events we want pushed to us)
// Scenes  (1<<2): CurrentProgramSceneChanged, SceneListChanged, SceneNameChanged
// Outputs (1<<6): StreamStateChanged, RecordStateChanged
#define EVENT_SUBS ((1 << 2) | (1 << 6))

// ---------------------------------------------------------------------------
// State
// ---------------------------------------------------------------------------

typedef enum {
    STATE_DISCONNECTED,
    STATE_CONNECTED,    // TCP up, waiting for Hello
    STATE_IDENTIFIED,   // auth complete, ready
} obs_state_t;

static struct {
    esp_websocket_client_handle_t client;
    obs_state_t     state;
    obs_callbacks_t cb;
    uint32_t        req_id;
    char            pass[128];
    bool            stream_live;
    bool            recording;
    // Accumulate fragmented WebSocket frames into one buffer before parsing
    char            msg_buf[8192];
} s;

// ---------------------------------------------------------------------------
// Authentication — obs-websocket v5 spec:
//   secret = base64( sha256( password + salt ) )
//   auth   = base64( sha256( secret + challenge ) )
// ---------------------------------------------------------------------------

static void compute_auth(const char *password, const char *salt,
                          const char *challenge, char *out, size_t out_len)
{
    mbedtls_sha256_context ctx;
    uint8_t hash[32];
    char    secret[48]; // base64 of 32 bytes = 44 chars + NUL
    size_t  b64_len;

    // sha256(password + salt)
    mbedtls_sha256_init(&ctx);
    mbedtls_sha256_starts(&ctx, 0 /* is224=false */);
    mbedtls_sha256_update(&ctx, (const uint8_t *)password, strlen(password));
    mbedtls_sha256_update(&ctx, (const uint8_t *)salt,     strlen(salt));
    mbedtls_sha256_finish(&ctx, hash);
    mbedtls_sha256_free(&ctx);

    mbedtls_base64_encode((uint8_t *)secret, sizeof(secret), &b64_len, hash, 32);
    secret[b64_len] = '\0';

    // sha256(secret + challenge)
    mbedtls_sha256_init(&ctx);
    mbedtls_sha256_starts(&ctx, 0);
    mbedtls_sha256_update(&ctx, (const uint8_t *)secret,    b64_len);
    mbedtls_sha256_update(&ctx, (const uint8_t *)challenge, strlen(challenge));
    mbedtls_sha256_finish(&ctx, hash);
    mbedtls_sha256_free(&ctx);

    mbedtls_base64_encode((uint8_t *)out, out_len, &b64_len, hash, 32);
    out[b64_len] = '\0';
}

// ---------------------------------------------------------------------------
// JSON send helpers
// ---------------------------------------------------------------------------

static void send_json(cJSON *root)
{
    char *str = cJSON_PrintUnformatted(root);
    if (!str) return;
    if (s.client && s.state != STATE_DISCONNECTED) {
        int sent = esp_websocket_client_send_text(s.client, str, strlen(str),
                                                   pdMS_TO_TICKS(2000));
        if (sent < 0) ESP_LOGW(TAG, "send failed");
    }
    cJSON_free(str);
}

// Builds and sends a Request (op 6).
// `data` is optional requestData — ownership transfers to this function.
static void send_request(const char *type, cJSON *data)
{
    if (s.state != STATE_IDENTIFIED) {
        if (data) cJSON_Delete(data);
        return;
    }

    char id_str[16];
    snprintf(id_str, sizeof(id_str), "%lu", (unsigned long)++s.req_id);

    cJSON *root = cJSON_CreateObject();
    cJSON_AddNumberToObject(root, "op", OP_REQUEST);
    cJSON *d = cJSON_AddObjectToObject(root, "d");
    cJSON_AddStringToObject(d, "requestType", type);
    cJSON_AddStringToObject(d, "requestId",   id_str);
    if (data) cJSON_AddItemToObject(d, "requestData", data); // transfers ownership

    send_json(root);
    cJSON_Delete(root); // also frees data
}

// ---------------------------------------------------------------------------
// Opcode handlers
// ---------------------------------------------------------------------------

static void handle_hello(cJSON *d)
{
    // Build Identify (op 1)
    cJSON *root = cJSON_CreateObject();
    cJSON_AddNumberToObject(root, "op", OP_IDENTIFY);
    cJSON *id = cJSON_AddObjectToObject(root, "d");
    cJSON_AddNumberToObject(id, "rpcVersion",        1);
    cJSON_AddNumberToObject(id, "eventSubscriptions", EVENT_SUBS);

    // Auth is optional — server only sends the challenge when a password is set
    cJSON *auth_obj = cJSON_GetObjectItem(d, "authentication");
    if (auth_obj && s.pass[0]) {
        const char *salt      = cJSON_GetStringValue(cJSON_GetObjectItem(auth_obj, "salt"));
        const char *challenge = cJSON_GetStringValue(cJSON_GetObjectItem(auth_obj, "challenge"));
        if (salt && challenge) {
            char auth_str[64];
            compute_auth(s.pass, salt, challenge, auth_str, sizeof(auth_str));
            cJSON_AddStringToObject(id, "authentication", auth_str);
        }
    }

    send_json(root);
    cJSON_Delete(root);
    ESP_LOGI(TAG, "sent Identify");
}

static void handle_identified(void)
{
    s.state = STATE_IDENTIFIED;
    ESP_LOGI(TAG, "identified — querying initial state");

    if (s.cb.on_connected) s.cb.on_connected();

    // Pull initial state — responses handled in handle_response()
    send_request("GetSceneList",    NULL);
    send_request("GetStreamStatus", NULL);
    send_request("GetRecordStatus", NULL);
}

static void handle_scene_list_response(cJSON *rd)
{
    const char *active = cJSON_GetStringValue(
        cJSON_GetObjectItem(rd, "currentProgramSceneName"));
    cJSON *arr   = cJSON_GetObjectItem(rd, "scenes");
    int    count = cJSON_GetArraySize(arr);

    // obs returns scenes newest-first; reverse so index 0 = first created
    const char **names = (const char **)malloc(count * sizeof(char *));
    if (!names) return;
    for (int i = 0; i < count; i++) {
        cJSON *scene  = cJSON_GetArrayItem(arr, count - 1 - i);
        names[i] = cJSON_GetStringValue(cJSON_GetObjectItem(scene, "sceneName"));
    }

    if (s.cb.on_scene_list) s.cb.on_scene_list(names, count, active);
    free(names);
}

static void handle_response(cJSON *d)
{
    const char *type = cJSON_GetStringValue(cJSON_GetObjectItem(d, "requestType"));
    cJSON *status    = cJSON_GetObjectItem(d, "requestStatus");
    cJSON *rd        = cJSON_GetObjectItem(d, "responseData");
    if (!type) return;

    bool ok = cJSON_IsTrue(cJSON_GetObjectItem(status, "result"));
    if (!ok) {
        ESP_LOGW(TAG, "request '%s' failed (code %d)", type,
                 (int)cJSON_GetNumberValue(cJSON_GetObjectItem(status, "code")));
        return;
    }

    if (strcmp(type, "GetSceneList") == 0 && rd) {
        handle_scene_list_response(rd);

    } else if (strcmp(type, "GetStreamStatus") == 0 && rd) {
        s.stream_live = cJSON_IsTrue(cJSON_GetObjectItem(rd, "outputActive"));
        if (s.cb.on_stream_state) s.cb.on_stream_state(s.stream_live, s.recording);

    } else if (strcmp(type, "GetRecordStatus") == 0 && rd) {
        s.recording = cJSON_IsTrue(cJSON_GetObjectItem(rd, "outputActive"));
        if (s.cb.on_stream_state) s.cb.on_stream_state(s.stream_live, s.recording);
    }
    // ToggleStream / ToggleRecord / SetCurrentProgramScene responses need no
    // action — the matching Event arrives and drives the UI update.
}

static void handle_event(cJSON *d)
{
    const char *type = cJSON_GetStringValue(cJSON_GetObjectItem(d, "eventType"));
    cJSON *data      = cJSON_GetObjectItem(d, "eventData");
    if (!type) return;

    if (strcmp(type, "CurrentProgramSceneChanged") == 0 && data) {
        const char *name = cJSON_GetStringValue(cJSON_GetObjectItem(data, "sceneName"));
        if (name && s.cb.on_scene_changed) s.cb.on_scene_changed(name);

    } else if (strcmp(type, "SceneListChanged") == 0 ||
               strcmp(type, "SceneNameChanged")  == 0 ||
               strcmp(type, "SceneCreated")       == 0 ||
               strcmp(type, "SceneRemoved")       == 0) {
        // Re-query the full list so the display stays in sync
        send_request("GetSceneList", NULL);

    } else if (strcmp(type, "StreamStateChanged") == 0 && data) {
        s.stream_live = cJSON_IsTrue(cJSON_GetObjectItem(data, "outputActive"));
        if (s.cb.on_stream_state) s.cb.on_stream_state(s.stream_live, s.recording);

    } else if (strcmp(type, "RecordStateChanged") == 0 && data) {
        s.recording = cJSON_IsTrue(cJSON_GetObjectItem(data, "outputActive"));
        if (s.cb.on_stream_state) s.cb.on_stream_state(s.stream_live, s.recording);
    }
}

static void dispatch_message(const char *json_str)
{
    cJSON *root = cJSON_Parse(json_str);
    if (!root) {
        ESP_LOGW(TAG, "JSON parse failed");
        return;
    }

    int    op = (int)cJSON_GetNumberValue(cJSON_GetObjectItem(root, "op"));
    cJSON *d  = cJSON_GetObjectItem(root, "d");

    switch (op) {
        case OP_HELLO:      handle_hello(d);       break;
        case OP_IDENTIFIED: handle_identified();   break;
        case OP_EVENT:      handle_event(d);       break;
        case OP_RESPONSE:   handle_response(d);    break;
        default:
            ESP_LOGD(TAG, "unhandled opcode %d", op);
    }

    cJSON_Delete(root);
}

// ---------------------------------------------------------------------------
// WebSocket event handler
// ---------------------------------------------------------------------------

static void ws_event_handler(void *arg, esp_event_base_t base,
                              int32_t event_id, void *event_data)
{
    esp_websocket_event_data_t *data = (esp_websocket_event_data_t *)event_data;

    switch (event_id) {
        case WEBSOCKET_EVENT_CONNECTED:
            ESP_LOGI(TAG, "WebSocket connected");
            s.state = STATE_CONNECTED;
            memset(s.msg_buf, 0, sizeof(s.msg_buf));
            break;

        case WEBSOCKET_EVENT_DATA:
            // Only handle text frames (opcode 1)
            if (data->op_code != 1) break;

            if ((int)data->payload_len > (int)sizeof(s.msg_buf) - 1) {
                ESP_LOGE(TAG, "message too large (%d bytes) — skipping",
                         data->payload_len);
                break;
            }

            // Accumulate fragment into buffer
            memcpy(s.msg_buf + data->payload_offset, data->data_ptr, data->data_len);

            // Dispatch once we have the complete message
            if ((int)(data->payload_offset + data->data_len) >= (int)data->payload_len) {
                s.msg_buf[data->payload_len] = '\0';
                dispatch_message(s.msg_buf);
            }
            break;

        case WEBSOCKET_EVENT_DISCONNECTED:
            ESP_LOGW(TAG, "WebSocket disconnected");
            s.state = STATE_DISCONNECTED;
            s.stream_live = false;
            s.recording   = false;
            if (s.cb.on_disconnected) s.cb.on_disconnected();
            break;

        case WEBSOCKET_EVENT_ERROR:
            ESP_LOGE(TAG, "WebSocket error");
            break;

        default:
            break;
    }
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

void obs_client_set_callbacks(const obs_callbacks_t *cb)
{
    s.cb = *cb;
}

void obs_client_start(const app_config_t *cfg)
{
    obs_client_stop();

    strlcpy(s.pass, cfg->obs_pass, sizeof(s.pass));
    s.state       = STATE_DISCONNECTED;
    s.req_id      = 0;
    s.stream_live = false;
    s.recording   = false;

    char uri[128];
    snprintf(uri, sizeof(uri), "ws://%s:%d", cfg->obs_host, cfg->obs_port);

    esp_websocket_client_config_t ws_cfg = {};
    ws_cfg.uri                   = uri;
    ws_cfg.reconnect_timeout_ms  = 5000;
    ws_cfg.network_timeout_ms    = 10000;
    ws_cfg.buffer_size           = 4096;
    ws_cfg.task_stack             = 6144;

    s.client = esp_websocket_client_init(&ws_cfg);
    esp_websocket_register_events(s.client, WEBSOCKET_EVENT_ANY,
                                   ws_event_handler, NULL);
    esp_websocket_client_start(s.client);
    ESP_LOGI(TAG, "connecting to %s", uri);
}

void obs_client_stop(void)
{
    if (!s.client) return;
    esp_websocket_client_stop(s.client);
    esp_websocket_client_destroy(s.client);
    s.client = NULL;
    s.state  = STATE_DISCONNECTED;
    ESP_LOGI(TAG, "stopped");
}

void obs_client_set_scene(const char *scene_name)
{
    cJSON *data = cJSON_CreateObject();
    cJSON_AddStringToObject(data, "sceneName", scene_name);
    send_request("SetCurrentProgramScene", data);
}

void obs_client_toggle_stream(void)
{
    send_request("ToggleStream", NULL);
}

void obs_client_toggle_recording(void)
{
    send_request("ToggleRecord", NULL);
}
