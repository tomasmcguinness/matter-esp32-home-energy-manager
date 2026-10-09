#include "mcp_server.h"

#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <stdio.h>
#include <stdbool.h>
#include <ctype.h>
#include <time.h>
#include <dirent.h>
#include <sys/stat.h>

#include "esp_log.h"
#include "cJSON.h"

#include "managers/node_manager.h"
#include "power_logger.h"
#include "node_power_logger.h"
#include "tariff.h"
#include "solar_forecast.h"
#include "consumption_forecast.h"
#include "surplus_forecast.h"
#include "surplus_model.h"
#include "appliance_profile.h"
#include "scheduler.h"

static const char *TAG = "mcp_server";

#define MCP_MAX_BODY      8192
#define MCP_MAX_FILES     200
#define SD_BASE           "/sdcard"
#define JSONRPC_PARSE_ERROR      -32700
#define JSONRPC_INVALID_REQUEST  -32600
#define JSONRPC_METHOD_NOT_FOUND -32601
#define JSONRPC_INVALID_PARAMS   -32602

// Protocol revisions this server can speak, newest first. The client's requested
// revision is echoed when listed; otherwise the newest is offered.
static const char *const PROTOCOL_VERSIONS[] = { "2025-06-18", "2025-03-26", "2024-11-05" };

// ---- Tool results -----------------------------------------------------------

// What a tool hands back: a malloc'd string (JSON, or a plain message when
// is_error is set). NULL text means out of memory.
typedef struct {
    char *text;
    bool  is_error;
} tool_result_t;

static tool_result_t result_json(char *json)
{
    tool_result_t r = { json, false };
    return r;
}

static tool_result_t result_error(const char *msg)
{
    tool_result_t r = { strdup(msg), true };
    return r;
}

// ---- Argument helpers -------------------------------------------------------

static const char *arg_str(const cJSON *args, const char *key)
{
    const cJSON *j = cJSON_GetObjectItemCaseSensitive(args, key);
    return (cJSON_IsString(j) && j->valuestring[0]) ? j->valuestring : NULL;
}

static bool is_date(const char *s)
{
    if (!s || strlen(s) != 10) return false;
    for (int i = 0; i < 10; i++) {
        if (i == 4 || i == 7) { if (s[i] != '-') return false; }
        else if (!isdigit((unsigned char)s[i])) return false;
    }
    return true;
}

// The "date" argument, or the local date `default_offset` days from today when
// absent. Returns false (with *err set) when present but malformed.
static bool arg_date(const cJSON *args, int default_offset, char out[11], tool_result_t *err)
{
    const char *given = arg_str(args, "date");
    if (given) {
        if (!is_date(given)) {
            *err = result_error("date must be YYYY-MM-DD");
            return false;
        }
        memcpy(out, given, 11);
        return true;
    }
    time_t now = time(NULL);
    struct tm tm_info;
    localtime_r(&now, &tm_info);
    tm_info.tm_hour = 12;
    tm_info.tm_mday += default_offset;
    mktime(&tm_info);
    strftime(out, 11, "%Y-%m-%d", &tm_info);
    return true;
}

// ---- Tools ------------------------------------------------------------------

static tool_result_t tool_get_topology(const cJSON *args)
{
    return result_json(node_manager_get_all_json());
}

static tool_result_t tool_get_power_series(const cJSON *args)
{
    const char *node_id = arg_str(args, "node_id");
    if (!node_id) return result_error("node_id is required (ids come from get_topology)");
    tool_result_t err;
    char date[11];
    if (!arg_date(args, 0, date, &err)) return err;
    const char *res = arg_str(args, "resolution");
    bool hourly = !res || strcmp(res, "hourly") == 0;
    if (!hourly && strcmp(res, "minute") != 0) return result_error("resolution must be \"hourly\" or \"minute\"");

    char *json = node_power_logger_series_json(node_id, date, hourly);
    return json ? result_json(json) : result_error("node_id is not a valid node id");
}

