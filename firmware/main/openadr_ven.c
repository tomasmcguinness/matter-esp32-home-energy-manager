#include "openadr_ven.h"

#include <ctype.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <time.h>

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "mqtt_client.h"
#include "nvs.h"

#include "consumption_forecast.h"
#include "openadr_slots.h"
#include "power_logger.h"
#include "surplus_forecast.h"
#include "ws_server.h"

#define TAG "openadr_ven"

#define NVS_NAMESPACE     "openadr"
#define SD_BASE           "/sdcard"

#define VEN_TASK_STACK    10240
#define VEN_TASK_PRIO     5
#define MAX_STEP_WAIT_MS  10000   // upper bound on any idle wait, so async flags are never missed for long

#define HTTP_TIMEOUT_MS   15000
#define RESP_INITIAL_CAP  2048
#define RESP_MAX_CAP      (512 * 1024)
#define REPORT_BUF_SIZE   (16 * 1024)

#define MQTT_CONNECT_TIMEOUT_S 20
#define REPORT_DELAY_S         5     // post this long after the top of the hour
#define BACKOFF_INITIAL_S      5
#define BACKOFF_MAX_S          300

// vtn_call() results that are not HTTP statuses.
#define VTN_NO_RESPONSE (-1)
#define VTN_NO_TOKEN    (-2)    // the token request itself failed

#define ACTIVITY_MAX  20
#define HOURS_PER_DAY 24

typedef enum {
    STATE_DISABLED,
    STATE_WAIT_TIME_SYNC,
    STATE_AUTHENTICATING,
    STATE_REGISTERING_VEN,
    STATE_CONNECTING_MQTT,
    STATE_DISCOVERING,
    STATE_RUNNING,
    STATE_BACKOFF,
} ven_state_t;

static const char *const STATE_NAMES[] = {
    "DISABLED", "WAIT_TIME_SYNC", "AUTHENTICATING", "REGISTERING_VEN",
    "CONNECTING_MQTT", "DISCOVERING", "RUNNING", "BACKOFF",
};

typedef struct {
    bool enabled;
    char vtn_base_url[128];
    char client_id[64];
    char client_secret[128];
    char ven_name[48];
    char forecast_source[8];        // "net" or "gross"
    char mqtt_host_override[128];
} ven_config_t;

typedef struct {
    time_t ts;
    char   kind[8];                 // state | report | mqtt | error | info
    char   text[120];
} activity_t;

typedef struct {
    ven_state_t state;
    char   last_error[160];
    char   ven_id[64];
    char   program_id[64];
    char   program_name[64];
    char   event_id[64];
    time_t last_report_ts;
    int    last_report_http;
    time_t next_report_ts;
    time_t retry_ts;                // when BACKOFF ends
    char   source_used[16];         // net | gross | gross_fallback
    activity_t activity[ACTIVITY_MAX];
    int    activity_head;           // next slot to write
    int    activity_count;
} ven_status_t;

typedef enum { CMD_WAKE, CMD_RESTART, CMD_RESET, CMD_SEND_NOW } ven_cmd_t;

// Shared with the HTTP handlers and the MQTT task; guarded by s_lock.
static SemaphoreHandle_t s_lock;
static ven_config_t      s_cfg;
static ven_status_t      s_st;

static QueueHandle_t s_queue;

// Set by the MQTT task, read by the VEN task.
static volatile bool     s_mqtt_connected;
static volatile uint32_t s_mqtt_conn_gen;    // bumped on every (re)connect
static volatile bool     s_resync_pending;   // a notification arrived

// Everything below is owned by the VEN task.
static ven_config_t s_run;                   // config snapshot the state machine is running with
static char        *s_token;
static esp_mqtt_client_handle_t s_mqtt;
static time_t       s_mqtt_deadline;
static uint32_t     s_sub_gen;               // connection generation the ven topics were subscribed on
static uint32_t     s_program_sub_gen;       // ... and the program topic
static char         s_program_sub_id[64];
static char         s_program_target[96];    // e.g. "PROGRAM_NAME:HomeForecast", from the ven's targets
static ven_state_t  s_resume_state;
static int          s_backoff_s = BACKOFF_INITIAL_S;
static time_t       s_forecast_hour;         // hour (UTC start) the forecast was last posted for
static time_t       s_actual_hour;
static bool         s_send_now;
static bool         s_retried_after_404;

// ── Status and activity log ───────────────────────────────────────────────────

static cJSON *status_to_json(void)
{
    cJSON *root = cJSON_CreateObject();

    xSemaphoreTake(s_lock, portMAX_DELAY);
    cJSON_AddStringToObject(root, "state", STATE_NAMES[s_st.state]);
    cJSON_AddStringToObject(root, "last_error", s_st.last_error);
    cJSON_AddStringToObject(root, "ven_id", s_st.ven_id);
    cJSON_AddStringToObject(root, "program_id", s_st.program_id);
    cJSON_AddStringToObject(root, "program_name", s_st.program_name);
    cJSON_AddStringToObject(root, "event_id", s_st.event_id);
    cJSON_AddBoolToObject(root, "event_found", s_st.event_id[0] != '\0');
    cJSON_AddBoolToObject(root, "mqtt_connected", s_mqtt_connected);
    cJSON_AddNumberToObject(root, "last_report_ts", (double)s_st.last_report_ts);
    cJSON_AddNumberToObject(root, "last_report_http", s_st.last_report_http);
    cJSON_AddNumberToObject(root, "next_report_ts", s_st.state == STATE_RUNNING ? (double)s_st.next_report_ts : 0);
    cJSON_AddNumberToObject(root, "retry_ts", s_st.state == STATE_BACKOFF ? (double)s_st.retry_ts : 0);
    cJSON_AddStringToObject(root, "forecast_source_used", s_st.source_used);

    // Newest first.
    cJSON *log = cJSON_AddArrayToObject(root, "activity");
    for (int i = 1; i <= s_st.activity_count; i++) {
        const activity_t *a = &s_st.activity[(s_st.activity_head - i + ACTIVITY_MAX) % ACTIVITY_MAX];
        cJSON *obj = cJSON_CreateObject();
        cJSON_AddNumberToObject(obj, "ts", (double)a->ts);
        cJSON_AddStringToObject(obj, "kind", a->kind);
        cJSON_AddStringToObject(obj, "text", a->text);
        cJSON_AddItemToArray(log, obj);
    }
    xSemaphoreGive(s_lock);

    return root;
}

