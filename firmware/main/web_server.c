#include "web_server.h"

#include <string.h>
#include <sys/stat.h>
#include <stdio.h>
#include <inttypes.h>
#include <dirent.h>
#include <math.h>
#include <unistd.h>

#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_spiffs.h"
#include "cJSON.h"

#include "managers/device_manager.h"
#include "managers/node_manager.h"
#include "power_logger.h"
#include "node_power_logger.h"
#include "solar_forecast.h"
#include "consumption_forecast.h"
#include "surplus_forecast.h"
#include "surplus_model.h"
#include "appliance_profile.h"
#include "scheduler.h"
#include "sd_card.h"
#include "matter_controller.h"
#include "ws_server.h"

#include "mbedtls/base64.h"
#include "mdns.h"
#include "esp_timer.h"
#include "esp_random.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/sockets.h"
#include "lwip/netif.h"
#include "lwip/ip6_addr.h"
#include "lwip/icmp6.h"
#include "lwip/ip6.h"
#include "lwip/nd6.h"
#include "lwip/priv/nd6_priv.h"
#include "esp_netif.h"
#include "esp_netif_ip_addr.h"

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

/// @brief Returns endpoints matching a given Matter device type, excluding those already assigned
///        to a topology node. Intended for use by UI dropdowns (e.g. "Select Grid Sensor").
///        Query parameter: deviceTypeId=<decimal> (required).
///        Handles bridged/nested devices by inheriting the label from the nearest ancestor
///        BridgedNode endpoint when the leaf endpoint's own label is empty.
/// @param req
/// @return
static esp_err_t devices_endpoints_get_handler(httpd_req_t *req)
{
    // --- 1. Parse required deviceTypeId query param ---
    char qbuf[32] = {0};
    uint32_t filter_type = 0;
    bool has_filter = false;
    size_t qlen = httpd_req_get_url_query_len(req);
    if (qlen > 0 && qlen < sizeof(qbuf))
    {
        if (httpd_req_get_url_query_str(req, qbuf, sizeof(qbuf)) == ESP_OK)
        {
            char val[16] = {0};
            if (httpd_query_key_value(qbuf, "deviceTypeId", val, sizeof(val)) == ESP_OK)
            {
                filter_type = (uint32_t)strtoul(val, NULL, 0); // accepts decimal or 0x... hex
                has_filter = true;
            }
        }
    }
    if (!has_filter)
    {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing deviceTypeId");
        return ESP_FAIL;
    }

    // --- 2. Build in-use set from node_manager ---
    // Any node carrying settings.nodeId + settings.endpointId has already claimed that
    // Matter endpoint (grid_meter, solar_inverter, appliance_N, ...). An endpoint may be
    // assigned to at most one topology node regardless of its role, so exclude all of them.
#define MAX_IN_USE 16
    uint64_t used_node_ids[MAX_IN_USE];
    uint16_t used_ep_ids[MAX_IN_USE];
    int used_count = 0;

    char *nodes_json = node_manager_get_all_json();
    if (nodes_json)
    {
        cJSON *nodes_root = cJSON_Parse(nodes_json);
        free(nodes_json);
        if (nodes_root)
        {
            cJSON *nodes_arr = cJSON_GetObjectItemCaseSensitive(nodes_root, "nodes");
            cJSON *n;
            cJSON_ArrayForEach(n, nodes_arr)
            {
                cJSON *settings = cJSON_GetObjectItemCaseSensitive(n, "settings");
                if (!cJSON_IsObject(settings)) continue;
                cJSON *nid_j = cJSON_GetObjectItemCaseSensitive(settings, "nodeId");
                cJSON *eid_j = cJSON_GetObjectItemCaseSensitive(settings, "endpointId");
                if (!cJSON_IsNumber(nid_j) || !cJSON_IsNumber(eid_j)) continue;
                if (used_count < MAX_IN_USE)
                {
                    used_node_ids[used_count] = (uint64_t)nid_j->valuedouble;
                    used_ep_ids[used_count]   = (uint16_t)eid_j->valuedouble;
                    used_count++;
                }
            }
            cJSON_Delete(nodes_root);
        }
    }

    // --- 3. Load all devices and filter ---
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

    cJSON *result  = cJSON_CreateObject();
    cJSON *out_arr = cJSON_AddArrayToObject(result, "endpoints");
    cJSON *dev_arr = cJSON_GetObjectItemCaseSensitive(full, "devices");
    cJSON *dev;

    cJSON_ArrayForEach(dev, dev_arr)
    {
        cJSON *nid_j  = cJSON_GetObjectItemCaseSensitive(dev, "nodeId");
        if (!cJSON_IsNumber(nid_j)) continue;
        uint64_t node_id = (uint64_t)nid_j->valuedouble;

        // Prefer user-assigned name; fall back to product name.
        cJSON *name_j = cJSON_GetObjectItemCaseSensitive(dev, "name");
        const char *device_name = (cJSON_IsString(name_j) && name_j->valuestring[0])
                                  ? name_j->valuestring
                                  : cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(dev, "productName"));

        cJSON *endpoints = cJSON_GetObjectItemCaseSensitive(dev, "endpoints");
        cJSON *ep;
        cJSON_ArrayForEach(ep, endpoints)
        {
            // --- device type filter ---
            bool type_match = false;
            cJSON *dtypes = cJSON_GetObjectItemCaseSensitive(ep, "deviceTypes");
            cJSON *dt;
            cJSON_ArrayForEach(dt, dtypes)
            {
                if ((uint32_t)dt->valuedouble == filter_type) { type_match = true; break; }
            }
            if (!type_match) continue;

            cJSON *epid_j = cJSON_GetObjectItemCaseSensitive(ep, "endpointId");
            if (!cJSON_IsNumber(epid_j)) continue;
            uint16_t ep_id = (uint16_t)epid_j->valuedouble;

            // --- exclusion check ---
            bool in_use = false;
            for (int i = 0; i < used_count; i++)
            {
                if (used_node_ids[i] == node_id && used_ep_ids[i] == ep_id) { in_use = true; break; }
            }
            if (in_use) continue;

            // --- label: prefer own label, otherwise walk parent chain ---
            // For bridged/nested devices the meaningful label is often on the BridgedNode
            // ancestor, not on the leaf measurement endpoint.
            cJSON *label_j = cJSON_GetObjectItemCaseSensitive(ep, "label");
            const char *label = (cJSON_IsString(label_j) && label_j->valuestring[0])
                                ? label_j->valuestring : NULL;

            if (!label)
            {
                cJSON *parent_id_j = cJSON_GetObjectItemCaseSensitive(ep, "parentEndpointId");
                while (!label && cJSON_IsNumber(parent_id_j))
                {
                    uint16_t parent_ep_id = (uint16_t)parent_id_j->valuedouble;
                    cJSON *parent_ep;
                    parent_id_j = NULL; // will be updated if we find the parent
                    cJSON_ArrayForEach(parent_ep, endpoints)
                    {
                        cJSON *pid_j = cJSON_GetObjectItemCaseSensitive(parent_ep, "endpointId");
                        if (!cJSON_IsNumber(pid_j) || (uint16_t)pid_j->valuedouble != parent_ep_id) continue;
                        cJSON *plabel_j = cJSON_GetObjectItemCaseSensitive(parent_ep, "label");
                        if (cJSON_IsString(plabel_j) && plabel_j->valuestring[0])
                            label = plabel_j->valuestring;
                        parent_id_j = cJSON_GetObjectItemCaseSensitive(parent_ep, "parentEndpointId");
                        break;
                    }
                }
            }

            // --- build result entry ---
            cJSON *entry = cJSON_CreateObject();
            cJSON_AddNumberToObject(entry, "nodeId",     (double)node_id);
            cJSON_AddNumberToObject(entry, "endpointId", (double)ep_id);
            cJSON_AddStringToObject(entry, "label",      label ? label : "");
            cJSON_AddStringToObject(entry, "deviceName", device_name ? device_name : "");
            cJSON_AddItemToArray(out_arr, entry);
        }
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
        // The device is offline/unreachable, so RemoveFabric could not be sent.
        // Honour the delete anyway: forget the node locally so a dead device can
        // still be removed instead of being stuck forever re-subscribing.
        ESP_LOGW(TAG, "Unpair of node 0x%llx failed (0x%x); forgetting locally",
                 (unsigned long long)node_id, err);
        matter_controller_forget_node(node_id);
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

static esp_err_t data_node_get_handler(httpd_req_t *req)
{
    char node_id[48] = {0};
    char date[16]    = {0};

    size_t qlen = httpd_req_get_url_query_len(req);
    if (qlen > 0 && qlen < 96)
    {
        char query[96];
        if (httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK)
        {
            httpd_query_key_value(query, "id", node_id, sizeof(node_id));
            httpd_query_key_value(query, "date", date, sizeof(date));
        }
    }
    if (!node_id[0])
    {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing id");
        return ESP_FAIL;
    }
    if (!date[0])
    {
        time_t now = time(NULL);
        struct tm tm_info;
        localtime_r(&now, &tm_info);
        strftime(date, sizeof(date), "%Y-%m-%d", &tm_info);
    }

    // node_power_logger sanitises id/date before touching the filesystem.
    char *json = node_power_logger_day_json(node_id, date);
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

// Matter device type ids used to classify a hybrid inverter's child endpoints.
#define DEVICE_TYPE_POWER_SOURCE      0x0011
#define DEVICE_TYPE_ELECTRICAL_SENSOR 0x0510

// Walk the child endpoints (the "parts") of the chosen Solar Power endpoint and
// create a topology node + edge for each: a child carrying the Power Source device
// type is the battery (hangs off the inverter's "battery" handle); any other
// electrical-sensor child is a PV string (feeds the inverter's "dc_in" handle).
// Best-effort: failures to upsert an individual child are logged and skipped so
// the inverter itself still configures.
static void topology_solar_build_children(uint64_t matter_node_id, uint16_t solar_ep_id,
                                          float inv_x, float inv_y)
{
    char *full_json = device_manager_get_all_json();
    if (!full_json)
        return;
    cJSON *full = cJSON_Parse(full_json);
    free(full_json);
    if (!full)
        return;

    cJSON *dev_arr = cJSON_GetObjectItemCaseSensitive(full, "devices");
    cJSON *endpoints = NULL;
    cJSON *parts = NULL;
    cJSON *dev;
    cJSON_ArrayForEach(dev, dev_arr)
    {
        cJSON *nid_j = cJSON_GetObjectItemCaseSensitive(dev, "nodeId");
        if (!cJSON_IsNumber(nid_j) || (uint64_t)nid_j->valuedouble != matter_node_id)
            continue;
        endpoints = cJSON_GetObjectItemCaseSensitive(dev, "endpoints");
        cJSON *ep;
        cJSON_ArrayForEach(ep, endpoints)
        {
            cJSON *eid_j = cJSON_GetObjectItemCaseSensitive(ep, "endpointId");
            if (cJSON_IsNumber(eid_j) && (uint16_t)eid_j->valuedouble == solar_ep_id)
            {
                parts = cJSON_GetObjectItemCaseSensitive(ep, "parts");
                break;
            }
        }
        break;
    }

    if (cJSON_IsArray(parts))
    {
        int pv_index = 0;
        cJSON *part_id_j;
        cJSON_ArrayForEach(part_id_j, parts)
        {
            if (!cJSON_IsNumber(part_id_j))
                continue;
            uint16_t child_ep = (uint16_t)part_id_j->valuedouble;

            // Look up this child endpoint's device types to classify it.
            bool is_battery = false;
            bool is_sensor = false;
            cJSON *ep;
            cJSON_ArrayForEach(ep, endpoints)
            {
                cJSON *eid_j = cJSON_GetObjectItemCaseSensitive(ep, "endpointId");
                if (!cJSON_IsNumber(eid_j) || (uint16_t)eid_j->valuedouble != child_ep)
                    continue;
                cJSON *types = cJSON_GetObjectItemCaseSensitive(ep, "deviceTypes");
                cJSON *t;
                cJSON_ArrayForEach(t, types)
                {
                    int dt = (int)t->valuedouble;
                    if (dt == DEVICE_TYPE_POWER_SOURCE) is_battery = true;
                    if (dt == DEVICE_TYPE_ELECTRICAL_SENSOR) is_sensor = true;
                }
                break;
            }
            if (!is_battery && !is_sensor)
                continue; // not a metered child; skip

            char node_id[48];
            char edge_id[160];
            float cx, cy;
            cJSON *cs = cJSON_CreateObject();
            cJSON_AddNumberToObject(cs, "nodeId", (double)matter_node_id);
            cJSON_AddNumberToObject(cs, "endpointId", (double)child_ep);

            if (is_battery)
            {
                snprintf(node_id, sizeof(node_id), "battery_%u", (unsigned)child_ep);
                cJSON_AddStringToObject(cs, "label", "Battery");
                cJSON_AddStringToObject(cs, "type", "battery");
                cx = inv_x;
                cy = inv_y + 160.0f;
            }
            else
            {
                snprintf(node_id, sizeof(node_id), "pv_string_%u", (unsigned)child_ep);
                char pv_label[24];
                snprintf(pv_label, sizeof(pv_label), "PV String %d", pv_index + 1);
                cJSON_AddStringToObject(cs, "label", pv_label);
                cJSON_AddStringToObject(cs, "type", "pvString");
                cx = inv_x + 220.0f;
                cy = inv_y - 60.0f + (float)pv_index * 90.0f;
                pv_index++;
            }

            char *cs_str = cJSON_PrintUnformatted(cs);
            cJSON_Delete(cs);
            if (node_manager_upsert(node_id, cx, cy, cs_str) != ESP_OK)
            {
                ESP_LOGW(TAG, "Failed to upsert solar child node %s", node_id);
                free(cs_str);
                continue;
            }
            free(cs_str);

            esp_err_t eerr;
            if (is_battery)
            {
                // Battery hangs off the inverter; power flows either way.
                snprintf(edge_id, sizeof(edge_id), "solar_inverter-battery-%s-power-in", node_id);
                eerr = node_manager_upsert_edge(edge_id, "solar_inverter", node_id, "battery", "power-in");
            }
            else
            {
                snprintf(edge_id, sizeof(edge_id), "%s-power-out-solar_inverter-dc_in", node_id);
                eerr = node_manager_upsert_edge(edge_id, node_id, "solar_inverter", "power-out", "dc_in");
            }
            if (eerr != ESP_OK)
                ESP_LOGW(TAG, "Failed to upsert solar child edge for %s", node_id);
        }
    }

    cJSON_Delete(full);
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
    cJSON_AddStringToObject(settings, "type", "solarInverter");
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

    // A hybrid inverter exposes its PV strings and battery as child endpoints under
    // the Solar Power endpoint. Surface each as its own topology node wired to the
    // inverter: PV strings feed DC power in (electrical-sensor-only children), the
    // battery sits off the inverter (the child also carrying a Power Source device
    // type, 0x0011). EPM ActivePower for these endpoints already flows via the
    // wildcard-endpoint subscription, so the nodes render live once they exist.
    topology_solar_build_children((uint64_t)matter_node_id, (uint16_t)matter_ep_id, node_x, node_y);

    cJSON *resp = cJSON_CreateObject();

    cJSON *node_obj = cJSON_AddObjectToObject(resp, "node");
    cJSON_AddStringToObject(node_obj, "id", "solar_inverter");
    cJSON_AddNumberToObject(node_obj, "x", node_x);
    cJSON_AddNumberToObject(node_obj, "y", node_y);
    cJSON *resp_settings = cJSON_AddObjectToObject(node_obj, "settings");
    cJSON_AddStringToObject(resp_settings, "label", label);
    cJSON_AddStringToObject(resp_settings, "type", "solarInverter");
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
    snprintf(path, sizeof(path), "/sdcard/grid-%s", date);

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

// Runs the full nightly forecast pipeline now (solar fetch + consumption + surplus
// for the day ahead) — the same operation the 2 AM timer performs.
static esp_err_t test_run_daily_job_handler(httpd_req_t *req)
{
    esp_err_t err = solar_forecast_run_daily_job();
    if (err != ESP_OK) { httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Daily job failed"); return ESP_FAIL; }
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

static esp_err_t forecast_solar_get_handler(httpd_req_t *req)
{
    char date[16] = {0};
    size_t qlen = httpd_req_get_url_query_len(req);
    if (qlen > 0 && qlen < 32) {
        char query[32];
        if (httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK)
            httpd_query_key_value(query, "date", date, sizeof(date));
    }
    if (!date[0]) {
        // Default to today — the daily job stores the current day's forecast.
        time_t now = time(NULL);
        struct tm tm_info;
        localtime_r(&now, &tm_info);
        strftime(date, sizeof(date), "%Y-%m-%d", &tm_info);
    }

    char *json = solar_forecast_hourly_json(date);
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

    // Mirror the nightly job: refit the regression, refresh the consumption
    // fallback, then derive tomorrow's surplus.
    surplus_model_train(56);

    esp_err_t cf_err = consumption_forecast_compute(tomorrow);
    if (cf_err != ESP_OK && cf_err != ESP_ERR_NOT_FOUND)
        ESP_LOGW(TAG, "consumption_forecast_compute failed: 0x%x", cf_err);

    esp_err_t sf_err = surplus_forecast_compute(tomorrow);
    if (sf_err != ESP_OK && sf_err != ESP_ERR_NOT_FOUND)
        ESP_LOGW(TAG, "surplus_forecast_compute failed: 0x%x", sf_err);

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

    char *json = surplus_forecast_json(date);
    if (!json) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "OOM");
        return ESP_FAIL;
    }
    httpd_resp_set_type(req, "application/json");
    esp_err_t send_err = httpd_resp_sendstr(req, json);
    free(json);
    return send_err;
}

// Dump the trained surplus regression (slopes + per-weekday intercepts) for
// inspection.
static esp_err_t forecast_surplus_model_get_handler(httpd_req_t *req)
{
    char *json = surplus_model_json();
    if (!json) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "OOM");
        return ESP_FAIL;
    }
    httpd_resp_set_type(req, "application/json");
    esp_err_t send_err = httpd_resp_sendstr(req, json);
    free(json);
    return send_err;
}

// Refit the surplus regression from history now and report the usable-day count.
static esp_err_t test_surplus_model_train_handler(httpd_req_t *req)
{
    int days = surplus_model_train(56);
    char body[64];
    snprintf(body, sizeof(body), "{\"usable_days\":%d}", days);
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, body);
}