static tool_result_t tool_get_daily_energy(const cJSON *args)
{
    int days = 7;
    const cJSON *d = cJSON_GetObjectItemCaseSensitive(args, "days");
    if (cJSON_IsNumber(d)) days = (int)d->valuedouble;
    if (days < 1) days = 1;
    if (days > 60) days = 60;
    return result_json(node_power_logger_daily_energy_json(days));
}

static tool_result_t tool_get_battery_source(const cJSON *args)
{
    return result_json(node_power_logger_battery_source_json());
}

static tool_result_t tool_get_forecast(const cJSON *args)
{
    const char *kind = arg_str(args, "kind");
    if (!kind) return result_error("kind is required: solar, consumption, surplus or surplus_model");
    if (strcmp(kind, "surplus_model") == 0) return result_json(surplus_model_json());

    // The solar forecast is stored for the current day; the other two are built for tomorrow.
    bool solar = strcmp(kind, "solar") == 0;
    tool_result_t err;
    char date[11];
    if (!arg_date(args, solar ? 0 : 1, date, &err)) return err;
    if (solar) return result_json(solar_forecast_hourly_json(date));
    if (strcmp(kind, "consumption") == 0) return result_json(consumption_forecast_json(date));
    if (strcmp(kind, "surplus") == 0) return result_json(surplus_forecast_json(date));
    return result_error("kind must be solar, consumption, surplus or surplus_model");
}

static tool_result_t tool_get_schedule(const cJSON *args)
{
    tool_result_t err;
    char date[11];
    if (!arg_date(args, 1, date, &err)) return err;
    return result_json(scheduler_json(date));
}

static tool_result_t tool_get_appliance_profiles(const cJSON *args)
{
    const char *node_id = arg_str(args, "node_id");
    return result_json(node_id ? appliance_profile_json(node_id) : appliance_profile_all_json());
}

static tool_result_t tool_get_tariff(const cJSON *args)
{
    tool_result_t err;
    char date[11];
    if (!arg_date(args, 0, date, &err)) return err;
    return result_json(tariff_day_json(date));
}

static tool_result_t tool_explain_battery_mix(const cJSON *args)
{
    tool_result_t err;
    char date[11];
    if (!arg_date(args, 0, date, &err)) return err;
    char *json = node_power_logger_battery_mix_trace_json(date);
    return json ? result_json(json) : result_error("date must be today or earlier");
}

static tool_result_t tool_get_stream_health(const cJSON *args)
{
    return result_json(node_power_logger_stream_health_json());
}

static tool_result_t tool_list_data_files(const cJSON *args)
{
    const char *prefix = arg_str(args, "prefix");
    const char *date = arg_str(args, "date");
    if (date && !is_date(date)) return result_error("date must be YYYY-MM-DD");

    DIR *dir = opendir(SD_BASE);
    if (!dir) return result_error("The SD card is not mounted");

    cJSON *root = cJSON_CreateObject();
    cJSON *arr = cJSON_AddArrayToObject(root, "files");
    int matched = 0;
    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL) {
        if (entry->d_type != DT_REG) continue;
        const char *name = entry->d_name;
        size_t len = strlen(name);
        if (prefix && strncmp(name, prefix, strlen(prefix)) != 0) continue;
        // Every per-day file ends in its date.
        if (date && (len < 10 || strcmp(name + len - 10, date) != 0)) continue;
        if (++matched > MCP_MAX_FILES) continue;

        char full[300];
        snprintf(full, sizeof(full), "%s/%s", SD_BASE, name);
        struct stat st;
        cJSON *obj = cJSON_CreateObject();
        cJSON_AddStringToObject(obj, "name", name);
        cJSON_AddNumberToObject(obj, "size", stat(full, &st) == 0 ? (double)st.st_size : -1);
        cJSON_AddItemToArray(arr, obj);
    }
    closedir(dir);
    cJSON_AddNumberToObject(root, "matched", matched);
    cJSON_AddBoolToObject(root, "truncated", matched > MCP_MAX_FILES);

    char *json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    return result_json(json);
}

