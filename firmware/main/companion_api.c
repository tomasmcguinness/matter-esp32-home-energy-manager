// The REST contract the MCC iOS app (Matter Controller Companion) expects of a
// self-hosted Matter controller: list the commissioned nodes, add one, rename it, remove
// it. See firmware/COMPANION_API.md.
//
// MCC publishes this contract at /api/nodes, which in this firmware already means the
// topology graph, so everything here is namespaced under /api/companion instead.
//
// There is no authentication, in keeping with the rest of the web server. MCC shows the
// response body of any non-2xx to the user, so the error strings below are written to be
// read by a person, not a log.

#include "companion_api.h"

#include <string.h>
#include <inttypes.h>

#include "cJSON.h"
#include "esp_log.h"
#include "esp_netif.h"

#include "managers/device_manager.h"
#include "matter_controller.h"

static const char *TAG = "companion_api";

// A setup code is ~30 characters and a name is bounded by the device manager, so this is
// generous for both bodies this API accepts.
#define COMPANION_MAX_BODY 512

// web_server.c keeps its equivalents of these static, so the companion surface carries its
// own rather than widening that file's interface for two callers.

static esp_err_t send_json(httpd_req_t *req, cJSON *root)
{
    char *text = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!text)
    {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Out of memory building the response");
        return ESP_FAIL;
    }
    httpd_resp_set_type(req, "application/json");
    esp_err_t err = httpd_resp_sendstr(req, text);
    free(text);
    return err;
}

// Reads the whole request body into `out`. Replies to the client and returns ESP_FAIL on
// any problem, so callers can simply return the result.
static esp_err_t read_body(httpd_req_t *req, char *out, size_t out_size)
{
    if (req->content_len <= 0 || req->content_len >= (int)out_size)
    {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "The request body is missing or too large");
        return ESP_FAIL;
    }

    int received = 0;
    while (received < (int)req->content_len)
    {
        int r = httpd_req_recv(req, out + received, req->content_len - received);
        if (r <= 0)
        {
            httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "The request body could not be read");
            return ESP_FAIL;
        }
        received += r;
    }
    out[received] = '\0';
    return ESP_OK;
}

// Confirms the controller is reachable. MCC calls this the moment the user types in an
// address, so a wrong one fails there rather than on the device list. It ignores the body;
// the contents are for whoever curls it.
static esp_err_t info_get_handler(httpd_req_t *req)
{
    cJSON *root = cJSON_CreateObject();

    cJSON_AddNumberToObject(root, "v", 1);
    cJSON_AddStringToObject(root, "name", "Home Energy Manager");

    // mDNS is disabled on this board (see main.cpp), so the address is whatever DHCP gave
    // the Ethernet interface. When mDNS comes back, "url" becomes the .local name and
    // nothing else about this contract changes.
    esp_netif_t *eth_netif = esp_netif_get_handle_from_ifkey("ETH_DEF");
    esp_netif_ip_info_t ip_info;

    if (eth_netif != NULL && esp_netif_get_ip_info(eth_netif, &ip_info) == ESP_OK && ip_info.ip.addr != 0)
    {
        char ip[16];
        snprintf(ip, sizeof(ip), IPSTR, IP2STR(&ip_info.ip));
        cJSON_AddStringToObject(root, "ip", ip);

        char url[24];
        snprintf(url, sizeof(url), "http://%s", ip);
        cJSON_AddStringToObject(root, "url", url);
    }
    else
    {
        cJSON_AddNullToObject(root, "ip");
        cJSON_AddNullToObject(root, "url");
    }

    return send_json(req, root);
}

static esp_err_t nodes_get_handler(httpd_req_t *req)
{
    char *json = device_manager_get_companion_nodes_json();
    if (!json)
    {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Out of memory building the device list");
        return ESP_FAIL;
    }
    httpd_resp_set_type(req, "application/json");
    esp_err_t err = httpd_resp_sendstr(req, json);
    free(json);
    return err;
}

// Commissions a device onto this controller's fabric.
//
// This does not return until commissioning has finished, which is what MCC requires: it
// treats a 2xx as proof the device really joined, and waits up to 90 seconds for one.
static esp_err_t nodes_post_handler(httpd_req_t *req)
{
    char body[COMPANION_MAX_BODY];
    if (read_body(req, body, sizeof(body)) != ESP_OK)
    {
        return ESP_FAIL;
    }

    cJSON *root = cJSON_Parse(body);
    if (!root)
    {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "The request body is not valid JSON");
        return ESP_FAIL;
    }

    // "inUse" is part of MCC's request but is always false: the system setup sheet only
    // ever hands over a device that is not on a fabric yet, which is the only case this
    // controller can pair anyway.
    cJSON *setup_code_item = cJSON_GetObjectItemCaseSensitive(root, "setupCode");
    if (!cJSON_IsString(setup_code_item) || setup_code_item->valuestring[0] == '\0')
    {
        cJSON_Delete(root);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "A setupCode is required");
        return ESP_FAIL;
    }

    char setup_code[256];
    strncpy(setup_code, setup_code_item->valuestring, sizeof(setup_code) - 1);
    setup_code[sizeof(setup_code) - 1] = '\0';
    cJSON_Delete(root);

    ESP_LOGI(TAG, "Commissioning on-network from a companion app request");

    uint64_t node_id = 0;
    esp_err_t err = matter_controller_commission_on_network(setup_code, &node_id);

    if (err == ESP_ERR_INVALID_ARG)
    {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "That setup code could not be read");
        return ESP_FAIL;
    }
    if (err == ESP_ERR_TIMEOUT)
    {
        // MCC names 504 as "the controller gave up waiting" and reports it as a failed setup.
        httpd_resp_set_status(req, "504 Gateway Timeout");
        httpd_resp_sendstr(req, "Commissioning timed out. Check the device is powered on and in pairing mode.");
        return ESP_OK;
    }
    if (err != ESP_OK)
    {
        httpd_resp_set_status(req, "502 Bad Gateway");
        httpd_resp_sendstr(req, "The device could not be commissioned.");
        return ESP_OK;
    }

    ESP_LOGI(TAG, "Commissioned node %" PRIu64, node_id);

    cJSON *resp = cJSON_CreateObject();
    cJSON_AddNumberToObject(resp, "nodeId", (double)node_id);
    return send_json(req, resp);
}