// Return one appliance's learned usage profile (standby / program power / length).
static esp_err_t appliance_profile_get_handler(httpd_req_t *req)
{
    char id[48] = {0};
    size_t qlen = httpd_req_get_url_query_len(req);
    if (qlen > 0 && qlen < 96)
    {
        char query[96];
        if (httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK)
            httpd_query_key_value(query, "id", id, sizeof(id));
    }
    if (!id[0])
    {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing id");
        return ESP_FAIL;
    }

    char *json = appliance_profile_json(id);
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

// Return every appliance's learned usage profile.
static esp_err_t appliance_profiles_get_handler(httpd_req_t *req)
{
    char *json = appliance_profile_all_json();
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

// Suggested appliance schedule for a day: slides each appliance's learned run
// across that day's surplus forecast. Defaults to tomorrow (matching the
// surplus forecast endpoint) when no ?date= is given.
static esp_err_t schedule_get_handler(httpd_req_t *req)
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

    char *json = scheduler_json(date);
    if (!json) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "OOM");
        return ESP_FAIL;
    }
    httpd_resp_set_type(req, "application/json");
    esp_err_t err = httpd_resp_sendstr(req, json);
    free(json);
    return err;
}

// Re-derive every appliance profile from history now (test hook).
static esp_err_t test_appliance_profiles_train_handler(httpd_req_t *req)
{
    appliance_profile_train_all(30);
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, "{\"trained\":true}");
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