static tool_result_t tool_recompute_days(const cJSON *args)
{
    const char *from = arg_str(args, "from_date");
    if (!is_date(from)) return result_error("from_date is required, as YYYY-MM-DD");
    int days = node_power_logger_recompute_from(from);
    if (days < 0) return result_error("from_date must be within the last 60 days");

    char body[64];
    snprintf(body, sizeof(body), "{\"days_rebuilt\":%d}", days);
    return result_json(strdup(body));
}

// Mirrors the nightly job for tomorrow: fetch the solar forecast, refit the
// surplus regression, refresh the consumption fallback, then derive the surplus.
static tool_result_t tool_refresh_forecast(const cJSON *args)
{
    cJSON *forecast = NULL;
    if (solar_forecast_fetch_tomorrow(&forecast) != ESP_OK)
        return result_error("The solar forecast could not be fetched");

    time_t now = time(NULL);
    struct tm tm_info;
    localtime_r(&now, &tm_info);
    tm_info.tm_hour = 12;
    tm_info.tm_mday += 1;
    mktime(&tm_info);
    char tomorrow[11];
    strftime(tomorrow, sizeof(tomorrow), "%Y-%m-%d", &tm_info);

    int usable_days = surplus_model_train(56);
    esp_err_t cf_err = consumption_forecast_compute(tomorrow);
    esp_err_t sf_err = surplus_forecast_compute(tomorrow);

    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "date", tomorrow);
    cJSON_AddNumberToObject(root, "surplus_model_usable_days", usable_days);
    cJSON_AddBoolToObject(root, "consumption_forecast_built", cf_err == ESP_OK);
    cJSON_AddBoolToObject(root, "surplus_forecast_built", sf_err == ESP_OK);
    cJSON_AddItemToObject(root, "solar_forecast", forecast);
    char *json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    return result_json(json);
}

// ---- Tool table -------------------------------------------------------------

typedef struct {
    const char *name;
    const char *description;
    const char *schema;     // JSON Schema for the arguments
    bool        read_only;
    tool_result_t (*call)(const cJSON *args);
} tool_t;

#define NO_ARGS   "{\"type\":\"object\",\"properties\":{}}"
#define DATE_PROP "\"date\":{\"type\":\"string\",\"description\":\"Local date, YYYY-MM-DD.\"}"