// Push the current status to any open web UI. ws_server_broadcast only queues
// work for the httpd task, so this does not block on slow clients.
static void broadcast_status(void)
{
    cJSON *msg = cJSON_CreateObject();
    cJSON_AddStringToObject(msg, "type", "openadr_status");
    cJSON_AddItemToObject(msg, "data", status_to_json());
    char *json = cJSON_PrintUnformatted(msg);
    cJSON_Delete(msg);
    if (json) {
        ws_server_broadcast(json, strlen(json));
        free(json);
    }
}

__attribute__((format(printf, 2, 3)))
static void activity_add(const char *kind, const char *fmt, ...)
{
    activity_t a = {.ts = time(NULL)};
    strlcpy(a.kind, kind, sizeof(a.kind));
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(a.text, sizeof(a.text), fmt, ap);
    va_end(ap);

    ESP_LOGI(TAG, "[%s] %s", a.kind, a.text);

    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_st.activity[s_st.activity_head] = a;
    s_st.activity_head = (s_st.activity_head + 1) % ACTIVITY_MAX;
    if (s_st.activity_count < ACTIVITY_MAX)
        s_st.activity_count++;
    xSemaphoreGive(s_lock);

    broadcast_status();
}

static void set_state(ven_state_t state)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    bool changed = s_st.state != state;
    s_st.state = state;
    xSemaphoreGive(s_lock);

    if (changed)
        activity_add("state", "%s", STATE_NAMES[state]);
}

static void set_last_error(const char *msg)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    strlcpy(s_st.last_error, msg, sizeof(s_st.last_error));
    xSemaphoreGive(s_lock);
}

// Record an error and retry `resume` after an exponentially growing delay.
__attribute__((format(printf, 2, 3)))
static void fail(ven_state_t resume, const char *fmt, ...)
{
    char msg[160];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);

    s_resume_state = resume;

    xSemaphoreTake(s_lock, portMAX_DELAY);
    strlcpy(s_st.last_error, msg, sizeof(s_st.last_error));
    s_st.retry_ts = time(NULL) + s_backoff_s;
    xSemaphoreGive(s_lock);

    activity_add("error", "%s (retry in %d s)", msg, s_backoff_s);
    set_state(STATE_BACKOFF);

    s_backoff_s *= 2;
    if (s_backoff_s > BACKOFF_MAX_S)
        s_backoff_s = BACKOFF_MAX_S;
}

// ── NVS ───────────────────────────────────────────────────────────────────────

static void nvs_read_str(nvs_handle_t h, const char *key, char *out, size_t len, const char *fallback)
{
    size_t n = len;
    if (nvs_get_str(h, key, out, &n) != ESP_OK)
        strlcpy(out, fallback, len);
}

static void default_ven_name(char *out, size_t len)
{
    uint8_t mac[6] = {0};
    esp_read_mac(mac, ESP_MAC_ETH);
    snprintf(out, len, "hems-%02x%02x%02x", mac[3], mac[4], mac[5]);
}

static void config_load(ven_config_t *cfg)
{
    char ven_name[sizeof(cfg->ven_name)];
    default_ven_name(ven_name, sizeof(ven_name));

    memset(cfg, 0, sizeof(*cfg));
    strlcpy(cfg->ven_name, ven_name, sizeof(cfg->ven_name));
    strlcpy(cfg->forecast_source, "net", sizeof(cfg->forecast_source));

    nvs_handle_t h;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &h) != ESP_OK)
        return; // nothing saved yet

    uint8_t enabled = 0;
    nvs_get_u8(h, "enabled", &enabled);
    cfg->enabled = enabled != 0;
    nvs_read_str(h, "vtn_url", cfg->vtn_base_url, sizeof(cfg->vtn_base_url), "");
    nvs_read_str(h, "client_id", cfg->client_id, sizeof(cfg->client_id), "");
    nvs_read_str(h, "client_secret", cfg->client_secret, sizeof(cfg->client_secret), "");
    nvs_read_str(h, "ven_name", cfg->ven_name, sizeof(cfg->ven_name), ven_name);
    nvs_read_str(h, "fc_source", cfg->forecast_source, sizeof(cfg->forecast_source), "net");
    nvs_read_str(h, "mqtt_host", cfg->mqtt_host_override, sizeof(cfg->mqtt_host_override), "");

    xSemaphoreTake(s_lock, portMAX_DELAY);
    nvs_read_str(h, "ven_id", s_st.ven_id, sizeof(s_st.ven_id), "");
    nvs_read_str(h, "program_id", s_st.program_id, sizeof(s_st.program_id), "");
    nvs_read_str(h, "event_id", s_st.event_id, sizeof(s_st.event_id), "");
    xSemaphoreGive(s_lock);

    nvs_close(h);
}

static esp_err_t config_save(const ven_config_t *cfg)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h);
    if (err != ESP_OK)
        return err;

    nvs_set_u8(h, "enabled", cfg->enabled ? 1 : 0);
    nvs_set_str(h, "vtn_url", cfg->vtn_base_url);
    nvs_set_str(h, "client_id", cfg->client_id);
    nvs_set_str(h, "client_secret", cfg->client_secret);
    nvs_set_str(h, "ven_name", cfg->ven_name);
    nvs_set_str(h, "fc_source", cfg->forecast_source);
    nvs_set_str(h, "mqtt_host", cfg->mqtt_host_override);
    err = nvs_commit(h);
    nvs_close(h);
    return err;
}

// Update one cached ID in memory and, if it changed, in NVS.
static void cached_id_set(const char *key, char *field, size_t len, const char *value)
{
    if (strcmp(field, value) == 0)
        return;

    xSemaphoreTake(s_lock, portMAX_DELAY);
    strlcpy(field, value, len);
    xSemaphoreGive(s_lock);

    nvs_handle_t h;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h) != ESP_OK)
        return;
    if (value[0])
        nvs_set_str(h, key, value);
    else
        nvs_erase_key(h, key);
    nvs_commit(h);
    nvs_close(h);
}

static void cached_ids_clear(void)
{
    cached_id_set("ven_id", s_st.ven_id, sizeof(s_st.ven_id), "");
    cached_id_set("program_id", s_st.program_id, sizeof(s_st.program_id), "");
    cached_id_set("event_id", s_st.event_id, sizeof(s_st.event_id), "");

    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_st.program_name[0] = '\0';
    xSemaphoreGive(s_lock);
    s_program_target[0] = '\0';
}

// ── HTTP ──────────────────────────────────────────────────────────────────────

