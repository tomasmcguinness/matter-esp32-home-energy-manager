#include "web_server.h"

#include <string.h>
#include <sys/stat.h>
#include <stdio.h>
#include <inttypes.h>

#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_spiffs.h"
#include "cJSON.h"

#include "settings_store.h"
#include "managers/device_manager.h"
#include "managers/node_config_manager.h"
#include "matter_controller.h"

#include "mbedtls/base64.h"
#include "mdns.h"

static const char *TAG = "web_server";

#define SPIFFS_BASE_PATH "/spiffs"
#define SPIFFS_LABEL     "storage"
#define FILE_READ_CHUNK  1024
#define FS_PATH_MAX      544
#define MAX_POST_BODY    1024
#define MAX_DER_CERT_LEN 600

static const char *mime_type_for(const char *path)
{
    const char *dot = strrchr(path, '.');
    if (!dot) return "application/octet-stream";
    if (strcmp(dot, ".html") == 0) return "text/html";
    if (strcmp(dot, ".css")  == 0) return "text/css";
    if (strcmp(dot, ".js")   == 0) return "application/javascript";
    if (strcmp(dot, ".json") == 0) return "application/json";
    if (strcmp(dot, ".svg")  == 0) return "image/svg+xml";
    if (strcmp(dot, ".png")  == 0) return "image/png";
    if (strcmp(dot, ".ico")  == 0) return "image/x-icon";
    if (strcmp(dot, ".woff2")== 0) return "font/woff2";
    return "application/octet-stream";
}

static esp_err_t send_file(httpd_req_t *req, const char *fs_path)
{
    FILE *f = fopen(fs_path, "r");
    if (!f) {
        httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "Not found");
        return ESP_FAIL;
    }
    httpd_resp_set_type(req, mime_type_for(fs_path));
    char buf[FILE_READ_CHUNK];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0) {
        if (httpd_resp_send_chunk(req, buf, n) != ESP_OK) {
            fclose(f);
            return ESP_FAIL;
        }
    }
    fclose(f);
    httpd_resp_send_chunk(req, NULL, 0);
    return ESP_OK;
}

static esp_err_t send_json(httpd_req_t *req, cJSON *root, int status)
{
    char *text = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!text) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "OOM");
        return ESP_FAIL;
    }
    if (status == 201) httpd_resp_set_status(req, "201 Created");
    httpd_resp_set_type(req, "application/json");
    esp_err_t err = httpd_resp_sendstr(req, text);
    free(text);
    return err;
}

static cJSON *settings_to_json(const settings_t *s)
{
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "name", s->name);
    return o;
}

static esp_err_t settings_get_handler(httpd_req_t *req)
{
    return send_json(req, settings_to_json(settings_store_get()), 200);
}

static esp_err_t settings_put_handler(httpd_req_t *req)
{
    if (req->content_len <= 0 || req->content_len > MAX_POST_BODY) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid body");
        return ESP_FAIL;
    }
    char buf[MAX_POST_BODY + 1];
    int received = 0;
    while (received < req->content_len) {
        int r = httpd_req_recv(req, buf + received, req->content_len - received);
        if (r <= 0) {
            httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Recv failed");
            return ESP_FAIL;
        }
        received += r;
    }
    buf[received] = '\0';

    cJSON *root = cJSON_Parse(buf);
    if (!root) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Bad JSON");
        return ESP_FAIL;
    }
    cJSON *name = cJSON_GetObjectItemCaseSensitive(root, "name");
    if (!cJSON_IsString(name)) {
        cJSON_Delete(root);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing fields");
        return ESP_FAIL;
    }
    settings_t updated;
    esp_err_t err = settings_store_update(name->valuestring, &updated);
    cJSON_Delete(root);
    if (err != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Persist failed");
        return ESP_FAIL;
    }
    return send_json(req, settings_to_json(&updated), 200);
}

static esp_err_t devices_get_handler(httpd_req_t *req)
{
    char *json = device_manager_get_all_json();
    if (!json) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "OOM");
        return ESP_FAIL;
    }
    httpd_resp_set_type(req, "application/json");
    esp_err_t err = httpd_resp_sendstr(req, json);
    free(json);
    return err;
}