static const tool_t TOOLS[] = {
    { "get_topology",
      "The home's electrical topology: every node (grid meter, consumer unit, inverter, battery, PV strings, "
      "appliances) with its id, label, type and latest live readings, plus the edges between them. Call this "
      "first to learn the node ids the other tools take. Live values are raw Matter attributes (cluster 144 "
      "attribute 8 is active power in mW; cluster 47 attribute 12 is battery charge in half-percent).",
      NO_ARGS, true, tool_get_topology },
    { "get_power_series",
      "Logged power in watts for one node over one local day, as an array starting at midnight. Hourly "
      "averages by default; minute resolution returns up to 1440 values, so ask for it only when needed. "
      "The result states the sign convention.",
      "{\"type\":\"object\",\"properties\":{"
      "\"node_id\":{\"type\":\"string\",\"description\":\"Node id from get_topology, e.g. grid_meter or solar_inverter.\"},"
      DATE_PROP ","
      "\"resolution\":{\"type\":\"string\",\"enum\":[\"hourly\",\"minute\"]}},"
      "\"required\":[\"node_id\"]}",
      true, tool_get_power_series },
    { "get_daily_energy",
      "Energy per node per day (kWh, raw sign: + = into the node), with cost and each load's solar/grid "
      "split where available. Today is included and is partial.",
      "{\"type\":\"object\",\"properties\":{"
      "\"days\":{\"type\":\"integer\",\"minimum\":1,\"maximum\":60,\"description\":\"Days back from today, default 7.\"}}}",
      true, tool_get_daily_energy },
    { "get_battery_source",
      "Where the usable energy now held in the battery came from, as solar and grid percentages. "
      "known=false when the battery is at its reserve or the origin is not tracked.",
      NO_ARGS, true, tool_get_battery_source },
    { "get_forecast",
      "A stored hourly forecast: solar generation (defaults to today), household consumption or surplus "
      "(both default to tomorrow), or the coefficients of the trained surplus model.",
      "{\"type\":\"object\",\"properties\":{"
      "\"kind\":{\"type\":\"string\",\"enum\":[\"solar\",\"consumption\",\"surplus\",\"surplus_model\"]},"
      DATE_PROP "},\"required\":[\"kind\"]}",
      true, tool_get_forecast },
    { "get_schedule",
      "The suggested run window for each appliance on a day, from sliding its learned cycle across that "
      "day's surplus forecast. Defaults to tomorrow.",
      "{\"type\":\"object\",\"properties\":{" DATE_PROP "}}",
      true, tool_get_schedule },
    { "get_appliance_profiles",
      "Learned usage profiles (standby power, program power and length) for one appliance, or all of them "
      "when node_id is omitted.",
      "{\"type\":\"object\",\"properties\":{\"node_id\":{\"type\":\"string\"}}}",
      true, tool_get_appliance_profiles },
    { "get_tariff",
      "The resolved electricity price slots for a day. Defaults to today.",
      "{\"type\":\"object\",\"properties\":{" DATE_PROP "}}",
      true, tool_get_tariff },
    { "explain_battery_mix",
      "Hour-by-hour trace of the ledger behind get_battery_source for one day: the ledger it started from, "
      "then held energy and solar/grid percentages at the end of each hour. Use it to see when the mix "
      "changed and why. Defaults to today.",
      "{\"type\":\"object\",\"properties\":{" DATE_PROP "}}",
      true, tool_explain_battery_mix },
    { "get_stream_health",
      "Data-quality check for every logged node: how long since its last Matter report, and how complete "
      "today's minute file is. A large report age means the logged values are a frozen reading.",
      NO_ARGS, true, tool_get_stream_health },
    { "list_data_files",
      "Files on the SD card with their sizes, filtered by name prefix (e.g. node-, soc-, grid-, split-, "
      "cost-, solar-forecast-) and/or by the date they end in. Returns at most 200.",
      "{\"type\":\"object\",\"properties\":{\"prefix\":{\"type\":\"string\"}," DATE_PROP "}}",
      true, tool_list_data_files },
    { "recompute_days",
      "Discard the cached cost and solar/grid split for every day from from_date up to yesterday and rebuild "
      "them from the logged minute data. Use after a fix to the split or tariff. Raw data is not touched.",
      "{\"type\":\"object\",\"properties\":{"
      "\"from_date\":{\"type\":\"string\",\"description\":\"First day to rebuild, YYYY-MM-DD, at most 60 days back.\"}},"
      "\"required\":[\"from_date\"]}",
      false, tool_recompute_days },
    { "refresh_forecast",
      "Fetch tomorrow's solar forecast from the provider now, refit the surplus model and rebuild tomorrow's "
      "consumption and surplus forecasts, overwriting the stored ones. Takes several seconds.",
      NO_ARGS, false, tool_refresh_forecast },
};

#define TOOL_COUNT (sizeof(TOOLS) / sizeof(TOOLS[0]))

// ---- JSON-RPC ---------------------------------------------------------------

static esp_err_t send_text(httpd_req_t *req, cJSON *root)
{
    char *text = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!text) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Out of memory building the response");
        return ESP_FAIL;
    }
    httpd_resp_set_type(req, "application/json");
    esp_err_t err = httpd_resp_sendstr(req, text);
    free(text);
    return err;
}

// A response envelope carrying the request's id (null when it had none).
static cJSON *envelope(const cJSON *id)
{
    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "jsonrpc", "2.0");
    cJSON_AddItemToObject(root, "id", id ? cJSON_Duplicate(id, true) : cJSON_CreateNull());
    return root;
}