typedef struct {
    char  *data;
    size_t len;
    size_t cap;
} resp_buf_t;

static esp_err_t http_event_handler(esp_http_client_event_t *evt)
{
    if (evt->event_id != HTTP_EVENT_ON_DATA)
        return ESP_OK;

    resp_buf_t *b = evt->user_data;
    if (b->len + evt->data_len > b->cap) {
        size_t new_cap = b->cap * 2 + evt->data_len;
        if (new_cap > RESP_MAX_CAP)
            return ESP_ERR_NO_MEM;
        char *tmp = heap_caps_realloc(b->data, new_cap + 1, MALLOC_CAP_SPIRAM);
        if (!tmp)
            return ESP_ERR_NO_MEM;
        b->data = tmp;
        b->cap  = new_cap;
    }
    memcpy(b->data + b->len, evt->data, evt->data_len);
    b->len += evt->data_len;
    return ESP_OK;
}

static void url_encode(const char *in, char *out, size_t len)
{
    size_t o = 0;
    for (; *in && o + 4 < len; in++) {
        unsigned char c = (unsigned char)*in;
        if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~')
            out[o++] = (char)c;
        else
            o += snprintf(out + o, len - o, "%%%02X", c);
    }
    out[o] = '\0';
}

// One request to the VTN. Returns the HTTP status, or -1 if there was no
// response. *out (if not NULL) receives the NUL-terminated body, allocated in
// PSRAM; the caller frees it.
static int http_do(esp_http_client_method_t method, const char *path, const char *content_type,
                   const char *body, bool auth, char **out)
{
    if (out)
        *out = NULL;

    char url[512];
    size_t base_len = strlen(s_run.vtn_base_url);
    while (base_len > 0 && s_run.vtn_base_url[base_len - 1] == '/')
        base_len--;
    snprintf(url, sizeof(url), "%.*s%s", (int)base_len, s_run.vtn_base_url, path);

    resp_buf_t buf = {
        .data = heap_caps_malloc(RESP_INITIAL_CAP + 1, MALLOC_CAP_SPIRAM),
        .cap  = RESP_INITIAL_CAP,
    };
    if (!buf.data)
        return -1;

    esp_http_client_config_t cfg = {
        .url           = url,
        .method        = method,
        .event_handler = http_event_handler,
        .user_data     = &buf,
        .timeout_ms    = HTTP_TIMEOUT_MS,
    };
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client) {
        free(buf.data);
        return -1;
    }

    char *bearer = NULL;
    if (auth && s_token && asprintf(&bearer, "Bearer %s", s_token) > 0)
        esp_http_client_set_header(client, "Authorization", bearer);
    if (body) {
        esp_http_client_set_header(client, "Content-Type", content_type);
        esp_http_client_set_post_field(client, body, (int)strlen(body));
    }

    esp_err_t err = esp_http_client_perform(client);
    int status = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);
    free(bearer);

    if (err != ESP_OK) {
        ESP_LOGW(TAG, "%s failed: %s", url, esp_err_to_name(err));
        free(buf.data);
        return -1;
    }

    buf.data[buf.len] = '\0';
    if (out)
        *out = buf.data;
    else
        free(buf.data);
    return status;
}

// Contract step 1. Returns the HTTP status (200 on success) or -1.
static int authenticate(void)
{
    free(s_token);
    s_token = NULL;

    char id[3 * sizeof(s_run.client_id)], secret[3 * sizeof(s_run.client_secret)];
    url_encode(s_run.client_id, id, sizeof(id));
    url_encode(s_run.client_secret, secret, sizeof(secret));

    char *form = NULL;
    if (asprintf(&form, "grant_type=client_credentials&client_id=%s&client_secret=%s", id, secret) < 0)
        return -1;

    char *resp = NULL;
    int status = http_do(HTTP_METHOD_POST, "/auth/token", "application/x-www-form-urlencoded", form, false, &resp);
    free(form);

    if (status == 200) {
        cJSON *json = cJSON_Parse(resp);
        const cJSON *tok = cJSON_GetObjectItem(json, "access_token");
        if (cJSON_IsString(tok) && tok->valuestring[0])
            s_token = strdup(tok->valuestring);
        cJSON_Delete(json);
        if (!s_token)
            status = -1;
    }
    free(resp);
    return status;
}

// An authenticated call. Returns the HTTP status, VTN_NO_RESPONSE or
// VTN_NO_TOKEN. On 401 the token is refreshed and the call retried once; a
// second 401 is returned to the caller as a hard error.
static int vtn_call(esp_http_client_method_t method, const char *path, const char *json_body, char **out)
{
    if (out)
        *out = NULL;
    if (!s_token) {
        int st = authenticate();
        if (st != 200)
            return st == -1 ? VTN_NO_RESPONSE : VTN_NO_TOKEN;
    }

    int status = http_do(method, path, "application/json", json_body, true, out);
    if (status != 401)
        return status;

    if (out) {
        free(*out);
        *out = NULL;
    }
    activity_add("info", "%s returned 401, re-authenticating", path);
    if (authenticate() != 200)
        return VTN_NO_TOKEN;
    return http_do(method, path, "application/json", json_body, true, out);
}

// Turn a failed VTN call into the right recovery: re-authenticate after a 401,
// forget the cached IDs and register again after a 404, otherwise retry `resume`.
static void fail_http(ven_state_t resume, const char *what, int status)
{
    if (status == VTN_NO_RESPONSE) {
        fail(resume, "%s: no response from VTN", what);
    } else if (status == VTN_NO_TOKEN) {
        fail(STATE_AUTHENTICATING, "%s: could not get a token from the VTN", what);
    } else if (status == 401) {
        free(s_token);
        s_token = NULL;
        fail(STATE_AUTHENTICATING, "%s: HTTP 401 after re-authenticating", what);
    } else if (status == 404) {
        cached_ids_clear();
        fail(STATE_REGISTERING_VEN, "%s: HTTP 404, cached IDs cleared", what);
    } else {
        fail(resume, "%s: HTTP %d", what, status);
    }
}

static const char *json_str(const cJSON *obj, const char *key)
{
    const cJSON *item = cJSON_GetObjectItem(obj, key);
    return cJSON_IsString(item) ? item->valuestring : NULL;
}

// ── MQTT ──────────────────────────────────────────────────────────────────────

static void wake_task(void)
{
    ven_cmd_t cmd = CMD_WAKE;
    xQueueSend(s_queue, &cmd, 0); // if the queue is full the task is about to run anyway
}