static esp_err_t device_endpoint_put_handler(httpd_req_t *req)
{
    uint64_t node_id = 0;
    unsigned int endpoint_id = 0;
    if (sscanf(req->uri, "/api/devices/%" SCNu64 "/endpoints/%u", &node_id, &endpoint_id) != 2) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid URI");
        return ESP_FAIL;
    }

    if (req->content_len <= 0 || req->content_len > MAX_POST_BODY) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid body");
        return ESP_FAIL;
    }
    char body[MAX_POST_BODY + 1];
    int received = 0;
    while (received < (int)req->content_len) {
        int r = httpd_req_recv(req, body + received, req->content_len - received);
        if (r <= 0) {
            httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Recv failed");
            return ESP_FAIL;
        }
        received += r;
    }
    body[received] = '\0';

    cJSON *root = cJSON_Parse(body);
    if (!root) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Bad JSON");
        return ESP_FAIL;
    }
    cJSON *included_json = cJSON_GetObjectItemCaseSensitive(root, "included");
    if (!cJSON_IsBool(included_json)) {
        cJSON_Delete(root);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing included");
        return ESP_FAIL;
    }
    bool included = cJSON_IsTrue(included_json);
    cJSON_Delete(root);

    esp_err_t err = device_manager_set_endpoint_included(node_id, (uint16_t)endpoint_id, included);
    if (err == ESP_ERR_NOT_FOUND) {
        httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "Not found");
        return ESP_FAIL;
    }
    if (err != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Update failed");
        return ESP_FAIL;
    }
    device_manager_persist();

    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{}");
    return ESP_OK;
}

static esp_err_t controller_commission_post_handler(httpd_req_t *req)
{
    if (req->content_len <= 0 || req->content_len > MAX_POST_BODY) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid body");
        return ESP_FAIL;
    }
    char body[MAX_POST_BODY + 1];
    int received = 0;
    while (received < (int)req->content_len) {
        int r = httpd_req_recv(req, body + received, req->content_len - received);
        if (r <= 0) {
            httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Recv failed");
            return ESP_FAIL;
        }
        received += r;
    }
    body[received] = '\0';

    cJSON *root = cJSON_Parse(body);
    if (!root) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Bad JSON");
        return ESP_FAIL;
    }
    cJSON *payload_item = cJSON_GetObjectItemCaseSensitive(root, "onboardingPayload");
    if (!cJSON_IsString(payload_item)) {
        cJSON_Delete(root);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing onboardingPayload");
        return ESP_FAIL;
    }
    // Copy the payload string before freeing the JSON tree.
    char payload[256];
    strncpy(payload, payload_item->valuestring, sizeof(payload) - 1);
    payload[sizeof(payload) - 1] = '\0';
    cJSON_Delete(root);

    ESP_LOGI(TAG, "Payload: %s", payload);

    esp_err_t err = matter_controller_commission_on_network(payload);
    
    if (err == ESP_ERR_INVALID_ARG) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid onboarding payload");
        return ESP_FAIL;
    }
    if (err == ESP_ERR_TIMEOUT) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Commissioning timed out");
        return ESP_FAIL;
    }
    if (err != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Commissioning failed");
        return ESP_FAIL;
    }

    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{}");
    return ESP_OK;
}

static esp_err_t controller_unpair_post_handler(httpd_req_t *req)
{
    if (req->content_len <= 0 || req->content_len > MAX_POST_BODY) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid body");
        return ESP_FAIL;
    }
    char body[MAX_POST_BODY + 1];
    int received = 0;
    while (received < (int)req->content_len) {
        int r = httpd_req_recv(req, body + received, req->content_len - received);
        if (r <= 0) {
            httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Recv failed");
            return ESP_FAIL;
        }
        received += r;
    }
    body[received] = '\0';

    cJSON *root = cJSON_Parse(body);
    if (!root) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Bad JSON");
        return ESP_FAIL;
    }
    cJSON *node_item = cJSON_GetObjectItemCaseSensitive(root, "nodeId");
    if (!cJSON_IsNumber(node_item)) {
        cJSON_Delete(root);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing nodeId");
        return ESP_FAIL;
    }
    uint64_t node_id = (uint64_t)node_item->valuedouble;
    cJSON_Delete(root);

    esp_err_t err = matter_controller_remove_node(node_id);
    if (err == ESP_ERR_TIMEOUT) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Unpair timed out");
        return ESP_FAIL;
    }
    if (err != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Unpair failed");
        return ESP_FAIL;
    }

    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{}");
    return ESP_OK;
}