static esp_err_t send_rpc_error(httpd_req_t *req, const cJSON *id, int code, const char *message)
{
    cJSON *root = envelope(id);
    cJSON *err = cJSON_AddObjectToObject(root, "error");
    cJSON_AddNumberToObject(err, "code", code);
    cJSON_AddStringToObject(err, "message", message);
    return send_text(req, root);
}

static cJSON *initialize_result(const cJSON *params)
{
    const char *wanted = arg_str(params, "protocolVersion");
    const char *version = PROTOCOL_VERSIONS[0];
    for (size_t i = 0; wanted && i < sizeof(PROTOCOL_VERSIONS) / sizeof(PROTOCOL_VERSIONS[0]); i++)
        if (strcmp(wanted, PROTOCOL_VERSIONS[i]) == 0) version = PROTOCOL_VERSIONS[i];

    cJSON *result = cJSON_CreateObject();
    cJSON_AddStringToObject(result, "protocolVersion", version);
    cJSON *caps = cJSON_AddObjectToObject(result, "capabilities");
    cJSON_AddObjectToObject(caps, "tools");
    cJSON *info = cJSON_AddObjectToObject(result, "serverInfo");
    cJSON_AddStringToObject(info, "name", "home-energy-manager");
    cJSON_AddStringToObject(info, "version", "1.0.0");
    cJSON_AddStringToObject(result, "instructions",
        "Home Energy Manager for one house. Start with get_topology to learn the node ids. Dates are local "
        "to the house. Logged power keeps the meter's raw sign: + is power into a node, - is power out of it.");
    return result;
}

static cJSON *tools_list_result(void)
{
    cJSON *result = cJSON_CreateObject();
    cJSON *arr = cJSON_AddArrayToObject(result, "tools");
    for (size_t i = 0; i < TOOL_COUNT; i++) {
        cJSON *t = cJSON_CreateObject();
        cJSON_AddStringToObject(t, "name", TOOLS[i].name);
        cJSON_AddStringToObject(t, "description", TOOLS[i].description);
        cJSON_AddItemToObject(t, "inputSchema", cJSON_Parse(TOOLS[i].schema));
        cJSON *ann = cJSON_AddObjectToObject(t, "annotations");
        cJSON_AddBoolToObject(ann, "readOnlyHint", TOOLS[i].read_only);
        cJSON_AddBoolToObject(ann, "destructiveHint", false);
        cJSON_AddItemToArray(arr, t);
    }
    return result;
}

static esp_err_t handle_tools_call(httpd_req_t *req, const cJSON *id, const cJSON *params)
{
    const char *name = arg_str(params, "name");
    const tool_t *tool = NULL;
    for (size_t i = 0; name && i < TOOL_COUNT; i++)
        if (strcmp(name, TOOLS[i].name) == 0) tool = &TOOLS[i];
    if (!tool) return send_rpc_error(req, id, JSONRPC_INVALID_PARAMS, "Unknown tool");

    const cJSON *args = cJSON_GetObjectItemCaseSensitive(params, "arguments");
    ESP_LOGI(TAG, "tools/call %s", tool->name);
    tool_result_t r = tool->call(cJSON_IsObject(args) ? args : NULL);
    if (!r.text) {
        r.text = strdup("The device ran out of memory building the result");
        r.is_error = true;
    }

    cJSON *root = envelope(id);
    cJSON *result = cJSON_AddObjectToObject(root, "result");
    cJSON *content = cJSON_AddArrayToObject(result, "content");
    cJSON *item = cJSON_CreateObject();
    cJSON_AddStringToObject(item, "type", "text");
    cJSON_AddStringToObject(item, "text", r.text ? r.text : "");
    cJSON_AddItemToArray(content, item);
    cJSON_AddBoolToObject(result, "isError", r.is_error);
    free(r.text);
    return send_text(req, root);
}