// Resolve the optional ?fs= query param to a mounted base path. "littlefs"
// (the default) browses the internal config partition; "sdcard" browses the SD
// card, where the per-day data files now live. Returns NULL for an unknown value.
static const char *resolve_fs_base(httpd_req_t *req)
{
    char query[64];
    char fs[16] = {0};
    size_t qlen = httpd_req_get_url_query_len(req);
    if (qlen > 0 && qlen < sizeof(query) &&
        httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK)
        httpd_query_key_value(query, "fs", fs, sizeof(fs));

    if (fs[0] == '\0' || strcmp(fs, "littlefs") == 0) return LFS_BASE_PATH;
    if (strcmp(fs, "sdcard") == 0 || strcmp(fs, "sd") == 0) return SD_CARD_MOUNT_POINT;
    return NULL; // unknown selector
}

static esp_err_t debug_files_list_handler(httpd_req_t *req)
{
    const char *base = resolve_fs_base(req);
    if (!base)
    {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Unknown fs (use littlefs or sdcard)");
        return ESP_FAIL;
    }

    DIR *dir = opendir(base);
    if (!dir)
    {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Cannot open filesystem");
        return ESP_FAIL;
    }

    cJSON *arr = cJSON_CreateArray();
    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL)
    {
        if (entry->d_type != DT_REG)
            continue;
        char full[FS_PATH_MAX];
        snprintf(full, sizeof(full), "%s/%s", base, entry->d_name);
        struct stat st;
        long size = (stat(full, &st) == 0) ? (long)st.st_size : -1;
        cJSON *obj = cJSON_CreateObject();
        cJSON_AddStringToObject(obj, "name", entry->d_name);
        cJSON_AddNumberToObject(obj, "size", size);
        cJSON_AddItemToArray(arr, obj);
    }
    closedir(dir);

    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "base", base);
    cJSON_AddItemToObject(root, "files", arr);
    return send_json(req, root, 200);
}

