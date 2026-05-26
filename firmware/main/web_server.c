#include "web_server.h"

#include <string.h>
#include <sys/stat.h>
#include <stdio.h>
#include <inttypes.h>
#include <dirent.h>
#include <math.h>

#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_spiffs.h"
#include "cJSON.h"

#include "managers/device_manager.h"
#include "managers/node_manager.h"
#include "power_logger.h"
#include "solar_forecast.h"
#include "consumption_forecast.h"
#include "matter_controller.h"
#include "ws_server.h"

#include "mbedtls/base64.h"
#include "mdns.h"

static const char *TAG = "web_server";

#define SPIFFS_BASE_PATH "/spiffs"
#define SPIFFS_LABEL "storage"
#define LFS_BASE_PATH "/littlefs"
#define FILE_READ_CHUNK 1024
#define FS_PATH_MAX 544
#define MAX_POST_BODY 1024
#define MAX_DER_CERT_LEN 600

static const char *mime_type_for(const char *path)
{
    const char *dot = strrchr(path, '.');
    if (!dot)
        return "application/octet-stream";
    if (strcmp(dot, ".html") == 0)
        return "text/html";
    if (strcmp(dot, ".css") == 0)
        return "text/css";
    if (strcmp(dot, ".js") == 0)
        return "application/javascript";
    if (strcmp(dot, ".json") == 0)
        return "application/json";
    if (strcmp(dot, ".svg") == 0)
        return "image/svg+xml";
    if (strcmp(dot, ".png") == 0)
        return "image/png";
    if (strcmp(dot, ".ico") == 0)
        return "image/x-icon";
    if (strcmp(dot, ".woff2") == 0)
        return "font/woff2";
    return "application/octet-stream";
}

static esp_err_t send_file(httpd_req_t *req, const char *fs_path)
{
    FILE *f = fopen(fs_path, "r");
    if (!f)
    {
        httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "Not found");
        return ESP_FAIL;
    }
    httpd_resp_set_type(req, mime_type_for(fs_path));
    char buf[FILE_READ_CHUNK];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0)
    {
        if (httpd_resp_send_chunk(req, buf, n) != ESP_OK)
        {
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
    if (!text)
    {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "OOM");
        return ESP_FAIL;
    }
    if (status == 201)
        httpd_resp_set_status(req, "201 Created");
    httpd_resp_set_type(req, "application/json");
    esp_err_t err = httpd_resp_sendstr(req, text);
    free(text);
    return err;
}

static esp_err_t devices_get_handler(httpd_req_t *req)
{
    char *json = device_manager_get_all_json();
    if (!json)
    {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "OOM");
        return ESP_FAIL;
    }
    httpd_resp_set_type(req, "application/json");
    esp_err_t err = httpd_resp_sendstr(req, json);
    free(json);
    return err;
}

/// @brief This method's job is to distill the full device/endpoint JSON down to a simpler list of endpoints
/// @param req
/// @return
static esp_err_t devices_simple_get_handler(httpd_req_t *req)
{
    // Load all the devices.
    //
    char *full_json = device_manager_get_all_json();
    if (!full_json)
    {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "OOM");
        return ESP_FAIL;
    }
    cJSON *full = cJSON_Parse(full_json);
    free(full_json);
    if (!full)
    {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Parse failed");
        return ESP_FAIL;
    }

    cJSON *result = cJSON_CreateObject();
    cJSON *out_arr = cJSON_AddArrayToObject(result, "devices");

    cJSON *dev_arr = cJSON_GetObjectItemCaseSensitive(full, "devices");
    cJSON *dev;

    // This loop will handle simple devices, with basic composed device support.
    // If the device is a Bridge, the individual bridged nodes would be exposed.
    // Devices made up of multiple compound/composed/leaf nodes would require more complex handling, and are not currently supported.
    //
    cJSON_ArrayForEach(dev, dev_arr)
    {
        cJSON *node_id_j = cJSON_GetObjectItemCaseSensitive(dev, "nodeId");
        uint64_t node_id = node_id_j ? (uint64_t)node_id_j->valuedouble : 0;

        cJSON *endpoints = cJSON_GetObjectItemCaseSensitive(dev, "endpoints");

        bool has_electrical_sensor = false;
        uint16_t endpoint_id = 0;

        cJSON *ep;
        cJSON_ArrayForEach(ep, endpoints)
        {
            cJSON *deviceTypes = cJSON_GetObjectItemCaseSensitive(ep, "deviceTypes");

            cJSON *deviceType;
            cJSON_ArrayForEach(deviceType, deviceTypes)
            {
                if ((int)deviceType->valuedouble == 0x0510) {
                    has_electrical_sensor = true;
                    endpoint_id = (uint16_t)cJSON_GetObjectItemCaseSensitive(ep, "endpointId")->valuedouble;
                    break;
                }
            }
        }

        bool has_solar_power = false;
        uint16_t solar_endpoint_id = 0;
        cJSON_ArrayForEach(ep, endpoints)
        {
            cJSON *deviceTypes = cJSON_GetObjectItemCaseSensitive(ep, "deviceTypes");
            cJSON *deviceType;
            cJSON_ArrayForEach(deviceType, deviceTypes)
            {
                if ((int)deviceType->valuedouble == 0x0017) {
                    has_solar_power = true;
                    solar_endpoint_id = (uint16_t)cJSON_GetObjectItemCaseSensitive(ep, "endpointId")->valuedouble;
                    break;
                }
            }
        }

        cJSON *simple = cJSON_CreateObject();
        cJSON_AddNumberToObject(simple, "nodeId", (double)node_id);
        cJSON_AddNumberToObject(simple, "endpointId", (double)(has_solar_power ? solar_endpoint_id : endpoint_id));
        cJSON_AddStringToObject(simple, "label", "device name");
        cJSON_AddBoolToObject(simple, "hasElectricalSensor", has_electrical_sensor);
        cJSON_AddBoolToObject(simple, "hasSolarPower", has_solar_power);
        cJSON_AddItemToArray(out_arr, simple);
    }
    cJSON_Delete(full);

    return send_json(req, result, 200);
}