// Sets the node's name. MCC calls this after commissioning with whatever the user typed
// into the system setup sheet, so the device carries the same name in both places.
static esp_err_t node_update_put_handler(httpd_req_t *req)
{
    uint64_t node_id = 0;
    if (sscanf(req->uri, "/api/companion/nodes/%" SCNu64 "/update", &node_id) != 1)
    {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "The node id in the URL is not a number");
        return ESP_FAIL;
    }

    char body[COMPANION_MAX_BODY];
    if (read_body(req, body, sizeof(body)) != ESP_OK)
    {
        return ESP_FAIL;
    }

    cJSON *root = cJSON_Parse(body);
    if (!root)
    {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "The request body is not valid JSON");
        return ESP_FAIL;
    }

    cJSON *name_item = cJSON_GetObjectItemCaseSensitive(root, "name");
    if (!cJSON_IsString(name_item))
    {
        cJSON_Delete(root);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "A name is required");
        return ESP_FAIL;
    }

    const char *name = name_item->valuestring;
    esp_err_t err = device_manager_set_device_name(node_id, name, strlen(name));
    cJSON_Delete(root);

    if (err == ESP_ERR_NOT_FOUND)
    {
        httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "No device with that node id");
        return ESP_FAIL;
    }
    if (err != ESP_OK)
    {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "The name could not be saved");
        return ESP_FAIL;
    }

    device_manager_persist();

    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{}");
    return ESP_OK;
}

static esp_err_t node_delete_handler(httpd_req_t *req)
{
    uint64_t node_id = 0;
    if (sscanf(req->uri, "/api/companion/nodes/%" SCNu64, &node_id) != 1)
    {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "The node id in the URL is not a number");
        return ESP_FAIL;
    }

    esp_err_t err = matter_controller_remove_node(node_id);
    if (err != ESP_OK && err != ESP_ERR_NOT_FOUND)
    {
        // The device is offline, so RemoveFabric never reached it. Honour the delete
        // anyway: forget the node locally so a dead device can still be removed instead
        // of being stuck forever re-subscribing.
        ESP_LOGW(TAG, "Unpair of node 0x%llx failed (0x%x); forgetting locally",
                 (unsigned long long)node_id, err);
        matter_controller_forget_node(node_id);
    }

    err = device_manager_remove_device(node_id);
    if (err != ESP_OK)
    {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "The device could not be removed");
        return ESP_FAIL;
    }

    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{}");
    return ESP_OK;
}

esp_err_t companion_api_register(httpd_handle_t server)
{
    if (!server)
    {
        return ESP_ERR_INVALID_ARG;
    }

    // "/api/companion/nodes/*/update" relies on web_server.c's uri_match_segments
    // treating a non-trailing '*' as exactly one path segment; the trailing '*' on the
    // DELETE keeps the stock greedy behaviour.
    const httpd_uri_t routes[] = {
        {.uri = "/api/companion/info",           .method = HTTP_GET,    .handler = info_get_handler},
        {.uri = "/api/companion/nodes",          .method = HTTP_GET,    .handler = nodes_get_handler},
        {.uri = "/api/companion/nodes",          .method = HTTP_POST,   .handler = nodes_post_handler},
        {.uri = "/api/companion/nodes/*/update", .method = HTTP_PUT,    .handler = node_update_put_handler},
        {.uri = "/api/companion/nodes/*",        .method = HTTP_DELETE, .handler = node_delete_handler},
    };

    for (size_t i = 0; i < sizeof(routes) / sizeof(routes[0]); i++)
    {
        esp_err_t err = httpd_register_uri_handler(server, &routes[i]);
        if (err != ESP_OK)
        {
            // Almost always max_uri_handlers being too small, which otherwise shows up as
            // a mystery 404 at runtime.
            ESP_LOGE(TAG, "Could not register %s (0x%x)", routes[i].uri, err);
            return err;
        }
    }

    ESP_LOGI(TAG, "Companion API registered under /api/companion");
    return ESP_OK;
}