static esp_err_t debug_files_get_handler(httpd_req_t *req)
{
    const char *base = resolve_fs_base(req);
    if (!base)
    {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Unknown fs (use littlefs or sdcard)");
        return ESP_FAIL;
    }

    // req->uri carries the full path including any ?fs= query string, so copy the
    // filename up to the '?' (or end) before validating and building the path.
    const char *uri_name = req->uri + strlen("/debug/files/");
    char name[FS_PATH_MAX];
    size_t i = 0;
    for (; uri_name[i] && uri_name[i] != '?' && i < sizeof(name) - 1; i++)
        name[i] = uri_name[i];
    name[i] = '\0';

    if (!name[0] || strchr(name, '/'))
    {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid filename");
        return ESP_FAIL;
    }
    char fs_path[FS_PATH_MAX];
    snprintf(fs_path, sizeof(fs_path), "%s/%s", base, name);
    httpd_resp_set_type(req, "text/plain");
    return send_file(req, fs_path);
}

static esp_err_t debug_ping6_get_handler(httpd_req_t *req)
{
    char qbuf[128] = {0};
    char addr_str[64] = {0};

    size_t qlen = httpd_req_get_url_query_len(req);
    if (qlen > 0 && qlen < sizeof(qbuf)) {
        if (httpd_req_get_url_query_str(req, qbuf, sizeof(qbuf)) == ESP_OK)
            httpd_query_key_value(qbuf, "addr", addr_str, sizeof(addr_str));
    }
    if (!addr_str[0]) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing addr");
        return ESP_FAIL;
    }

    struct sockaddr_in6 dst = {0};
    dst.sin6_family = AF_INET6;
    if (inet_pton(AF_INET6, addr_str, &dst.sin6_addr) != 1) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid IPv6 address");
        return ESP_FAIL;
    }

    // Find a usable source address and interface from the default netif
    char src_addr_str[50] = {0};
    char iface_name[10] = {0};
    esp_netif_t *netif = esp_netif_get_default_netif();
    if (netif) {
        esp_ip6_addr_t ip6_list[LWIP_IPV6_NUM_ADDRESSES];
        int count = esp_netif_get_all_ip6(netif, ip6_list);
        for (int j = 0; j < count; j++) {
            if (esp_netif_ip6_get_addr_type(&ip6_list[j]) != ESP_IP6_ADDR_IS_UNKNOWN) {
                snprintf(src_addr_str, sizeof(src_addr_str), IPV6STR, IPV62STR(ip6_list[j]));
                esp_netif_get_netif_impl_name(netif, iface_name);
                break;
            }
        }
    }

    int sock = socket(AF_INET6, SOCK_RAW, IPPROTO_ICMPV6);
    if (sock < 0) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Socket create failed");
        return ESP_FAIL;
    }

    if (src_addr_str[0]) {
        struct sockaddr_in6 src = {0};
        src.sin6_family = AF_INET6;
        inet_pton(AF_INET6, src_addr_str, &src.sin6_addr);
        bind(sock, (struct sockaddr *)&src, sizeof(src));
    }

    if (iface_name[0]) {
        struct ifreq ifr = {0};
        strlcpy(ifr.ifr_name, iface_name, sizeof(ifr.ifr_name));
        setsockopt(sock, SOL_SOCKET, SO_BINDTODEVICE, &ifr, sizeof(ifr));
    }

    struct timeval tv = {.tv_sec = 2, .tv_usec = 0};
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "target", addr_str);
    cJSON *results = cJSON_AddArrayToObject(root, "pings");

    uint16_t ping_id = (uint16_t)(esp_random() & 0xFFFF);
    uint8_t payload[8] = "PING";

    for (int i = 0; i < 4; i++) {
        struct icmp6_echo_hdr pkt = {
            .type   = ICMP6_TYPE_EREQ,
            .code   = 0,
            .chksum = 0,
            .id     = htons(ping_id),
            .seqno  = htons((uint16_t)(i + 1)),
        };

        struct iovec iov[2];
        iov[0].iov_base = &pkt;
        iov[0].iov_len  = sizeof(pkt);
        iov[1].iov_base = payload;
        iov[1].iov_len  = sizeof(payload);

        struct msghdr msg = {0};
        msg.msg_name    = &dst;
        msg.msg_namelen = sizeof(dst);
        msg.msg_iov     = iov;
        msg.msg_iovlen  = 2;

        int64_t t_send = esp_timer_get_time();
        ssize_t sent = sendmsg(sock, &msg, 0);

        cJSON *ping = cJSON_CreateObject();
        cJSON_AddNumberToObject(ping, "seq", i + 1);

        if (sent < 0) {
            cJSON_AddBoolToObject(ping, "success", false);
            cJSON_AddStringToObject(ping, "error", "send failed");
            cJSON_AddItemToArray(results, ping);
            continue;
        }

        // lwIP delivers the full IPv6 header (IP6_HLEN bytes) before the ICMPv6 payload.
        // Loop to discard non-echo-reply packets (e.g. Neighbor Discovery).
        uint8_t recv_buf[256];
        int64_t t_recv = 0;
        while (true) {
            struct iovec recv_iov = { .iov_base = recv_buf, .iov_len = sizeof(recv_buf) };
            struct msghdr recv_msg = {0};
            recv_msg.msg_iov    = &recv_iov;
            recv_msg.msg_iovlen = 1;

            ssize_t recvd = recvmsg(sock, &recv_msg, 0);
            t_recv = esp_timer_get_time();

            if (recvd < 0) {
                cJSON_AddBoolToObject(ping, "success", false);
                cJSON_AddStringToObject(ping, "error", "timeout");
                break;
            }

            if (recvd < IP6_HLEN + (ssize_t)ICMP6_HLEN) continue;

            struct icmp6_echo_hdr *reply = (struct icmp6_echo_hdr *)(recv_buf + IP6_HLEN);
            if (reply->type != ICMP6_TYPE_EREP) continue;

            cJSON_AddBoolToObject(ping, "success", true);
            cJSON_AddNumberToObject(ping, "rtt_ms", (t_recv - t_send) / 1000.0);
            break;
        }

        cJSON_AddItemToArray(results, ping);
        if (i < 3) vTaskDelay(pdMS_TO_TICKS(200));
    }

    close(sock);
    return send_json(req, root, 200);
}