static esp_err_t debug_mdns_get_handler(httpd_req_t *req)
{
    mdns_result_t *results = NULL;
    // Browse for _http._tcp — broadly advertised, good multicast smoke test.
    // 3 s timeout, up to 20 results.
    esp_err_t err = mdns_query_ptr("_http", "_tcp", 3000, 20, &results);

    cJSON *root  = cJSON_CreateObject();
    cJSON *array = cJSON_AddArrayToObject(root, "services");

    if (err == ESP_OK && results) {
        for (mdns_result_t *r = results; r; r = r->next) {
            cJSON *svc = cJSON_CreateObject();
            if (r->hostname) cJSON_AddStringToObject(svc, "host", r->hostname);
            if (r->instance_name) cJSON_AddStringToObject(svc, "name", r->instance_name);
            cJSON_AddNumberToObject(svc, "port", r->port);

            // First IPv4 address if present
            for (mdns_ip_addr_t *a = r->addr; a; a = a->next) {
                if (a->addr.type == ESP_IPADDR_TYPE_V4) {
                    char ip[16];
                    snprintf(ip, sizeof(ip), IPSTR, IP2STR(&a->addr.u_addr.ip4));
                    cJSON_AddStringToObject(svc, "ip", ip);
                    break;
                }
            }
            cJSON_AddItemToArray(array, svc);
        }
        mdns_query_results_free(results);
    }

    cJSON_AddStringToObject(root, "status", err == ESP_OK ? "ok" : esp_err_to_name(err));
    return send_json(req, root, 200);
}

static esp_err_t device_interrogate_post_handler(httpd_req_t *req)
{
    uint64_t node_id = 0;
    if (sscanf(req->uri, "/api/devices/%" SCNu64 "/interrogate", &node_id) != 1) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid URI");
        return ESP_FAIL;
    }

    esp_err_t err = matter_controller_interrogate_node(node_id);
    if (err == ESP_ERR_NOT_FOUND) {
        httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "Device not found");
        return ESP_FAIL;
    }
    if (err != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Interrogation failed");
        return ESP_FAIL;
    }

    httpd_resp_set_status(req, "202 Accepted");
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{}");
    return ESP_OK;
}

static esp_err_t factory_reset_post_handler(httpd_req_t *req)
{
    esp_err_t err = matter_factory_reset();
    if (err != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Factory reset failed");
        return ESP_FAIL;
    }
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{}");
    return ESP_OK;
}

static esp_err_t nodes_get_handler(httpd_req_t *req)
{
    char *json = node_config_manager_get_all_json();
    if (!json) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "OOM");
        return ESP_FAIL;
    }
    httpd_resp_set_type(req, "application/json");
    esp_err_t err = httpd_resp_sendstr(req, json);
    free(json);
    return err;
}

static esp_err_t node_put_handler(httpd_req_t *req)
{
    // URI is /api/nodes/<id>
    const char *last_slash = strrchr(req->uri, '/');
    if (!last_slash || *(last_slash + 1) == '\0') {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid URI");
        return ESP_FAIL;
    }
    const char *node_id = last_slash + 1;

    if (req->content_len <= 0 || req->content_len > MAX_POST_BODY) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid body");
        return ESP_FAIL;
    }
    char body[MAX_POST_BODY + 1];
    int received = 0;
    while (received < (int)req->content_len) {
        int r = httpd_req_recv(req, body + received, req->content_len - received);
        if (r <= 0) {
            httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Recv failed");
            return ESP_FAIL;
        }
        received += r;
    }
    body[received] = '\0';

    cJSON *root = cJSON_Parse(body);
    if (!root) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Bad JSON");
        return ESP_FAIL;
    }
    cJSON *xj = cJSON_GetObjectItemCaseSensitive(root, "x");
    cJSON *yj = cJSON_GetObjectItemCaseSensitive(root, "y");
    if (!cJSON_IsNumber(xj) || !cJSON_IsNumber(yj)) {
        cJSON_Delete(root);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing x/y");
        return ESP_FAIL;
    }
    float x = (float)xj->valuedouble;
    float y = (float)yj->valuedouble;
    cJSON_Delete(root);

    esp_err_t err = node_config_manager_upsert(node_id, x, y);
    if (err != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Persist failed");
        return ESP_FAIL;
    }
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, "{}");
}

static esp_err_t node_settings_put_handler(httpd_req_t *req)
{
    // URI is /api/nodes/<id>/settings
    const char *p = req->uri + strlen("/api/nodes/");
    const char *slash = strchr(p, '/');
    if (!slash) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid URI");
        return ESP_FAIL;
    }
    char node_id[64];
    size_t id_len = (size_t)(slash - p);
    if (id_len == 0 || id_len >= sizeof(node_id)) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid node id");
        return ESP_FAIL;
    }
    memcpy(node_id, p, id_len);
    node_id[id_len] = '\0';

    if (req->content_len <= 0 || req->content_len > MAX_POST_BODY) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid body");
        return ESP_FAIL;
    }
    char body[MAX_POST_BODY + 1];
    int received = 0;
    while (received < (int)req->content_len) {
        int r = httpd_req_recv(req, body + received, req->content_len - received);
        if (r <= 0) {
            httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Recv failed");
            return ESP_FAIL;
        }
        received += r;
    }
    body[received] = '\0';

    esp_err_t err = node_config_manager_update_settings(node_id, body);
    if (err == ESP_ERR_INVALID_ARG) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid JSON");
        return ESP_FAIL;
    }
    if (err != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Persist failed");
        return ESP_FAIL;
    }
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, "{}");
}