static esp_err_t device_endpoint_put_handler(httpd_req_t *req)
{
    uint64_t node_id = 0;
    unsigned int endpoint_id = 0;
    if (sscanf(req->uri, "/api/devices/%" SCNu64 "/endpoints/%u", &node_id, &endpoint_id) != 2)
    {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid URI");
        return ESP_FAIL;
    }

    if (req->content_len <= 0 || req->content_len > MAX_POST_BODY)
    {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid body");
        return ESP_FAIL;
    }
    char body[MAX_POST_BODY + 1];
    int received = 0;
    while (received < (int)req->content_len)
    {
        int r = httpd_req_recv(req, body + received, req->content_len - received);
        if (r <= 0)
        {
            httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Recv failed");
            return ESP_FAIL;
        }
        received += r;
    }
    body[received] = '\0';

    cJSON *root = cJSON_Parse(body);
    if (!root)
    {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Bad JSON");
        return ESP_FAIL;
    }
    cJSON *included_json = cJSON_GetObjectItemCaseSensitive(root, "included");
    if (!cJSON_IsBool(included_json))
    {
        cJSON_Delete(root);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing included");
        return ESP_FAIL;
    }
    bool included = cJSON_IsTrue(included_json);
    cJSON_Delete(root);

    esp_err_t err = device_manager_set_endpoint_included(node_id, (uint16_t)endpoint_id, included);
    if (err == ESP_ERR_NOT_FOUND)
    {
        httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "Not found");
        return ESP_FAIL;
    }
    if (err != ESP_OK)
    {
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
    ESP_LOGI(TAG, "Commissioning request received");
    if (req->content_len <= 0 || req->content_len > MAX_POST_BODY)
    {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid body");
        return ESP_FAIL;
    }
    char body[MAX_POST_BODY + 1];
    int received = 0;
    while (received < (int)req->content_len)
    {
        int r = httpd_req_recv(req, body + received, req->content_len - received);
        if (r <= 0)
        {
            httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Recv failed");
            return ESP_FAIL;
        }
        received += r;
    }
    body[received] = '\0';

    cJSON *root = cJSON_Parse(body);
    if (!root)
    {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Bad JSON");
        return ESP_FAIL;
    }
    cJSON *payload_item = cJSON_GetObjectItemCaseSensitive(root, "onboardingPayload");
    if (!cJSON_IsString(payload_item))
    {
        cJSON_Delete(root);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing onboardingPayload");
        return ESP_FAIL;
    }
    // Copy the payload string before freeing the JSON tree.
    char payload[256];
    strncpy(payload, payload_item->valuestring, sizeof(payload) - 1);
    payload[sizeof(payload) - 1] = '\0';
    cJSON_Delete(root);

    ESP_LOGI(TAG, "Beginning OnNetwork commissioning using payload: %s", payload);

    uint64_t commissioned_node_id = 0;
    esp_err_t err = matter_controller_commission_on_network(payload, &commissioned_node_id);

    if (err == ESP_ERR_INVALID_ARG)
    {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid onboarding payload");
        return ESP_FAIL;
    }
    if (err == ESP_ERR_TIMEOUT)
    {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Commissioning timed out");
        return ESP_FAIL;
    }
    if (err != ESP_OK)
    {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Commissioning failed");
        return ESP_FAIL;
    }

    cJSON *resp = cJSON_CreateObject();
    cJSON_AddNumberToObject(resp, "nodeId", (double)commissioned_node_id);
    return send_json(req, resp, 200);
}

static esp_err_t controller_unpair_post_handler(httpd_req_t *req)
{
    if (req->content_len <= 0 || req->content_len > MAX_POST_BODY)
    {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid body");
        return ESP_FAIL;
    }
    char body[MAX_POST_BODY + 1];
    int received = 0;
    while (received < (int)req->content_len)
    {
        int r = httpd_req_recv(req, body + received, req->content_len - received);
        if (r <= 0)
        {
            httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Recv failed");
            return ESP_FAIL;
        }
        received += r;
    }
    body[received] = '\0';

    cJSON *root = cJSON_Parse(body);
    if (!root)
    {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Bad JSON");
        return ESP_FAIL;
    }
    cJSON *node_item = cJSON_GetObjectItemCaseSensitive(root, "nodeId");
    if (!cJSON_IsNumber(node_item))
    {
        cJSON_Delete(root);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing nodeId");
        return ESP_FAIL;
    }
    uint64_t node_id = (uint64_t)node_item->valuedouble;
    cJSON_Delete(root);

    esp_err_t err = matter_controller_remove_node(node_id);
    if (err == ESP_ERR_TIMEOUT)
    {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Unpair timed out");
        return ESP_FAIL;
    }
    if (err != ESP_OK)
    {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Unpair failed");
        return ESP_FAIL;
    }

    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{}");
    return ESP_OK;
}