// Runs on the esp-mqtt task. Notifications are only a nudge: the VEN task
// re-reads the truth over REST, so a duplicate QoS 1 delivery just sets the
// same flag again.
static void mqtt_event_handler(void *arg, esp_event_base_t base, int32_t event_id, void *event_data)
{
    esp_mqtt_event_handle_t event = event_data;

    switch ((esp_mqtt_event_id_t)event_id) {
    case MQTT_EVENT_CONNECTED:
        s_mqtt_connected = true;
        s_mqtt_conn_gen++;
        activity_add("mqtt", "Connected to broker");
        wake_task();
        break;
    case MQTT_EVENT_DISCONNECTED:
        // Also fires for every failed reconnect attempt; only report the edge.
        if (s_mqtt_connected) {
            s_mqtt_connected = false;
            activity_add("mqtt", "Disconnected from broker");
        }
        break;
    case MQTT_EVENT_DATA:
        // A large payload arrives in fragments; the topic is only on the first.
        if (event->current_data_offset == 0) {
            activity_add("mqtt", "Notification on %.*s", event->topic_len, event->topic);
            s_resync_pending = true;
            wake_task();
        }
        break;
    default:
        break;
    }
}

static void mqtt_stop(void)
{
    if (s_mqtt) {
        esp_mqtt_client_destroy(s_mqtt);
        s_mqtt = NULL;
    }
    s_mqtt_connected = false;
}

// GET a topics endpoint and subscribe to its `all` topic. Topic names are never
// cached or hardcoded; the VTN owns the naming scheme.
static bool subscribe_from(const char *path)
{
    char *resp = NULL;
    int status = vtn_call(HTTP_METHOD_GET, path, NULL, &resp);
    if (status != 200) {
        free(resp);
        fail_http(STATE_CONNECTING_MQTT, path, status);
        return false;
    }

    cJSON *json = cJSON_Parse(resp);
    free(resp);
    const char *all = json_str(cJSON_GetObjectItem(json, "topics"), "all");
    bool ok = all && esp_mqtt_client_subscribe(s_mqtt, all, 1) >= 0;
    if (ok)
        ESP_LOGI(TAG, "Subscribed to %s", all);
    else
        fail(STATE_CONNECTING_MQTT, "Could not subscribe using %s", path);
    cJSON_Delete(json);
    return ok;
}

// ── State machine steps ───────────────────────────────────────────────────────
// Each step does one state's work and returns how long to wait before stepping
// again. A failed step calls fail()/fail_http(), which moves to BACKOFF.

static TickType_t step_authenticate(void)
{
    int status = authenticate();
    if (status != 200) {
        if (status == -1)
            fail(STATE_AUTHENTICATING, "POST /auth/token: no response from VTN");
        else
            fail(STATE_AUTHENTICATING, "POST /auth/token: HTTP %d", status);
        return 0;
    }
    set_state(STATE_REGISTERING_VEN);
    return 0;
}

// Contract step 2: find the ven by name, or create it.
static TickType_t step_register_ven(void)
{
    char name[3 * sizeof(s_run.ven_name)], path[256];
    url_encode(s_run.ven_name, name, sizeof(name));
    snprintf(path, sizeof(path), "/vens?venName=%s", name);

    char *resp = NULL;
    int status = vtn_call(HTTP_METHOD_GET, path, NULL, &resp);
    if (status != 200) {
        free(resp);
        fail_http(STATE_REGISTERING_VEN, "GET /vens", status);
        return 0;
    }

    cJSON *json = cJSON_Parse(resp);
    free(resp);
    cJSON *ven = cJSON_IsArray(json) ? cJSON_DetachItemFromArray(json, 0) : NULL;
    cJSON_Delete(json);

    if (!ven) {
        cJSON *req = cJSON_CreateObject();
        cJSON_AddStringToObject(req, "objectType", "VEN_VEN_REQUEST");
        cJSON_AddStringToObject(req, "venName", s_run.ven_name);
        char *body = cJSON_PrintUnformatted(req);
        cJSON_Delete(req);

        status = vtn_call(HTTP_METHOD_POST, "/vens", body, &resp);
        free(body);
        if (status != 201 && status != 200) {
            free(resp);
            fail_http(STATE_REGISTERING_VEN, "POST /vens", status);
            return 0;
        }
        ven = cJSON_Parse(resp);
        free(resp);
        activity_add("info", "Created ven %s", s_run.ven_name);
    }

    const char *id = json_str(ven, "id");
    if (!id) {
        cJSON_Delete(ven);
        fail(STATE_REGISTERING_VEN, "VTN returned a ven without an id");
        return 0;
    }
    cached_id_set("ven_id", s_st.ven_id, sizeof(s_st.ven_id), id);

    // The VTN stamps the ven with the program it belongs to.
    s_program_target[0] = '\0';
    const cJSON *target;
    cJSON_ArrayForEach(target, cJSON_GetObjectItem(ven, "targets")) {
        if (cJSON_IsString(target) && strncmp(target->valuestring, "PROGRAM_NAME:", 13) == 0) {
            strlcpy(s_program_target, target->valuestring, sizeof(s_program_target));
            break;
        }
    }
    cJSON_Delete(ven);

    set_state(STATE_CONNECTING_MQTT);
    return 0;
}