static esp_err_t static_get_handler(httpd_req_t *req)
{
    char fs_path[FS_PATH_MAX];
    if (strcmp(req->uri, "/") == 0)
        snprintf(fs_path, sizeof(fs_path), "%s/index.html", SPIFFS_BASE_PATH);
    else
        snprintf(fs_path, sizeof(fs_path), "%s%s", SPIFFS_BASE_PATH, req->uri);

    struct stat st;
    if (stat(fs_path, &st) != 0) {
        snprintf(fs_path, sizeof(fs_path), "%s/index.html", SPIFFS_BASE_PATH);
        if (stat(fs_path, &st) != 0) {
            httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "Not found");
            return ESP_FAIL;
        }
    }
    return send_file(req, fs_path);
}

esp_err_t web_server_start(void)
{
    esp_vfs_spiffs_conf_t conf = {
        .base_path              = SPIFFS_BASE_PATH,
        .partition_label        = SPIFFS_LABEL,
        .max_files              = 5,
        .format_if_mount_failed = false,
    };
    esp_err_t err = esp_vfs_spiffs_register(&conf);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "SPIFFS mount failed: 0x%x", err);
        return err;
    }

    httpd_config_t config  = HTTPD_DEFAULT_CONFIG();
    config.server_port     = 80;
    config.lru_purge_enable = true;
    config.uri_match_fn    = httpd_uri_match_wildcard;
    config.stack_size      = 12288;
    config.max_uri_handlers = 15;

    httpd_handle_t server = NULL;
    err = httpd_start(&server, &config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "httpd_start failed: 0x%x", err);
        return err;
    }

    const httpd_uri_t settings_get      = { .uri = "/api/settings",              .method = HTTP_GET, .handler = settings_get_handler           };
    const httpd_uri_t settings_put      = { .uri = "/api/settings",              .method = HTTP_PUT, .handler = settings_put_handler           };
    const httpd_uri_t devices_get       = { .uri = "/api/devices",               .method = HTTP_GET, .handler = devices_get_handler            };
    const httpd_uri_t endpoint_put      = { .uri = "/api/devices/*/endpoints/*", .method = HTTP_PUT, .handler = device_endpoint_put_handler    };
    const httpd_uri_t commission_post   = { .uri = "/controller/commission", .method = HTTP_POST, .handler = controller_commission_post_handler };
    const httpd_uri_t unpair_post       = { .uri = "/controller/unpair",    .method = HTTP_POST, .handler = controller_unpair_post_handler     };
    const httpd_uri_t debug_mdns        = { .uri = "/debug/mdns",           .method = HTTP_GET,  .handler = debug_mdns_get_handler            };
    const httpd_uri_t device_interrogate = { .uri = "/api/devices/*/interrogate", .method = HTTP_POST, .handler = device_interrogate_post_handler };
    const httpd_uri_t factory_reset      = { .uri = "/api/factory-reset",         .method = HTTP_POST, .handler = factory_reset_post_handler       };
    const httpd_uri_t nodes_get          = { .uri = "/api/nodes",                 .method = HTTP_GET,  .handler = nodes_get_handler                };
    const httpd_uri_t node_settings_put  = { .uri = "/api/nodes/*/settings",      .method = HTTP_PUT,  .handler = node_settings_put_handler        };
    const httpd_uri_t node_put           = { .uri = "/api/nodes/*",               .method = HTTP_PUT,  .handler = node_put_handler                 };
    const httpd_uri_t static_files       = { .uri = "/*",                         .method = HTTP_GET,  .handler = static_get_handler               };

    httpd_register_uri_handler(server, &settings_get);
    httpd_register_uri_handler(server, &settings_put);
    httpd_register_uri_handler(server, &devices_get);
    httpd_register_uri_handler(server, &endpoint_put);
    httpd_register_uri_handler(server, &commission_post);
    httpd_register_uri_handler(server, &unpair_post);
    httpd_register_uri_handler(server, &debug_mdns);
    httpd_register_uri_handler(server, &device_interrogate);
    httpd_register_uri_handler(server, &factory_reset);
    httpd_register_uri_handler(server, &nodes_get);
    httpd_register_uri_handler(server, &node_settings_put);
    httpd_register_uri_handler(server, &node_put);
    httpd_register_uri_handler(server, &static_files);

    ESP_LOGI(TAG, "Web server started on port 80");
    return ESP_OK;
}