static esp_err_t device_delete_handler(httpd_req_t *req)
{
    uint64_t node_id = 0;
    if (sscanf(req->uri, "/api/devices/%" SCNu64, &node_id) != 1)
    {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid URI");
        return ESP_FAIL;
    }

    esp_err_t err = matter_controller_remove_node(node_id);
    if (err != ESP_OK && err != ESP_ERR_NOT_FOUND)
    {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Unpair failed");
        return ESP_FAIL;
    }

    err = device_manager_remove_device(node_id);
    if (err != ESP_OK)
    {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Remove failed");
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

    cJSON *root = cJSON_CreateObject();
    cJSON *array = cJSON_AddArrayToObject(root, "services");

    if (err == ESP_OK && results)
    {
        for (mdns_result_t *r = results; r; r = r->next)
        {
            cJSON *svc = cJSON_CreateObject();
            if (r->hostname)
                cJSON_AddStringToObject(svc, "host", r->hostname);
            if (r->instance_name)
                cJSON_AddStringToObject(svc, "name", r->instance_name);
            cJSON_AddNumberToObject(svc, "port", r->port);

            // First IPv4 address if present
            for (mdns_ip_addr_t *a = r->addr; a; a = a->next)
            {
                if (a->addr.type == ESP_IPADDR_TYPE_V4)
                {
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
    if (sscanf(req->uri, "/api/devices/%" SCNu64 "/interrogate", &node_id) != 1)
    {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid URI");
        return ESP_FAIL;
    }

    esp_err_t err = matter_controller_interrogate_node(node_id);
    if (err == ESP_ERR_NOT_FOUND)
    {
        httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "Device not found");
        return ESP_FAIL;
    }
    if (err != ESP_OK)
    {
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
    if (err != ESP_OK)
    {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Factory reset failed");
        return ESP_FAIL;
    }
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{}");
    return ESP_OK;
}

static esp_err_t nodes_get_handler(httpd_req_t *req)
{
    char *json = node_manager_get_all_json();
    if (!json)
    {
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
    if (!last_slash || *(last_slash + 1) == '\0')
    {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid URI");
        return ESP_FAIL;
    }
    const char *node_id = last_slash + 1;

    if (req->content_len <= 0 || req->content_len > MAX_POST_BODY)
    {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid body");
        return ESP_FAIL;
    }
    char body[MAX_POST_BODY + 1];
    int received = 0;
    while (received < (int)req->content_len)
    {
        int r = httpd_req_recv(req, body + received, req->content_len - received);
        if (r <= 0)
        {
            httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Recv failed");
            return ESP_FAIL;
        }
        received += r;
    }
    body[received] = '\0';

    cJSON *root = cJSON_Parse(body);
    if (!root)
    {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Bad JSON");
        return ESP_FAIL;
    }
    cJSON *xj = cJSON_GetObjectItemCaseSensitive(root, "x");
    cJSON *yj = cJSON_GetObjectItemCaseSensitive(root, "y");
    if (!cJSON_IsNumber(xj) || !cJSON_IsNumber(yj))
    {
        cJSON_Delete(root);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing x/y");
        return ESP_FAIL;
    }
    float x = (float)xj->valuedouble;
    float y = (float)yj->valuedouble;

    char *settings_str = NULL;
    cJSON *settings_j = cJSON_GetObjectItemCaseSensitive(root, "settings");
    if (cJSON_IsObject(settings_j))
        settings_str = cJSON_PrintUnformatted(settings_j);
    cJSON_Delete(root);

    esp_err_t err = node_manager_upsert(node_id, x, y, settings_str);
    free(settings_str);
    if (err != ESP_OK)
    {
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
    if (!slash)
    {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid URI");
        return ESP_FAIL;
    }
    char node_id[64];
    size_t id_len = (size_t)(slash - p);
    if (id_len == 0 || id_len >= sizeof(node_id))
    {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid node id");
        return ESP_FAIL;
    }
    memcpy(node_id, p, id_len);
    node_id[id_len] = '\0';

    if (req->content_len <= 0 || req->content_len > MAX_POST_BODY)
    {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid body");
        return ESP_FAIL;
    }
    char body[MAX_POST_BODY + 1];
    int received = 0;
    while (received < (int)req->content_len)
    {
        int r = httpd_req_recv(req, body + received, req->content_len - received);
        if (r <= 0)
        {
            httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Recv failed");
            return ESP_FAIL;
        }
        received += r;
    }
    body[received] = '\0';

    esp_err_t err = node_manager_update_settings(node_id, body);
    if (err == ESP_ERR_INVALID_ARG)
    {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid JSON");
        return ESP_FAIL;
    }
    if (err != ESP_OK)
    {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Persist failed");
        return ESP_FAIL;
    }
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, "{}");
}

static esp_err_t node_delete_handler(httpd_req_t *req)
{
    const char *last_slash = strrchr(req->uri, '/');
    if (!last_slash || *(last_slash + 1) == '\0')
    {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid URI");
        return ESP_FAIL;
    }
    const char *node_id = last_slash + 1;

    esp_err_t err = node_manager_delete(node_id);
    if (err == ESP_ERR_NOT_SUPPORTED)
    {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Node cannot be deleted");
        return ESP_FAIL;
    }
    if (err != ESP_OK)
    {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Delete failed");
        return ESP_FAIL;
    }
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, "{}");
}

static esp_err_t device_name_put_handler(httpd_req_t *req)
{
    uint64_t node_id = 0;
    if (sscanf(req->uri, "/api/devices/%" SCNu64 "/name", &node_id) != 1)
    {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid URI");
        return ESP_FAIL;
    }

    if (req->content_len <= 0 || req->content_len > MAX_POST_BODY)
    {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid body");
        return ESP_FAIL;
    }
    char body[MAX_POST_BODY + 1];
    int received = 0;
    while (received < (int)req->content_len)
    {
        int r = httpd_req_recv(req, body + received, req->content_len - received);
        if (r <= 0)
        {
            httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Recv failed");
            return ESP_FAIL;
        }
        received += r;
    }
    body[received] = '\0';

    cJSON *root = cJSON_Parse(body);
    if (!root)
    {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Bad JSON");
        return ESP_FAIL;
    }
    cJSON *name_json = cJSON_GetObjectItemCaseSensitive(root, "name");
    if (!cJSON_IsString(name_json))
    {
        cJSON_Delete(root);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing name");
        return ESP_FAIL;
    }
    const char *name = name_json->valuestring;
    esp_err_t err = device_manager_set_device_name(node_id, name, strlen(name));
    cJSON_Delete(root);

    if (err == ESP_ERR_NOT_FOUND)
    {
        httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "Device not found");
        return ESP_FAIL;
    }
    if (err != ESP_OK)
    {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Update failed");
        return ESP_FAIL;
    }
    device_manager_persist();

    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{}");
    return ESP_OK;
}

static esp_err_t data_grid_get_handler(httpd_req_t *req)
{
    char date[16] = {0};
    size_t qlen = httpd_req_get_url_query_len(req);
    if (qlen > 0 && qlen < 32)
    {
        char query[32];
        if (httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK)
            httpd_query_key_value(query, "date", date, sizeof(date));
    }
    if (!date[0])
    {
        time_t now = time(NULL);
        struct tm tm_info;
        localtime_r(&now, &tm_info);
        strftime(date, sizeof(date), "%Y-%m-%d", &tm_info);
    }

    char *json = power_logger_day_json(date);
    if (!json)
    {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "OOM");
        return ESP_FAIL;
    }
    httpd_resp_set_type(req, "application/json");
    esp_err_t err = httpd_resp_sendstr(req, json);
    free(json);
    return err;
}

static esp_err_t topology_grid_put_handler(httpd_req_t *req)
{
    if (req->content_len <= 0 || req->content_len > MAX_POST_BODY)
    {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid body");
        return ESP_FAIL;
    }
    char body[MAX_POST_BODY + 1];
    int received = 0;
    while (received < (int)req->content_len)
    {
        int r = httpd_req_recv(req, body + received, req->content_len - received);
        if (r <= 0)
        {
            httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Recv failed");
            return ESP_FAIL;
        }
        received += r;
    }
    body[received] = '\0';

    cJSON *req_json = cJSON_Parse(body);
    if (!req_json)
    {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Bad JSON");
        return ESP_FAIL;
    }
    cJSON *matter_node_id_j = cJSON_GetObjectItemCaseSensitive(req_json, "nodeId");
    cJSON *matter_ep_id_j   = cJSON_GetObjectItemCaseSensitive(req_json, "endpointId");
    cJSON *label_j          = cJSON_GetObjectItemCaseSensitive(req_json, "label");
    if (!cJSON_IsNumber(matter_node_id_j) || !cJSON_IsNumber(matter_ep_id_j) || !cJSON_IsString(label_j))
    {
        cJSON_Delete(req_json);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing nodeId/endpointId/label");
        return ESP_FAIL;
    }
    double matter_node_id = matter_node_id_j->valuedouble;
    double matter_ep_id   = matter_ep_id_j->valuedouble;
    const char *label     = label_j->valuestring;

    // Locate the consumer_unit node to derive the grid meter's position
    float cu_x = 0.0f, cu_y = 0.0f;
    char *all_json = node_manager_get_all_json();
    if (all_json)
    {
        cJSON *all = cJSON_Parse(all_json);
        free(all_json);
        if (all)
        {
            cJSON *nodes_arr = cJSON_GetObjectItemCaseSensitive(all, "nodes");
            cJSON *n;
            cJSON_ArrayForEach(n, nodes_arr)
            {
                cJSON *id_j2 = cJSON_GetObjectItemCaseSensitive(n, "id");
                if (cJSON_IsString(id_j2) && strcmp(id_j2->valuestring, "consumer_unit") == 0)
                {
                    cJSON *xj = cJSON_GetObjectItemCaseSensitive(n, "x");
                    cJSON *yj = cJSON_GetObjectItemCaseSensitive(n, "y");
                    if (cJSON_IsNumber(xj)) cu_x = (float)xj->valuedouble;
                    if (cJSON_IsNumber(yj)) cu_y = (float)yj->valuedouble;
                    break;
                }
            }
            cJSON_Delete(all);
        }
    }

    float node_x = cu_x - 220.0f;
    float node_y = cu_y;

    cJSON *settings = cJSON_CreateObject();
    cJSON_AddStringToObject(settings, "label", label);
    cJSON_AddStringToObject(settings, "type", "device");
    cJSON_AddNumberToObject(settings, "nodeId", matter_node_id);
    cJSON_AddNumberToObject(settings, "endpointId", matter_ep_id);
    char *settings_str = cJSON_PrintUnformatted(settings);
    cJSON_Delete(settings);

    esp_err_t err = node_manager_upsert("grid_meter", node_x, node_y, settings_str);
    free(settings_str);
    if (err != ESP_OK)
    {
        cJSON_Delete(req_json);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Node persist failed");
        return ESP_FAIL;
    }

    const char *edge_id = "grid_meter-power-out-consumer_unit-grid";
    err = node_manager_upsert_edge(edge_id, "grid_meter", "consumer_unit", "power-out", "grid");
    cJSON_Delete(req_json);
    if (err != ESP_OK)
    {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Edge persist failed");
        return ESP_FAIL;
    }

    cJSON *resp = cJSON_CreateObject();

    cJSON *node_obj = cJSON_AddObjectToObject(resp, "node");
    cJSON_AddStringToObject(node_obj, "id", "grid_meter");
    cJSON_AddNumberToObject(node_obj, "x", node_x);
    cJSON_AddNumberToObject(node_obj, "y", node_y);
    cJSON *resp_settings = cJSON_AddObjectToObject(node_obj, "settings");
    cJSON_AddStringToObject(resp_settings, "label", label);
    cJSON_AddStringToObject(resp_settings, "type", "device");
    cJSON_AddNumberToObject(resp_settings, "nodeId", matter_node_id);
    cJSON_AddNumberToObject(resp_settings, "endpointId", matter_ep_id);

    cJSON *edge_obj = cJSON_AddObjectToObject(resp, "edge");
    cJSON_AddStringToObject(edge_obj, "id", edge_id);
    cJSON_AddStringToObject(edge_obj, "source", "grid_meter");
    cJSON_AddStringToObject(edge_obj, "sourceHandle", "power-out");
    cJSON_AddStringToObject(edge_obj, "target", "consumer_unit");
    cJSON_AddStringToObject(edge_obj, "targetHandle", "grid");

    return send_json(req, resp, 200);
}

static esp_err_t topology_solar_put_handler(httpd_req_t *req)
{
    if (req->content_len <= 0 || req->content_len > MAX_POST_BODY)
    {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid body");
        return ESP_FAIL;
    }
    char body[MAX_POST_BODY + 1];
    int received = 0;
    while (received < (int)req->content_len)
    {
        int r = httpd_req_recv(req, body + received, req->content_len - received);
        if (r <= 0)
        {
            httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Recv failed");
            return ESP_FAIL;
        }
        received += r;
    }
    body[received] = '\0';

    cJSON *req_json = cJSON_Parse(body);
    if (!req_json)
    {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Bad JSON");
        return ESP_FAIL;
    }
    cJSON *matter_node_id_j = cJSON_GetObjectItemCaseSensitive(req_json, "nodeId");
    cJSON *matter_ep_id_j   = cJSON_GetObjectItemCaseSensitive(req_json, "endpointId");
    cJSON *label_j          = cJSON_GetObjectItemCaseSensitive(req_json, "label");
    if (!cJSON_IsNumber(matter_node_id_j) || !cJSON_IsNumber(matter_ep_id_j) || !cJSON_IsString(label_j))
    {
        cJSON_Delete(req_json);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing nodeId/endpointId/label");
        return ESP_FAIL;
    }
    double matter_node_id = matter_node_id_j->valuedouble;
    double matter_ep_id   = matter_ep_id_j->valuedouble;
    const char *label     = label_j->valuestring;

    float cu_x = 0.0f, cu_y = 0.0f;
    char *all_json = node_manager_get_all_json();
    if (all_json)
    {
        cJSON *all = cJSON_Parse(all_json);
        free(all_json);
        if (all)
        {
            cJSON *nodes_arr = cJSON_GetObjectItemCaseSensitive(all, "nodes");
            cJSON *n;
            cJSON_ArrayForEach(n, nodes_arr)
            {
                cJSON *id_j2 = cJSON_GetObjectItemCaseSensitive(n, "id");
                if (cJSON_IsString(id_j2) && strcmp(id_j2->valuestring, "consumer_unit") == 0)
                {
                    cJSON *xj = cJSON_GetObjectItemCaseSensitive(n, "x");
                    cJSON *yj = cJSON_GetObjectItemCaseSensitive(n, "y");
                    if (cJSON_IsNumber(xj)) cu_x = (float)xj->valuedouble;
                    if (cJSON_IsNumber(yj)) cu_y = (float)yj->valuedouble;
                    break;
                }
            }
            cJSON_Delete(all);
        }
    }

    float node_x = cu_x + 220.0f;
    float node_y = cu_y;

    cJSON *settings = cJSON_CreateObject();
    cJSON_AddStringToObject(settings, "label", label);
    cJSON_AddStringToObject(settings, "type", "device");
    cJSON_AddNumberToObject(settings, "nodeId", matter_node_id);
    cJSON_AddNumberToObject(settings, "endpointId", matter_ep_id);
    char *settings_str = cJSON_PrintUnformatted(settings);
    cJSON_Delete(settings);

    esp_err_t err = node_manager_upsert("solar_inverter", node_x, node_y, settings_str);
    free(settings_str);
    if (err != ESP_OK)
    {
        cJSON_Delete(req_json);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Node persist failed");
        return ESP_FAIL;
    }

    const char *edge_id = "solar_inverter-power-out-consumer_unit-solar_input";
    err = node_manager_upsert_edge(edge_id, "solar_inverter", "consumer_unit", "power-out", "solar_input");
    cJSON_Delete(req_json);
    if (err != ESP_OK)
    {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Edge persist failed");
        return ESP_FAIL;
    }

    cJSON *resp = cJSON_CreateObject();

    cJSON *node_obj = cJSON_AddObjectToObject(resp, "node");
    cJSON_AddStringToObject(node_obj, "id", "solar_inverter");
    cJSON_AddNumberToObject(node_obj, "x", node_x);
    cJSON_AddNumberToObject(node_obj, "y", node_y);
    cJSON *resp_settings = cJSON_AddObjectToObject(node_obj, "settings");
    cJSON_AddStringToObject(resp_settings, "label", label);
    cJSON_AddStringToObject(resp_settings, "type", "device");
    cJSON_AddNumberToObject(resp_settings, "nodeId", matter_node_id);
    cJSON_AddNumberToObject(resp_settings, "endpointId", matter_ep_id);

    cJSON *edge_obj = cJSON_AddObjectToObject(resp, "edge");
    cJSON_AddStringToObject(edge_obj, "id", edge_id);
    cJSON_AddStringToObject(edge_obj, "source", "solar_inverter");
    cJSON_AddStringToObject(edge_obj, "sourceHandle", "power-out");
    cJSON_AddStringToObject(edge_obj, "target", "consumer_unit");
    cJSON_AddStringToObject(edge_obj, "targetHandle", "solar_input");

    return send_json(req, resp, 200);
}

static esp_err_t test_generate_sample_data_handler(httpd_req_t *req)
{
    char date[16] = {0};
    size_t qlen = httpd_req_get_url_query_len(req);
    if (qlen > 0 && qlen < 32) {
        char query[32];
        if (httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK)
            httpd_query_key_value(query, "date", date, sizeof(date));
    }
    if (!date[0]) { httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing date"); return ESP_FAIL; }

    struct tm tm_info = {0};
    if (!strptime(date, "%Y-%m-%d", &tm_info)) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid date");
        return ESP_FAIL;
    }
    tm_info.tm_hour = 0; tm_info.tm_min = 0; tm_info.tm_sec = 0;
    time_t midnight = mktime(&tm_info);

    char path[64];
    snprintf(path, sizeof(path), "/littlefs/grid-%s", date);

    FILE *f = fopen(path, "wb");
    if (!f) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Cannot create file");
        return ESP_FAIL;
    }

    // Seed with a hash of the date so the same date always produces the same shape.
    unsigned int seed = 0;
    for (const char *c = date; *c; c++) seed = seed * 31 + (unsigned char)*c;
    srand(seed);

    for (int m = 0; m < 1440; m++) {
        double hour    = m / 60.0;
        double base    = 400.0;
        double morning = (hour >= 7.0  && hour < 9.0)  ? 1800.0 * sin(M_PI * (hour - 7.0)  / 2.0) : 0.0;
        double evening = (hour >= 17.0 && hour < 21.0) ? 2500.0 * sin(M_PI * (hour - 17.0) / 4.0) : 0.0;
        double solar   = (hour >= 9.0  && hour < 17.0) ? 3000.0 * sin(M_PI * (hour - 9.0)  / 8.0) : 0.0;
        double noise   = (double)(rand() % 301) - 150.0;
        double power_w = base + morning + evening - solar + noise;

        power_record_t rec = {
            .unix_minute = (uint32_t)(midnight + m * 60),
            .power_mw    = (int32_t)(power_w * 1000.0),
        };
        fwrite(&rec, sizeof(rec), 1, f);
    }
    fclose(f);

    ESP_LOGI(TAG, "Generated 1440 sample records for %s", date);
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, "{}");
}

static esp_err_t test_rollup_hourly_handler(httpd_req_t *req)
{
    char date[16] = {0};
    size_t qlen = httpd_req_get_url_query_len(req);
    if (qlen > 0 && qlen < 32) {
        char query[32];
        if (httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK)
            httpd_query_key_value(query, "date", date, sizeof(date));
    }
    if (!date[0]) { httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing date"); return ESP_FAIL; }

    esp_err_t err = power_logger_rollup_hourly(date);
    if (err == ESP_ERR_NOT_FOUND) { httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "No minute data for date"); return ESP_FAIL; }
    if (err != ESP_OK) { httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Rollup failed"); return ESP_FAIL; }
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, "{}");
}

static esp_err_t test_consumption_forecast_compute_handler(httpd_req_t *req)
{
    char date[16] = {0};
    size_t qlen = httpd_req_get_url_query_len(req);
    if (qlen > 0 && qlen < 32) {
        char query[32];
        if (httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK)
            httpd_query_key_value(query, "date", date, sizeof(date));
    }
    if (!date[0]) { httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing date"); return ESP_FAIL; }

    esp_err_t err = consumption_forecast_compute(date);
    if (err == ESP_ERR_NOT_FOUND) { httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "No prior same-weekday data"); return ESP_FAIL; }
    if (err != ESP_OK) { httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Compute failed"); return ESP_FAIL; }
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, "{}");
}

static esp_err_t forecast_consumption_get_handler(httpd_req_t *req)
{
    char date[16] = {0};
    size_t qlen = httpd_req_get_url_query_len(req);
    if (qlen > 0 && qlen < 32) {
        char query[32];
        if (httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK)
            httpd_query_key_value(query, "date", date, sizeof(date));
    }
    if (!date[0]) {
        // Default to tomorrow
        time_t now = time(NULL);
        struct tm tm_info;
        localtime_r(&now, &tm_info);
        tm_info.tm_hour = 0;
        tm_info.tm_min  = 0;
        tm_info.tm_sec  = 0;
        tm_info.tm_mday += 1;
        mktime(&tm_info);
        strftime(date, sizeof(date), "%Y-%m-%d", &tm_info);
    }

    char *json = consumption_forecast_json(date);
    if (!json) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "OOM");
        return ESP_FAIL;
    }
    httpd_resp_set_type(req, "application/json");
    esp_err_t err = httpd_resp_sendstr(req, json);
    free(json);
    return err;
}