// Contract steps 3 and 4: connect to the broker from GET /notifiers (client ID
// = venName), then subscribe to the ven's own event and program topics.
static TickType_t step_connect_mqtt(void)
{
    if (!s_mqtt) {
        char uri[160] = "";
        if (s_run.mqtt_host_override[0]) {
            if (strstr(s_run.mqtt_host_override, "://"))
                strlcpy(uri, s_run.mqtt_host_override, sizeof(uri));
            else
                snprintf(uri, sizeof(uri), "mqtt://%s:1883", s_run.mqtt_host_override);
        } else {
            char *resp = NULL;
            int status = vtn_call(HTTP_METHOD_GET, "/notifiers", NULL, &resp);
            if (status != 200) {
                free(resp);
                fail_http(STATE_CONNECTING_MQTT, "GET /notifiers", status);
                return 0;
            }
            cJSON *json = cJSON_Parse(resp);
            free(resp);
            const cJSON *first = cJSON_GetArrayItem(cJSON_GetObjectItem(cJSON_GetObjectItem(json, "mqtt"), "uris"), 0);
            if (cJSON_IsString(first))
                strlcpy(uri, first->valuestring, sizeof(uri));
            cJSON_Delete(json);
            if (!uri[0]) {
                fail(STATE_CONNECTING_MQTT, "GET /notifiers: no MQTT broker offered");
                return 0;
            }
        }

        esp_mqtt_client_config_t cfg = {
            .broker.address.uri    = uri,
            .credentials.client_id = s_run.ven_name,
        };
        s_mqtt = esp_mqtt_client_init(&cfg);
        if (!s_mqtt || esp_mqtt_client_register_event(s_mqtt, ESP_EVENT_ANY_ID, mqtt_event_handler, NULL) != ESP_OK ||
            esp_mqtt_client_start(s_mqtt) != ESP_OK) {
            mqtt_stop();
            fail(STATE_CONNECTING_MQTT, "Could not start MQTT client for %s", uri);
            return 0;
        }
        s_mqtt_deadline = time(NULL) + MQTT_CONNECT_TIMEOUT_S;
        activity_add("mqtt", "Connecting to %s", uri);
    }

    if (!s_mqtt_connected) {
        if (time(NULL) > s_mqtt_deadline) {
            // Drop the client so the next attempt re-reads the broker address.
            mqtt_stop();
            fail(STATE_CONNECTING_MQTT, "MQTT broker did not accept the connection");
            return 0;
        }
        return pdMS_TO_TICKS(500);
    }

    uint32_t gen = s_mqtt_conn_gen;
    char path[160];
    snprintf(path, sizeof(path), "/notifiers/mqtt/topics/vens/%s/events", s_st.ven_id);
    if (!subscribe_from(path))
        return 0;
    snprintf(path, sizeof(path), "/notifiers/mqtt/topics/vens/%s/programs", s_st.ven_id);
    if (!subscribe_from(path))
        return 0;
    s_sub_gen = gen;

    set_state(STATE_DISCOVERING);
    return 0;
}

// True if the event asks for a DEMAND forecast report.
static bool event_wants_forecast(const cJSON *event)
{
    const cJSON *desc;
    cJSON_ArrayForEach(desc, cJSON_GetObjectItem(event, "reportDescriptors")) {
        const char *payload = json_str(desc, "payloadType");
        const char *reading = json_str(desc, "readingType");
        if (payload && strcmp(payload, "FORECAST") == 0 && (!reading || strcmp(reading, "DEMAND") == 0))
            return true;
    }
    return false;
}

// Contract steps 5 and 6: find the program and the event that requests the forecast.
static TickType_t step_discover(void)
{
    if (!s_program_target[0]) {
        fail(STATE_REGISTERING_VEN, "The ven has no PROGRAM_NAME target, so there is no program to report to");
        return 0;
    }

    char target[3 * sizeof(s_program_target)], path[400];
    url_encode(s_program_target, target, sizeof(target));
    snprintf(path, sizeof(path), "/programs?targets=%s", target);

    char *resp = NULL;
    int status = vtn_call(HTTP_METHOD_GET, path, NULL, &resp);
    if (status != 200) {
        free(resp);
        fail_http(STATE_DISCOVERING, "GET /programs", status);
        return 0;
    }
    cJSON *json = cJSON_Parse(resp);
    free(resp);
    const cJSON *program = cJSON_GetArrayItem(json, 0);
    const char *program_id = json_str(program, "id");
    if (!program_id) {
        cJSON_Delete(json);
        fail(STATE_DISCOVERING, "No program found for %s", s_program_target);
        return 0;
    }
    const char *program_name = json_str(program, "programName");
    xSemaphoreTake(s_lock, portMAX_DELAY);
    strlcpy(s_st.program_name, program_name ? program_name : "", sizeof(s_st.program_name));
    xSemaphoreGive(s_lock);
    cached_id_set("program_id", s_st.program_id, sizeof(s_st.program_id), program_id);
    cJSON_Delete(json);

    // Subscribe to the program's event topic once per connection.
    if (s_program_sub_gen != s_sub_gen || strcmp(s_program_sub_id, s_st.program_id) != 0) {
        snprintf(path, sizeof(path), "/notifiers/mqtt/topics/programs/%s/events", s_st.program_id);
        if (!subscribe_from(path))
            return 0;
        s_program_sub_gen = s_sub_gen;
        strlcpy(s_program_sub_id, s_st.program_id, sizeof(s_program_sub_id));
    }

    snprintf(path, sizeof(path), "/events?programId=%s", s_st.program_id);
    status = vtn_call(HTTP_METHOD_GET, path, NULL, &resp);
    if (status != 200) {
        free(resp);
        fail_http(STATE_DISCOVERING, "GET /events", status);
        return 0;
    }
    json = cJSON_Parse(resp);
    free(resp);

    const char *event_id = NULL;
    const cJSON *event;
    cJSON_ArrayForEach(event, json) {
        if (event_wants_forecast(event)) {
            event_id = json_str(event, "id");
            break;
        }
    }
    if (!event_id) {
        cJSON_Delete(json);
        cached_id_set("event_id", s_st.event_id, sizeof(s_st.event_id), "");
        fail(STATE_DISCOVERING, "Program %s has no event requesting a DEMAND forecast", s_st.program_name);
        return 0;
    }
    cached_id_set("event_id", s_st.event_id, sizeof(s_st.event_id), event_id);
    cJSON_Delete(json);

    s_backoff_s = BACKOFF_INITIAL_S;
    set_last_error("");
    set_state(STATE_RUNNING);
    return 0;
}

// ── Reports ───────────────────────────────────────────────────────────────────

typedef enum {
    REPORT_SENT,
    REPORT_SKIPPED,     // nothing to send; do not retry this hour
    REPORT_FAILED,      // the state has changed (BACKOFF or re-registration); stop this tick
} report_result_t;

// Read one value per local hour from a forecast JSON (surplus_forecast_json or
// consumption_forecast_json). Frees `json`. False unless all 24 hours are there.
static bool hourly_from_json(char *json, const char *key, double out[HOURS_PER_DAY])
{
    cJSON *root = cJSON_Parse(json);
    free(json);

    const cJSON *slots = cJSON_GetObjectItem(root, "slots");
    bool ok = cJSON_GetArraySize(slots) == HOURS_PER_DAY;
    for (int h = 0; ok && h < HOURS_PER_DAY; h++) {
        const cJSON *value = cJSON_GetObjectItem(cJSON_GetArrayItem(slots, h), key);
        ok = cJSON_IsNumber(value);
        if (ok)
            out[h] = value->valuedouble;
    }
    cJSON_Delete(root);
    return ok;
}