static esp_err_t debug_routes6_get_handler(httpd_req_t *req)
{
    cJSON *root = cJSON_CreateObject();
    cJSON *ifaces = cJSON_AddArrayToObject(root, "interfaces");

    struct netif *netif;
    NETIF_FOREACH(netif) {
        cJSON *iface = cJSON_CreateObject();

        char name[8];
        snprintf(name, sizeof(name), "%c%c%u", netif->name[0], netif->name[1], netif->num);
        cJSON_AddStringToObject(iface, "name", name);
        cJSON_AddBoolToObject(iface, "up", netif_is_up(netif));
        cJSON_AddBoolToObject(iface, "link_up", netif_is_link_up(netif));

        // IPv4 address for reference
        char ip4[16];
        snprintf(ip4, sizeof(ip4), IPSTR, IP2STR(&netif->ip_addr.u_addr.ip4));
        cJSON_AddStringToObject(iface, "ip4", ip4);

        cJSON *addrs = cJSON_AddArrayToObject(iface, "ip6_addrs");
        for (int j = 0; j < LWIP_IPV6_NUM_ADDRESSES; j++) {
            uint8_t state = netif_ip6_addr_state(netif, j);
            if (ip6_addr_isinvalid(state)) continue;

            char addr6[64] = {0};
            ip6addr_ntoa_r(netif_ip6_addr(netif, j), addr6, sizeof(addr6));

            const char *state_str = "valid";
            if (ip6_addr_istentative(state))       state_str = "tentative";
            else if (ip6_addr_ispreferred(state))  state_str = "preferred";
            else if (ip6_addr_isdeprecated(state)) state_str = "deprecated";

            cJSON *a = cJSON_CreateObject();
            cJSON_AddStringToObject(a, "addr", addr6);
            cJSON_AddStringToObject(a, "state", state_str);
            cJSON_AddItemToArray(addrs, a);
        }

        // Always include the interface, even with no IPv6 — lets you see all netifs
        cJSON_AddItemToArray(ifaces, iface);
    }

    // Default routers learned via RA
    cJSON *routers = cJSON_AddArrayToObject(root, "default_routers");
    for (int i = 0; i < LWIP_ND6_NUM_ROUTERS; i++) {
        if (default_router_list[i].neighbor_entry == NULL) continue;
        char addr6[64] = {0};
        ip6addr_ntoa_r(&default_router_list[i].neighbor_entry->next_hop_address, addr6, sizeof(addr6));
        cJSON *r = cJSON_CreateObject();
        cJSON_AddStringToObject(r, "addr", addr6);
        cJSON_AddNumberToObject(r, "lifetime_s", default_router_list[i].invalidation_timer);
        struct netif *rnetif = default_router_list[i].neighbor_entry->netif;
        if (rnetif) {
            char name[8];
            snprintf(name, sizeof(name), "%c%c%u", rnetif->name[0], rnetif->name[1], rnetif->num);
            cJSON_AddStringToObject(r, "iface", name);
        }
        cJSON_AddItemToArray(routers, r);
    }

    // On-link prefixes learned via RA Prefix Information Option (PIO)
    cJSON *prefixes = cJSON_AddArrayToObject(root, "on_link_prefixes");
    for (int i = 0; i < LWIP_ND6_NUM_PREFIXES; i++) {
        if (ip6_addr_isany(&prefix_list[i].prefix)) continue;
        char addr6[64] = {0};
        ip6addr_ntoa_r(&prefix_list[i].prefix, addr6, sizeof(addr6));
        cJSON *p = cJSON_CreateObject();
        cJSON_AddStringToObject(p, "prefix", addr6);
        cJSON_AddNumberToObject(p, "lifetime_s", prefix_list[i].invalidation_timer);
        if (prefix_list[i].netif) {
            char name[8];
            snprintf(name, sizeof(name), "%c%c%u",
                     prefix_list[i].netif->name[0],
                     prefix_list[i].netif->name[1],
                     prefix_list[i].netif->num);
            cJSON_AddStringToObject(p, "iface", name);
        }
        cJSON_AddItemToArray(prefixes, p);
    }

    // Destination cache — shows next-hop actually used for each looked-up destination.
    // Thread mesh-local addresses (fd43::.../64 via RIO) appear here after the first
    // lookup, confirming they route through the Thread Border Router gateway.
    cJSON *dests = cJSON_AddArrayToObject(root, "destination_cache");
    for (int i = 0; i < LWIP_ND6_NUM_DESTINATIONS; i++) {
        if (ip6_addr_isany(&destination_cache[i].destination_addr)) continue;
        char dst[64] = {0}, hop[64] = {0};
        ip6addr_ntoa_r(&destination_cache[i].destination_addr, dst, sizeof(dst));
        ip6addr_ntoa_r(&destination_cache[i].next_hop_addr, hop, sizeof(hop));
        cJSON *d = cJSON_CreateObject();
        cJSON_AddStringToObject(d, "dest", dst);
        cJSON_AddStringToObject(d, "next_hop", hop);
        cJSON_AddNumberToObject(d, "age", destination_cache[i].age);
        cJSON_AddItemToArray(dests, d);
    }

    static const char *const nd6_states[] = {
        "NO_ENTRY", "INCOMPLETE", "REACHABLE", "STALE", "DELAY", "PROBE"
    };
    cJSON *neighbors = cJSON_AddArrayToObject(root, "neighbor_cache");
    for (int i = 0; i < LWIP_ND6_NUM_NEIGHBORS; i++) {
        if (neighbor_cache[i].state == ND6_NO_ENTRY) continue;
        char addr6[64] = {0};
        ip6addr_ntoa_r(&neighbor_cache[i].next_hop_address, addr6, sizeof(addr6));
        cJSON *n = cJSON_CreateObject();
        cJSON_AddStringToObject(n, "addr", addr6);
        uint8_t s = neighbor_cache[i].state;
        cJSON_AddStringToObject(n, "state", s < 6 ? nd6_states[s] : "UNKNOWN");
        cJSON_AddBoolToObject(n, "is_router", neighbor_cache[i].isrouter);
        cJSON_AddNumberToObject(n, "probes_sent", neighbor_cache[i].counter.probes_sent);
        cJSON_AddItemToArray(neighbors, n);
    }

    return send_json(req, root, 200);
}