static esp_err_t forecast_solar_fetch_handler(httpd_req_t *req)
{
    cJSON *forecast = NULL;
    esp_err_t err = solar_forecast_fetch_tomorrow(&forecast);
    if (err != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Forecast fetch failed");
        return ESP_FAIL;
    }

    // Compute tomorrow's date then run (or refresh) the consumption forecast.
    time_t now = time(NULL);
    struct tm tm_info;
    localtime_r(&now, &tm_info);
    tm_info.tm_hour = 0; tm_info.tm_min = 0; tm_info.tm_sec = 0;
    tm_info.tm_mday += 1;
    mktime(&tm_info);
    char tomorrow[11];
    strftime(tomorrow, sizeof(tomorrow), "%Y-%m-%d", &tm_info);

    esp_err_t cf_err = consumption_forecast_compute(tomorrow);
    if (cf_err != ESP_OK && cf_err != ESP_ERR_NOT_FOUND)
        ESP_LOGW(TAG, "consumption_forecast_compute failed: 0x%x", cf_err);

    return send_json(req, forecast, 200);
}

static esp_err_t forecast_surplus_get_handler(httpd_req_t *req)
{
    char date[16] = {0};
    size_t qlen = httpd_req_get_url_query_len(req);
    if (qlen > 0 && qlen < 32) {
        char query[32];
        if (httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK)
            httpd_query_key_value(query, "date", date, sizeof(date));
    }
    if (!date[0]) {
        time_t now = time(NULL);
        struct tm tm_info;
        localtime_r(&now, &tm_info);
        tm_info.tm_hour = 0; tm_info.tm_min = 0; tm_info.tm_sec = 0;
        tm_info.tm_mday += 1;
        mktime(&tm_info);
        strftime(date, sizeof(date), "%Y-%m-%d", &tm_info);
    }

    // Load solar forecast (hourly binary)
    char solar_path[64];
    snprintf(solar_path, sizeof(solar_path), "/littlefs/solar-forecast-%s", date);
    FILE *sf = fopen(solar_path, "rb");
    if (!sf) {
        httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "Solar forecast not found for date");
        return ESP_FAIL;
    }
    int32_t solar_mw[24] = {0};
    uint32_t solar_ts[24] = {0};
    {
        power_record_t rec;
        while (fread(&rec, sizeof(rec), 1, sf) == 1) {
            struct tm t; time_t ts = (time_t)rec.unix_minute;
            localtime_r(&ts, &t);
            int h = t.tm_hour;
            solar_mw[h]  = rec.power_mw;
            solar_ts[h]  = rec.unix_minute;
        }
    }
    fclose(sf);

    // Load consumption forecast (hourly binary)
    char con_path[64];
    snprintf(con_path, sizeof(con_path), "/littlefs/consumption-forecast-%s", date);
    FILE *cf = fopen(con_path, "rb");
    if (!cf) {
        httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "Consumption forecast not found for date");
        return ESP_FAIL;
    }
    int32_t con_mw[24] = {0};
    {
        power_record_t rec;
        while (fread(&rec, sizeof(rec), 1, cf) == 1) {
            struct tm t; time_t ts = (time_t)rec.unix_minute;
            localtime_r(&ts, &t);
            con_mw[t.tm_hour] = rec.power_mw;
        }
    }
    fclose(cf);

    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "date", date);
    cJSON *slots = cJSON_AddArrayToObject(root, "slots");
    for (int h = 0; h < 24; h++) {
        cJSON *obj = cJSON_CreateObject();
        cJSON_AddNumberToObject(obj, "hour_ts",  (double)solar_ts[h]);
        cJSON_AddNumberToObject(obj, "surplus_w", (solar_mw[h] - con_mw[h]) / 1000.0);
        cJSON_AddItemToArray(slots, obj);
    }
    return send_json(req, root, 200);
}