// A day's forecast demand in kW per local hour (positive is import) for the
// configured `source`. Writes the source actually used to `used`. False if no
// forecast is stored for the date.
static bool forecast_demand_kw(const char *source, const char *date, double kw[HOURS_PER_DAY], const char **used)
{
    double w[HOURS_PER_DAY];

    if (strcmp(source, "gross") != 0) {
        if (hourly_from_json(surplus_forecast_json(date), "surplus_w", w)) {
            for (int h = 0; h < HOURS_PER_DAY; h++)
                kw[h] = -w[h] / 1000.0;
            *used = "net";
            return true;
        }
        *used = "gross_fallback";
    } else {
        *used = "gross";
    }

    if (!hourly_from_json(consumption_forecast_json(date), "power_w", w))
        return false;
    for (int h = 0; h < HOURS_PER_DAY; h++)
        kw[h] = w[h] / 1000.0;
    return true;
}

bool openadr_ven_forecast_demand_w(const char *date, double w[24], const char **source_used, bool *enabled)
{
    char source[sizeof(s_cfg.forecast_source)] = "net";
    *enabled = false;
    if (s_lock) {
        xSemaphoreTake(s_lock, portMAX_DELAY);
        strlcpy(source, s_cfg.forecast_source, sizeof(source));
        *enabled = s_cfg.enabled;
        xSemaphoreGive(s_lock);
    }

    double kw[HOURS_PER_DAY];
    if (!forecast_demand_kw(source, date, kw, source_used))
        return false;
    for (int h = 0; h < HOURS_PER_DAY; h++)
        w[h] = kw[h] * 1000.0;
    return true;
}

// Average grid power in kW over the hour starting at `hour_start`, from the
// 1-minute log. (grid-hourly-* only exists after the midnight rollup.) Positive
// is import. False if no minutes were logged in that hour.
static bool actual_demand_kw(time_t hour_start, double *kw)
{
    struct tm tm;
    localtime_r(&hour_start, &tm);
    char path[64];
    strftime(path, sizeof(path), SD_BASE "/grid-%Y-%m-%d", &tm);

    FILE *f = fopen(path, "rb");
    if (!f)
        return false;

    int64_t sum_mw = 0;
    int     count  = 0;
    power_record_t rec;
    while (fread(&rec, sizeof(rec), 1, f) == 1) {
        if ((time_t)rec.unix_minute >= hour_start && (time_t)rec.unix_minute < hour_start + 3600) {
            sum_mw += rec.power_mw;
            count++;
        }
    }
    fclose(f);

    if (count == 0)
        return false;
    *kw = (double)sum_mw / count / 1e6;
    return true;
}

static void add_interval(cJSON *intervals, int id, time_t start, const char *payload_type, double kw)
{
    char start_str[24];
    openadr_format_utc(start, start_str, sizeof(start_str));

    cJSON *interval = cJSON_CreateObject();
    cJSON_AddNumberToObject(interval, "id", id);
    cJSON *period = cJSON_AddObjectToObject(interval, "intervalPeriod");
    cJSON_AddStringToObject(period, "start", start_str);
    cJSON_AddStringToObject(period, "duration", "PT1H");
    cJSON *payload = cJSON_CreateObject();
    cJSON_AddStringToObject(payload, "type", payload_type);
    cJSON *values = cJSON_AddArrayToObject(payload, "values");
    cJSON_AddItemToArray(values, cJSON_CreateNumber(round(kw * 1000.0) / 1000.0));
    cJSON *payloads = cJSON_AddArrayToObject(interval, "payloads");
    cJSON_AddItemToArray(payloads, payload);
    cJSON_AddItemToArray(intervals, interval);
}

// Start a report body (contract step 7); returns the intervals array to fill.
// `name_time` is the UTC hour the report is named after.
static cJSON *report_begin(cJSON **root, const char *name_prefix, time_t name_time, const char *payload_type)
{
    struct tm tm;
    gmtime_r(&name_time, &tm);
    char stamp[20], name[48];
    strftime(stamp, sizeof(stamp), "%Y-%m-%dT%H", &tm);
    snprintf(name, sizeof(name), "%s %s", name_prefix, stamp);

    *root = cJSON_CreateObject();
    cJSON_AddStringToObject(*root, "eventId", s_st.event_id);
    cJSON_AddStringToObject(*root, "clientName", s_run.ven_name);
    cJSON_AddStringToObject(*root, "reportName", name);

    cJSON *descriptor = cJSON_CreateObject();
    cJSON_AddStringToObject(descriptor, "payloadType", payload_type);
    cJSON_AddStringToObject(descriptor, "readingType", "DEMAND");
    cJSON_AddStringToObject(descriptor, "units", "KW");
    cJSON_AddItemToArray(cJSON_AddArrayToObject(*root, "payloadDescriptors"), descriptor);

    cJSON *resource = cJSON_CreateObject();
    cJSON_AddStringToObject(resource, "resourceName", "AGGREGATED_REPORT");
    cJSON *intervals = cJSON_AddArrayToObject(resource, "intervals");
    cJSON_AddItemToArray(cJSON_AddArrayToObject(*root, "resources"), resource);
    return intervals;
}

// POST a report and deal with the outcome. Takes ownership of `root`.
static report_result_t report_post(cJSON *root, const char *label)
{
    char *body = heap_caps_malloc(REPORT_BUF_SIZE, MALLOC_CAP_SPIRAM);
    bool printed = body && cJSON_PrintPreallocated(root, body, REPORT_BUF_SIZE, false);
    cJSON_Delete(root);
    if (!printed) {
        free(body);
        fail(STATE_RUNNING, "%s: report did not fit in the buffer", label);
        return REPORT_FAILED;
    }

    int status = vtn_call(HTTP_METHOD_POST, "/reports", body, NULL);
    free(body);

    if (status > 0) {
        xSemaphoreTake(s_lock, portMAX_DELAY);
        s_st.last_report_ts   = time(NULL);
        s_st.last_report_http = status;
        xSemaphoreGive(s_lock);
    }

    if (status == 201 || status == 200) {
        s_retried_after_404 = false;
        s_backoff_s = BACKOFF_INITIAL_S;
        set_last_error("");
        activity_add("report", "%s: HTTP %d", label, status);
        return REPORT_SENT;
    }

    if (status == 404) {
        // The event, program or ven is gone (the VTN may have been reset).
        // Register and discover again, then retry once; if that also gets a
        // 404, leave it until the next tick.
        activity_add("report", "%s: HTTP 404, re-registering", label);
        set_last_error("POST /reports: HTTP 404, cached IDs cleared");
        cached_ids_clear();
        if (s_retried_after_404) {
            time_t now = time(NULL);
            s_forecast_hour = s_actual_hour = now - now % 3600;
            s_send_now = false;
        }
        s_retried_after_404 = !s_retried_after_404;
        set_state(STATE_REGISTERING_VEN);
        return REPORT_FAILED;
    }

    activity_add("report", "%s: %s", label, status < 0 ? "not sent" : "rejected");
    fail_http(STATE_RUNNING, "POST /reports", status);
    return REPORT_FAILED;
}