// A browser sends Origin; a page served from anywhere but this device must not
// be able to drive the tools (DNS rebinding). Native MCP clients send none.
static bool origin_allowed(httpd_req_t *req)
{
    char origin[128], host[96];
    if (httpd_req_get_hdr_value_str(req, "Origin", origin, sizeof(origin)) != ESP_OK) return true;
    if (httpd_req_get_hdr_value_str(req, "Host", host, sizeof(host)) != ESP_OK) return false;
    const char *origin_host = strstr(origin, "://");
    return origin_host && strcasecmp(origin_host + 3, host) == 0;
}

static esp_err_t mcp_post_handler(httpd_req_t *req)
{
    if (!origin_allowed(req)) {
        httpd_resp_send_err(req, HTTPD_403_FORBIDDEN, "Origin not allowed");
        return ESP_FAIL;
    }
    if (req->content_len <= 0 || req->content_len > MCP_MAX_BODY) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "The request body is missing or too large");
        return ESP_FAIL;
    }

    char *body = malloc(req->content_len + 1);
    if (!body) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Out of memory");
        return ESP_FAIL;
    }
    int received = 0;
    while (received < (int)req->content_len) {
        int n = httpd_req_recv(req, body + received, req->content_len - received);
        if (n <= 0) {
            free(body);
            httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "The request body could not be read");
            return ESP_FAIL;
        }
        received += n;
    }
    body[received] = '\0';

    cJSON *msg = cJSON_Parse(body);
    free(body);
    if (!msg) return send_rpc_error(req, NULL, JSONRPC_PARSE_ERROR, "Parse error");

    const cJSON *id = cJSON_GetObjectItemCaseSensitive(msg, "id");
    const char *method = arg_str(msg, "method");
    const cJSON *params = cJSON_GetObjectItemCaseSensitive(msg, "params");
    esp_err_t err;

    if (!cJSON_IsObject(msg) || !method) {
        // Batches, and responses to requests this server never makes, land here.
        err = send_rpc_error(req, id, JSONRPC_INVALID_REQUEST, "Expected a single JSON-RPC request");
    } else if (!id) {
        // A notification (e.g. notifications/initialized): accepted, no body.
        httpd_resp_set_status(req, "202 Accepted");
        err = httpd_resp_send(req, NULL, 0);
    } else if (strcmp(method, "tools/call") == 0) {
        err = handle_tools_call(req, id, params);
    } else {
        cJSON *result = NULL;
        if (strcmp(method, "initialize") == 0) result = initialize_result(params);
        else if (strcmp(method, "tools/list") == 0) result = tools_list_result();
        else if (strcmp(method, "ping") == 0) result = cJSON_CreateObject();

        if (result) {
            cJSON *root = envelope(id);
            cJSON_AddItemToObject(root, "result", result);
            err = send_text(req, root);
        } else {
            err = send_rpc_error(req, id, JSONRPC_METHOD_NOT_FOUND, "Method not found");
        }
    }

    cJSON_Delete(msg);
    return err;
}

// No server-initiated stream is offered; without this the static catch-all
// would answer a client's probing GET with index.html.
static esp_err_t mcp_get_handler(httpd_req_t *req)
{
    httpd_resp_set_status(req, "405 Method Not Allowed");
    httpd_resp_set_hdr(req, "Allow", "POST");
    return httpd_resp_send(req, NULL, 0);
}

esp_err_t mcp_server_register(httpd_handle_t server)
{
    const httpd_uri_t mcp_post = {.uri = "/mcp", .method = HTTP_POST, .handler = mcp_post_handler};
    const httpd_uri_t mcp_get = {.uri = "/mcp", .method = HTTP_GET, .handler = mcp_get_handler};

    esp_err_t err = httpd_register_uri_handler(server, &mcp_post);
    if (err == ESP_OK) err = httpd_register_uri_handler(server, &mcp_get);
    if (err != ESP_OK) ESP_LOGE(TAG, "Failed to register /mcp: 0x%x", err);
    return err;
}