static esp_err_t edge_post_handler(httpd_req_t *req)
{
    if (req->content_len <= 0 || req->content_len > MAX_POST_BODY)
    {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid body");
        return ESP_FAIL;
    }
    char body[MAX_POST_BODY + 1];
    int received = 0;
    while (received < (int)req->content_len)
    {
        int r = httpd_req_recv(req, body + received, req->content_len - received);
        if (r <= 0)
        {
            httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Recv failed");
            return ESP_FAIL;
        }
        received += r;
    }
    body[received] = '\0';

    cJSON *root = cJSON_Parse(body);
    if (!root)
    {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Bad JSON");
        return ESP_FAIL;
    }
    cJSON *id_j = cJSON_GetObjectItemCaseSensitive(root, "id");
    cJSON *src_j = cJSON_GetObjectItemCaseSensitive(root, "source");
    cJSON *tgt_j = cJSON_GetObjectItemCaseSensitive(root, "target");
    if (!cJSON_IsString(id_j) || !cJSON_IsString(src_j) || !cJSON_IsString(tgt_j))
    {
        cJSON_Delete(root);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing id/source/target");
        return ESP_FAIL;
    }
    cJSON *sh_j = cJSON_GetObjectItemCaseSensitive(root, "sourceHandle");
    cJSON *th_j = cJSON_GetObjectItemCaseSensitive(root, "targetHandle");

    esp_err_t err = node_manager_upsert_edge(
        id_j->valuestring, src_j->valuestring, tgt_j->valuestring,
        cJSON_IsString(sh_j) ? sh_j->valuestring : NULL,
        cJSON_IsString(th_j) ? th_j->valuestring : NULL);
    cJSON_Delete(root);

    if (err != ESP_OK)
    {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Persist failed");
        return ESP_FAIL;
    }
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{}");
    return ESP_OK;
}