// Forecast for today's local day: one PT1H interval per hour slot from local
// midnight, so 23 or 25 intervals on a DST day.
static report_result_t send_forecast(time_t now)
{
    struct tm tm;
    localtime_r(&now, &tm);
    char date[11];
    strftime(date, sizeof(date), "%Y-%m-%d", &tm);

    time_t starts[OPENADR_MAX_DAY_SLOTS];
    int count = openadr_day_slots(date, starts, OPENADR_MAX_DAY_SLOTS);

    double kw[HOURS_PER_DAY];
    const char *used = "";
    bool have = count > 0 && forecast_demand_kw(s_run.forecast_source, date, kw, &used);

    xSemaphoreTake(s_lock, portMAX_DELAY);
    strlcpy(s_st.source_used, have ? used : "", sizeof(s_st.source_used));
    xSemaphoreGive(s_lock);

    if (!have) {
        set_last_error("No forecast stored for today");
        activity_add("error", "No forecast stored for %s, nothing sent", date);
        return REPORT_SKIPPED;
    }

    cJSON *root;
    cJSON *intervals = report_begin(&root, "Forecast", now, "FORECAST");
    for (int i = 0; i < count; i++)
        add_interval(intervals, i, starts[i], "FORECAST", kw[openadr_slot_local_hour(starts[i])]);

    char label[64];
    snprintf(label, sizeof(label), "Forecast, %d intervals, %s", count,
             strcmp(used, "gross_fallback") == 0 ? "gross (no surplus forecast)" : used);
    return report_post(root, label);
}

// Actual for the hour that just completed.
static report_result_t send_actual(time_t hour_start)
{
    char start_str[24];
    openadr_format_utc(hour_start, start_str, sizeof(start_str));

    double kw;
    if (!actual_demand_kw(hour_start, &kw)) {
        activity_add("info", "No grid data for %s, actual not sent", start_str);
        return REPORT_SKIPPED;
    }

    cJSON *root;
    cJSON *intervals = report_begin(&root, "Actual", hour_start, "DIRECT_READ");
    add_interval(intervals, 0, hour_start, "DIRECT_READ", kw);

    char label[64];
    snprintf(label, sizeof(label), "Actual for %s", start_str);
    return report_post(root, label);
}

// RUNNING: post both reports once per hour, a few seconds after the hour, and
// straight away the first time the VEN gets here.
static TickType_t step_running(void)
{
    time_t now  = time(NULL);
    time_t hour = now - now % 3600;
    bool   due  = now >= hour + REPORT_DELAY_S || s_forecast_hour == 0;

    if (s_send_now || (due && s_forecast_hour != hour)) {
        bool on_demand = s_send_now;
        s_send_now = false;
        if (send_forecast(now) == REPORT_FAILED) {
            s_send_now = on_demand; // still owed once the VEN is running again
            return 0;
        }
        if (due)
            s_forecast_hour = hour;
    }

    if (due && s_actual_hour != hour) {
        if (send_actual(hour - 3600) == REPORT_FAILED)
            return 0;
        s_actual_hour = hour;
    }

    time_t next = (s_forecast_hour == hour ? hour + 3600 : hour) + REPORT_DELAY_S;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_st.next_report_ts = next;
    xSemaphoreGive(s_lock);

    now = time(NULL);
    return next > now ? pdMS_TO_TICKS((next - now) * 1000) : 0;
}

static TickType_t step(void)
{
    switch (s_st.state) {
    case STATE_DISABLED:
        return portMAX_DELAY;
    case STATE_WAIT_TIME_SYNC:
        // Before SNTP syncs the clock reads 1970 and every timestamp would be wrong.
        if (time(NULL) < 1700000000)
            return pdMS_TO_TICKS(2000);
        set_state(STATE_AUTHENTICATING);
        return 0;
    case STATE_AUTHENTICATING:
        return step_authenticate();
    case STATE_REGISTERING_VEN:
        return step_register_ven();
    case STATE_CONNECTING_MQTT:
        return step_connect_mqtt();
    case STATE_DISCOVERING:
        return step_discover();
    case STATE_RUNNING:
        return step_running();
    case STATE_BACKOFF: {
        time_t now = time(NULL);
        if (now < s_st.retry_ts)
            return pdMS_TO_TICKS((s_st.retry_ts - now) * 1000);
        set_state(s_resume_state);
        return 0;
    }
    }
    return portMAX_DELAY;
}

// ── Task ──────────────────────────────────────────────────────────────────────

// (Re)start the state machine from the saved config.
static void restart(void)
{
    mqtt_stop();
    free(s_token);
    s_token = NULL;

    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_run = s_cfg;
    s_st.last_error[0]  = '\0';
    s_st.source_used[0] = '\0';
    xSemaphoreGive(s_lock);

    s_backoff_s         = BACKOFF_INITIAL_S;
    s_forecast_hour     = 0; // post a forecast as soon as the VEN is running again
    s_send_now          = false;
    s_retried_after_404 = false;
    s_resync_pending    = false;

    if (!s_run.enabled) {
        set_state(STATE_DISABLED);
    } else if (!s_run.vtn_base_url[0]) {
        set_last_error("VTN base URL is not set");
        set_state(STATE_DISABLED);
    } else {
        set_state(STATE_WAIT_TIME_SYNC);
    }
}

static void handle_cmd(ven_cmd_t cmd)
{
    switch (cmd) {
    case CMD_RESTART:
        restart();
        break;
    case CMD_RESET:
        cached_ids_clear();
        activity_add("info", "Registration reset, cached IDs cleared");
        restart();
        break;
    case CMD_SEND_NOW:
        if (s_st.state == STATE_DISABLED) {
            activity_add("info", "Send now ignored: OpenADR is disabled");
        } else {
            s_send_now = true;
            if (s_st.state != STATE_RUNNING)
                activity_add("info", "Forecast will be sent once the VEN is running");
        }
        break;
    case CMD_WAKE:
        break;
    }
}