// nd6_clear_destination_cache() touches lwIP internals and must run with the
// TCPIP core lock held, so it's invoked via esp_netif_tcpip_exec.
static esp_err_t clear_dest_cache_cb(void *ctx)
{
    (void)ctx;
    nd6_clear_destination_cache();
    return ESP_OK;
}

static esp_err_t debug_routes6_cache_delete_handler(httpd_req_t *req)
{
    esp_err_t err = esp_netif_tcpip_exec(clear_dest_cache_cb, NULL);

    cJSON *root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root, "cleared", err == ESP_OK);
    if (err != ESP_OK) {
        cJSON_AddStringToObject(root, "error", esp_err_to_name(err));
    }
    return send_json(req, root, err == ESP_OK ? 200 : 500);
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

// Custom URI matcher. The stock httpd_uri_match_wildcard only honours a '*' that
// is the final character of the template, so mid-path patterns such as
// "/api/devices/*/interrogate" never match and get shadowed by the trailing-
// wildcard "/api/devices/*" route (yielding a spurious 405). This matcher treats:
//   - a '*' that is NOT the last template char as exactly one path segment
//     (one or more non-'/' chars), and
//   - a trailing '*' as the remainder of the URI (zero or more chars, including
//     '/'), preserving the stock greedy behaviour relied on by "/*",
//     "/debug/files/*", "/api/devices/*", etc.
static bool uri_match_segments(const char *templ, const char *uri, size_t uri_len)
{
    size_t tlen = strlen(templ);
    size_t ti = 0, ui = 0;

    while (ti < tlen) {
        if (templ[ti] == '*') {
            if (ti == tlen - 1) {
                // Trailing wildcard: matches the rest (zero or more chars).
                return true;
            }
            // Mid-path wildcard: consume exactly one non-empty segment.
            if (ui >= uri_len || uri[ui] == '/') {
                return false;
            }
            while (ui < uri_len && uri[ui] != '/') {
                ui++;
            }
            ti++; // step past '*'; template should continue with '/'
        } else {
            if (ui >= uri_len || templ[ti] != uri[ui]) {
                return false;
            }
            ti++;
            ui++;
        }
    }
    // Template fully consumed — match only if the URI is too.
    return ui == uri_len;
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
    config.uri_match_fn = uri_match_segments;
    config.stack_size = 12288;
    config.max_uri_handlers = 46;
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
    const httpd_uri_t devices_endpoints_get = {.uri = "/api/devices/endpoints", .method = HTTP_GET, .handler = devices_endpoints_get_handler};
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
    const httpd_uri_t data_node_get = {.uri = "/api/data/node", .method = HTTP_GET, .handler = data_node_get_handler};
    const httpd_uri_t topology_grid_put = {.uri = "/api/topology/grid", .method = HTTP_PUT, .handler = topology_grid_put_handler};
    const httpd_uri_t topology_solar_put = {.uri = "/api/topology/solar", .method = HTTP_PUT, .handler = topology_solar_put_handler};
    const httpd_uri_t edge_post = {.uri = "/api/edges", .method = HTTP_POST, .handler = edge_post_handler};
    const httpd_uri_t edge_delete = {.uri = "/api/edges/*", .method = HTTP_DELETE, .handler = edge_delete_handler};
    const httpd_uri_t forecast_solar_get = {.uri = "/api/forecast/solar", .method = HTTP_GET, .handler = forecast_solar_get_handler};
    const httpd_uri_t forecast_solar_fetch = {.uri = "/api/forecast/solar/fetch", .method = HTTP_POST, .handler = forecast_solar_fetch_handler};
    const httpd_uri_t forecast_consumption_get = {.uri = "/api/forecast/consumption", .method = HTTP_GET, .handler = forecast_consumption_get_handler};
    const httpd_uri_t forecast_surplus_get = {.uri = "/api/forecast/surplus", .method = HTTP_GET, .handler = forecast_surplus_get_handler};
    const httpd_uri_t forecast_surplus_model_get = {.uri = "/api/forecast/surplus/model", .method = HTTP_GET, .handler = forecast_surplus_model_get_handler};
    const httpd_uri_t test_surplus_model_train = {.uri = "/api/test/surplus-model/train", .method = HTTP_POST, .handler = test_surplus_model_train_handler};
    const httpd_uri_t appliance_profile_get = {.uri = "/api/appliance/profile", .method = HTTP_GET, .handler = appliance_profile_get_handler};
    const httpd_uri_t appliance_profiles_get = {.uri = "/api/appliance/profiles", .method = HTTP_GET, .handler = appliance_profiles_get_handler};
    const httpd_uri_t schedule_get = {.uri = "/api/schedule", .method = HTTP_GET, .handler = schedule_get_handler};
    const httpd_uri_t test_appliance_profiles_train = {.uri = "/api/test/appliance-profiles/train", .method = HTTP_POST, .handler = test_appliance_profiles_train_handler};
    const httpd_uri_t test_generate_sample_data = {.uri = "/api/test/generate-sample-data", .method = HTTP_POST, .handler = test_generate_sample_data_handler};
    const httpd_uri_t test_rollup_hourly = {.uri = "/api/test/rollup-hourly", .method = HTTP_POST, .handler = test_rollup_hourly_handler};
    const httpd_uri_t test_consumption_forecast_compute = {.uri = "/api/test/consumption-forecast/compute", .method = HTTP_POST, .handler = test_consumption_forecast_compute_handler};
    const httpd_uri_t test_run_daily_job = {.uri = "/api/test/run-daily-job", .method = HTTP_POST, .handler = test_run_daily_job_handler};
    const httpd_uri_t debug_files_list = {.uri = "/debug/files", .method = HTTP_GET, .handler = debug_files_list_handler};
    const httpd_uri_t debug_files_get = {.uri = "/debug/files/*", .method = HTTP_GET, .handler = debug_files_get_handler};
    const httpd_uri_t debug_ping6 = {.uri = "/debug/ping6", .method = HTTP_GET, .handler = debug_ping6_get_handler};
    const httpd_uri_t debug_routes6 = {.uri = "/debug/routes6", .method = HTTP_GET, .handler = debug_routes6_get_handler};
    const httpd_uri_t debug_routes6_cache_delete = {.uri = "/debug/routes6/cache", .method = HTTP_DELETE, .handler = debug_routes6_cache_delete_handler};
    const httpd_uri_t static_files = {.uri = "/*", .method = HTTP_GET, .handler = static_get_handler};

    httpd_register_uri_handler(server, &devices_get);
    httpd_register_uri_handler(server, &devices_simple_get);
    httpd_register_uri_handler(server, &devices_endpoints_get);
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
    httpd_register_uri_handler(server, &data_node_get);
    httpd_register_uri_handler(server, &topology_grid_put);
    httpd_register_uri_handler(server, &topology_solar_put);
    httpd_register_uri_handler(server, &edge_post);
    httpd_register_uri_handler(server, &edge_delete);
    httpd_register_uri_handler(server, &forecast_solar_get);
    httpd_register_uri_handler(server, &forecast_solar_fetch);
    httpd_register_uri_handler(server, &forecast_consumption_get);
    httpd_register_uri_handler(server, &forecast_surplus_get);
    httpd_register_uri_handler(server, &forecast_surplus_model_get);
    httpd_register_uri_handler(server, &test_surplus_model_train);
    httpd_register_uri_handler(server, &appliance_profile_get);
    httpd_register_uri_handler(server, &appliance_profiles_get);
    httpd_register_uri_handler(server, &schedule_get);
    httpd_register_uri_handler(server, &test_appliance_profiles_train);
    httpd_register_uri_handler(server, &test_generate_sample_data);
    httpd_register_uri_handler(server, &test_rollup_hourly);
    httpd_register_uri_handler(server, &test_consumption_forecast_compute);
    httpd_register_uri_handler(server, &test_run_daily_job);
    httpd_register_uri_handler(server, &debug_files_list);
    httpd_register_uri_handler(server, &debug_files_get);
    httpd_register_uri_handler(server, &debug_ping6);
    httpd_register_uri_handler(server, &debug_routes6);
    httpd_register_uri_handler(server, &debug_routes6_cache_delete);

    ws_server_init(server);

    httpd_register_uri_handler(server, &static_files);

    ESP_LOGI(TAG, "Web server started on port 80");
    return ESP_OK;
}