static esp_err_t edge_delete_handler(httpd_req_t *req)
{
    const char *id = req->uri + strlen("/api/edges/");
    if (!*id)
    {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing edge id");
        return ESP_FAIL;
    }
    esp_err_t err = node_manager_delete_edge(id);
    if (err != ESP_OK)
    {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Delete failed");
        return ESP_FAIL;
    }
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{}");
    return ESP_OK;
}

static esp_err_t debug_files_list_handler(httpd_req_t *req)
{
    DIR *dir = opendir(LFS_BASE_PATH);
    if (!dir)
    {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Cannot open LittleFS");
        return ESP_FAIL;
    }

    cJSON *arr = cJSON_CreateArray();
    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL)
    {
        if (entry->d_type != DT_REG)
            continue;
        char full[FS_PATH_MAX];
        snprintf(full, sizeof(full), "%s/%s", LFS_BASE_PATH, entry->d_name);
        struct stat st;
        long size = (stat(full, &st) == 0) ? (long)st.st_size : -1;
        cJSON *obj = cJSON_CreateObject();
        cJSON_AddStringToObject(obj, "name", entry->d_name);
        cJSON_AddNumberToObject(obj, "size", size);
        cJSON_AddItemToArray(arr, obj);
    }
    closedir(dir);

    cJSON *root = cJSON_CreateObject();
    cJSON_AddItemToObject(root, "files", arr);
    return send_json(req, root, 200);
}