// React to what the MQTT task has flagged since the last step.
static void handle_mqtt_flags(void)
{
    ven_state_t state = s_st.state == STATE_BACKOFF ? s_resume_state : s_st.state;
    bool past_mqtt = state == STATE_DISCOVERING || state == STATE_RUNNING;

    // The broker connection was re-established: subscriptions are gone, and
    // anything may have changed while we were away. Re-run registration, which
    // leads back through the subscriptions and discovery (contract recovery 3).
    if (past_mqtt && s_mqtt_connected && s_mqtt_conn_gen != s_sub_gen) {
        s_resync_pending = false;
        activity_add("mqtt", "Reconnected, re-subscribing and resyncing");
        set_state(STATE_REGISTERING_VEN);
        return;
    }

    if (s_resync_pending) {
        s_resync_pending = false;
        if (s_st.state == STATE_RUNNING) {
            set_state(STATE_DISCOVERING);
        } else if (s_st.state == STATE_BACKOFF) {
            // Something changed on the VTN; it may be what we were waiting for.
            xSemaphoreTake(s_lock, portMAX_DELAY);
            s_st.retry_ts = 0;
            xSemaphoreGive(s_lock);
        }
    }
}

static void ven_task(void *arg)
{
    restart();

    for (;;) {
        handle_mqtt_flags();

        TickType_t wait = step();
        if (wait > pdMS_TO_TICKS(MAX_STEP_WAIT_MS) && s_st.state != STATE_DISABLED)
            wait = pdMS_TO_TICKS(MAX_STEP_WAIT_MS);

        ven_cmd_t cmd;
        if (xQueueReceive(s_queue, &cmd, wait) == pdTRUE)
            handle_cmd(cmd);
    }
}

// ── Public API ────────────────────────────────────────────────────────────────

static void post_cmd(ven_cmd_t cmd)
{
    if (s_queue)
        xQueueSend(s_queue, &cmd, pdMS_TO_TICKS(100));
}

esp_err_t openadr_ven_start(void)
{
    s_lock  = xSemaphoreCreateMutex();
    s_queue = xQueueCreate(8, sizeof(ven_cmd_t));
    if (!s_lock || !s_queue)
        return ESP_ERR_NO_MEM;

    config_load(&s_cfg);

    if (xTaskCreate(ven_task, "openadr_ven", VEN_TASK_STACK, NULL, VEN_TASK_PRIO, NULL) != pdPASS)
        return ESP_ERR_NO_MEM;
    return ESP_OK;
}

char *openadr_ven_config_json(void)
{
    cJSON *root = cJSON_CreateObject();

    xSemaphoreTake(s_lock, portMAX_DELAY);
    cJSON_AddBoolToObject(root, "enabled", s_cfg.enabled);
    cJSON_AddStringToObject(root, "vtn_base_url", s_cfg.vtn_base_url);
    cJSON_AddStringToObject(root, "client_id", s_cfg.client_id);
    cJSON_AddBoolToObject(root, "secret_set", s_cfg.client_secret[0] != '\0');
    cJSON_AddStringToObject(root, "ven_name", s_cfg.ven_name);
    cJSON_AddStringToObject(root, "forecast_source", s_cfg.forecast_source);
    cJSON_AddStringToObject(root, "mqtt_host_override", s_cfg.mqtt_host_override);
    xSemaphoreGive(s_lock);

    char *json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    return json;
}

// Copy a string field from the request if present. False if it is the wrong
// type or too long.
static bool take_str(const cJSON *body, const char *key, char *out, size_t len)
{
    const cJSON *item = cJSON_GetObjectItem(body, key);
    if (!item)
        return true;
    if (!cJSON_IsString(item) || strlen(item->valuestring) >= len)
        return false;
    strlcpy(out, item->valuestring, len);
    return true;
}

esp_err_t openadr_ven_config_update(const cJSON *body)
{
    if (!cJSON_IsObject(body))
        return ESP_ERR_INVALID_ARG;

    xSemaphoreTake(s_lock, portMAX_DELAY);
    ven_config_t cfg = s_cfg;
    xSemaphoreGive(s_lock);

    const cJSON *enabled = cJSON_GetObjectItem(body, "enabled");
    if (enabled) {
        if (!cJSON_IsBool(enabled))
            return ESP_ERR_INVALID_ARG;
        cfg.enabled = cJSON_IsTrue(enabled);
    }

    char secret[sizeof(cfg.client_secret)] = "";
    if (!take_str(body, "vtn_base_url", cfg.vtn_base_url, sizeof(cfg.vtn_base_url)) ||
        !take_str(body, "client_id", cfg.client_id, sizeof(cfg.client_id)) ||
        !take_str(body, "client_secret", secret, sizeof(secret)) ||
        !take_str(body, "ven_name", cfg.ven_name, sizeof(cfg.ven_name)) ||
        !take_str(body, "forecast_source", cfg.forecast_source, sizeof(cfg.forecast_source)) ||
        !take_str(body, "mqtt_host_override", cfg.mqtt_host_override, sizeof(cfg.mqtt_host_override)))
        return ESP_ERR_INVALID_ARG;

    if (secret[0]) // an empty secret keeps the stored one
        strlcpy(cfg.client_secret, secret, sizeof(cfg.client_secret));
    if (!cfg.ven_name[0])
        default_ven_name(cfg.ven_name, sizeof(cfg.ven_name));
    if (strcmp(cfg.forecast_source, "net") != 0 && strcmp(cfg.forecast_source, "gross") != 0)
        return ESP_ERR_INVALID_ARG;
    // The VTN is reached over plain HTTP; there is no TLS setup here.
    if (cfg.vtn_base_url[0] && strncmp(cfg.vtn_base_url, "http://", 7) != 0)
        return ESP_ERR_INVALID_ARG;

    esp_err_t err = config_save(&cfg);
    if (err != ESP_OK)
        return err;

    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_cfg = cfg;
    xSemaphoreGive(s_lock);

    post_cmd(CMD_RESTART);
    return ESP_OK;
}

char *openadr_ven_status_json(void)
{
    cJSON *root = status_to_json();
    char *json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    return json;
}

void openadr_ven_send_now(void)
{
    post_cmd(CMD_SEND_NOW);
}

void openadr_ven_reset(void)
{
    post_cmd(CMD_RESET);
}