static esp_err_t debug_files_get_handler(httpd_req_t *req)
{
    const char *name = req->uri + strlen("/debug/files/");
    if (!*name || strchr(name, '/'))
    {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid filename");
        return ESP_FAIL;
    }
    char fs_path[FS_PATH_MAX];
    snprintf(fs_path, sizeof(fs_path), "%s/%s", LFS_BASE_PATH, name);
    httpd_resp_set_type(req, "text/plain");
    return send_file(req, fs_path);
}

static esp_err_t static_get_handler(httpd_req_t *req)
{
    char fs_path[FS_PATH_MAX];
    if (strcmp(req->uri, "/") == 0)
        snprintf(fs_path, sizeof(fs_path), "%s/index.html", SPIFFS_BASE_PATH);
    else
        snprintf(fs_path, sizeof(fs_path), "%s%s", SPIFFS_BASE_PATH, req->uri);

    struct stat st;
    if (stat(fs_path, &st) != 0)
    {
        snprintf(fs_path, sizeof(fs_path), "%s/index.html", SPIFFS_BASE_PATH);
        if (stat(fs_path, &st) != 0)
        {
            httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "Not found");
            return ESP_FAIL;
        }
    }
    return send_file(req, fs_path);
}

esp_err_t web_server_start(void)
{
    esp_vfs_spiffs_conf_t conf = {
        .base_path = SPIFFS_BASE_PATH,
        .partition_label = SPIFFS_LABEL,
        .max_files = 5,
        .format_if_mount_failed = false,
    };
    esp_err_t err = esp_vfs_spiffs_register(&conf);
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "SPIFFS mount failed: 0x%x", err);
        return err;
    }

    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.server_port = 80;
    config.lru_purge_enable = true;
    config.uri_match_fn = httpd_uri_match_wildcard;
    config.stack_size = 12288;
    config.max_uri_handlers = 40;
    config.max_resp_headers = 20;

    httpd_handle_t server = NULL;
    err = httpd_start(&server, &config);
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "httpd_start failed: 0x%x", err);
        return err;
    }

    const httpd_uri_t devices_get = {.uri = "/api/devices", .method = HTTP_GET, .handler = devices_get_handler};
    const httpd_uri_t devices_simple_get = {.uri = "/api/devices/simple", .method = HTTP_GET, .handler = devices_simple_get_handler};
    const httpd_uri_t device_delete = {.uri = "/api/devices/*", .method = HTTP_DELETE, .handler = device_delete_handler};
    const httpd_uri_t device_name_put = {.uri = "/api/devices/*/name", .method = HTTP_PUT, .handler = device_name_put_handler};
    const httpd_uri_t endpoint_put = {.uri = "/api/devices/*/endpoints/*", .method = HTTP_PUT, .handler = device_endpoint_put_handler};
    const httpd_uri_t commission_post = {.uri = "/controller/commission", .method = HTTP_POST, .handler = controller_commission_post_handler};
    const httpd_uri_t unpair_post = {.uri = "/controller/unpair", .method = HTTP_POST, .handler = controller_unpair_post_handler};
    const httpd_uri_t debug_mdns = {.uri = "/debug/mdns", .method = HTTP_GET, .handler = debug_mdns_get_handler};
    const httpd_uri_t device_interrogate = {.uri = "/api/devices/*/interrogate", .method = HTTP_POST, .handler = device_interrogate_post_handler};
    const httpd_uri_t factory_reset = {.uri = "/api/factory-reset", .method = HTTP_POST, .handler = factory_reset_post_handler};
    const httpd_uri_t nodes_get = {.uri = "/api/nodes", .method = HTTP_GET, .handler = nodes_get_handler};
    const httpd_uri_t node_settings_put = {.uri = "/api/nodes/*/settings", .method = HTTP_PUT, .handler = node_settings_put_handler};
    const httpd_uri_t node_put = {.uri = "/api/nodes/*", .method = HTTP_PUT, .handler = node_put_handler};
    const httpd_uri_t node_delete = {.uri = "/api/nodes/*", .method = HTTP_DELETE, .handler = node_delete_handler};
    const httpd_uri_t data_grid_get = {.uri = "/api/data/grid", .method = HTTP_GET, .handler = data_grid_get_handler};
    const httpd_uri_t topology_grid_put = {.uri = "/api/topology/grid", .method = HTTP_PUT, .handler = topology_grid_put_handler};
    const httpd_uri_t topology_solar_put = {.uri = "/api/topology/solar", .method = HTTP_PUT, .handler = topology_solar_put_handler};
    const httpd_uri_t edge_post = {.uri = "/api/edges", .method = HTTP_POST, .handler = edge_post_handler};
    const httpd_uri_t edge_delete = {.uri = "/api/edges/*", .method = HTTP_DELETE, .handler = edge_delete_handler};
    const httpd_uri_t forecast_solar_fetch = {.uri = "/api/forecast/solar/fetch", .method = HTTP_POST, .handler = forecast_solar_fetch_handler};
    const httpd_uri_t forecast_consumption_get = {.uri = "/api/forecast/consumption", .method = HTTP_GET, .handler = forecast_consumption_get_handler};
    const httpd_uri_t forecast_surplus_get = {.uri = "/api/forecast/surplus", .method = HTTP_GET, .handler = forecast_surplus_get_handler};
    const httpd_uri_t test_generate_sample_data = {.uri = "/api/test/generate-sample-data", .method = HTTP_POST, .handler = test_generate_sample_data_handler};
    const httpd_uri_t test_rollup_hourly = {.uri = "/api/test/rollup-hourly", .method = HTTP_POST, .handler = test_rollup_hourly_handler};
    const httpd_uri_t test_consumption_forecast_compute = {.uri = "/api/test/consumption-forecast/compute", .method = HTTP_POST, .handler = test_consumption_forecast_compute_handler};
    const httpd_uri_t debug_files_list = {.uri = "/debug/files", .method = HTTP_GET, .handler = debug_files_list_handler};
    const httpd_uri_t debug_files_get = {.uri = "/debug/files/*", .method = HTTP_GET, .handler = debug_files_get_handler};
    const httpd_uri_t static_files = {.uri = "/*", .method = HTTP_GET, .handler = static_get_handler};

    httpd_register_uri_handler(server, &devices_get);
    httpd_register_uri_handler(server, &devices_simple_get);
    httpd_register_uri_handler(server, &device_delete);
    httpd_register_uri_handler(server, &device_name_put);
    httpd_register_uri_handler(server, &endpoint_put);
    httpd_register_uri_handler(server, &commission_post);
    httpd_register_uri_handler(server, &unpair_post);
    httpd_register_uri_handler(server, &debug_mdns);
    httpd_register_uri_handler(server, &device_interrogate);
    httpd_register_uri_handler(server, &factory_reset);
    httpd_register_uri_handler(server, &nodes_get);
    httpd_register_uri_handler(server, &node_settings_put);
    httpd_register_uri_handler(server, &node_put);
    httpd_register_uri_handler(server, &node_delete);
    httpd_register_uri_handler(server, &data_grid_get);
    httpd_register_uri_handler(server, &topology_grid_put);
    httpd_register_uri_handler(server, &topology_solar_put);
    httpd_register_uri_handler(server, &edge_post);
    httpd_register_uri_handler(server, &edge_delete);
    httpd_register_uri_handler(server, &forecast_solar_fetch);
    httpd_register_uri_handler(server, &forecast_consumption_get);
    httpd_register_uri_handler(server, &forecast_surplus_get);
    httpd_register_uri_handler(server, &test_generate_sample_data);
    httpd_register_uri_handler(server, &test_rollup_hourly);
    httpd_register_uri_handler(server, &test_consumption_forecast_compute);
    httpd_register_uri_handler(server, &debug_files_list);
    httpd_register_uri_handler(server, &debug_files_get);

    ws_server_init(server);

    httpd_register_uri_handler(server, &static_files);

    ESP_LOGI(TAG, "Web server started on port 80");
    return ESP_OK;
}
