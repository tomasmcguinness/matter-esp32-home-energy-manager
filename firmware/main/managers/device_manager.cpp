#include "device_manager.h"

#include <string.h>
#include <stdio.h>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>
#include <string>

#include "esp_log.h"
#include "esp_littlefs.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "cJSON.h"

static const char *TAG = "device_manager";

#define LFS_BASE_PATH   "/littlefs"
#define DEVICES_PATH    LFS_BASE_PATH "/devices.json"
#define DEVICES_TMP     LFS_BASE_PATH "/devices.json.tmp"
#define FIRST_NODE_ID   10000ULL

struct endpoint_entry_t {
    uint16_t endpoint_id;
    std::string label;
    bool included;
    std::vector<uint32_t> device_types;
    std::vector<uint16_t> parts;
};

struct device_entry_t {
    uint64_t node_id;
    std::string vendor_name;
    std::string product_name;
    std::vector<endpoint_entry_t> endpoints;
};

static std::vector<device_entry_t> s_devices;
static uint64_t s_next_node_id = FIRST_NODE_ID;
static SemaphoreHandle_t s_mutex = nullptr;

static device_entry_t *find_device(uint64_t node_id)
{
    for (auto &d : s_devices) {
        if (d.node_id == node_id) return &d;
    }
    return nullptr;
}

static endpoint_entry_t *find_endpoint(device_entry_t *dev, uint16_t endpoint_id)
{
    if (!dev) return nullptr;
    for (auto &ep : dev->endpoints) {
        if (ep.endpoint_id == endpoint_id) return &ep;
    }
    return nullptr;
}

static void load_from_disk(void)
{
    FILE *f = fopen(DEVICES_PATH, "r");
    if (!f) {
        ESP_LOGI(TAG, "No devices file; starting fresh");
        return;
    }
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (size <= 0 || size > 65536) { fclose(f); return; }

    char *buf = (char *)malloc(size + 1);
    if (!buf) { fclose(f); return; }
    size_t n = fread(buf, 1, size, f);
    fclose(f);
    buf[n] = '\0';

    cJSON *root = cJSON_Parse(buf);
    free(buf);
    if (!root) return;

    cJSON *next_id = cJSON_GetObjectItemCaseSensitive(root, "nextNodeId");
    if (cJSON_IsNumber(next_id)) {
        s_next_node_id = (uint64_t)next_id->valuedouble;
    }

    cJSON *devices = cJSON_GetObjectItemCaseSensitive(root, "devices");
    cJSON *dev_json = nullptr;
    cJSON_ArrayForEach(dev_json, devices) {
        device_entry_t dev;
        cJSON *nid = cJSON_GetObjectItemCaseSensitive(dev_json, "nodeId");
        if (cJSON_IsNumber(nid)) dev.node_id = (uint64_t)nid->valuedouble;
        cJSON *vn = cJSON_GetObjectItemCaseSensitive(dev_json, "vendorName");
        if (cJSON_IsString(vn)) dev.vendor_name = vn->valuestring;
        cJSON *pn = cJSON_GetObjectItemCaseSensitive(dev_json, "productName");
        if (cJSON_IsString(pn)) dev.product_name = pn->valuestring;

        cJSON *endpoints = cJSON_GetObjectItemCaseSensitive(dev_json, "endpoints");
        cJSON *ep_json = nullptr;
        cJSON_ArrayForEach(ep_json, endpoints) {
            endpoint_entry_t ep;
            cJSON *eid = cJSON_GetObjectItemCaseSensitive(ep_json, "endpointId");
            if (cJSON_IsNumber(eid)) ep.endpoint_id = (uint16_t)eid->valueint;
            cJSON *lbl = cJSON_GetObjectItemCaseSensitive(ep_json, "label");
            if (cJSON_IsString(lbl)) ep.label = lbl->valuestring;
            cJSON *inc = cJSON_GetObjectItemCaseSensitive(ep_json, "included");
            ep.included = cJSON_IsTrue(inc);
            cJSON *dts = cJSON_GetObjectItemCaseSensitive(ep_json, "deviceTypes");
            cJSON *dt_json = nullptr;
            cJSON_ArrayForEach(dt_json, dts) {
                if (cJSON_IsNumber(dt_json)) {
                    ep.device_types.push_back((uint32_t)dt_json->valueint);
                }
            }
            cJSON *parts = cJSON_GetObjectItemCaseSensitive(ep_json, "parts");
            cJSON *part_json = nullptr;
            cJSON_ArrayForEach(part_json, parts) {
                if (cJSON_IsNumber(part_json)) {
                    ep.parts.push_back((uint16_t)part_json->valueint);
                }
            }
            dev.endpoints.push_back(ep);
        }
        s_devices.push_back(dev);
    }
    cJSON_Delete(root);
    ESP_LOGI(TAG, "Loaded %u device(s), nextNodeId=%" PRIu64, (unsigned)s_devices.size(), s_next_node_id);
}

esp_err_t device_manager_init(void)
{
    s_mutex = xSemaphoreCreateMutex();
    if (!s_mutex) return ESP_ERR_NO_MEM;

    // LittleFS already mounted by settings_store; tolerate already-mounted error
    esp_vfs_littlefs_conf_t conf = {
        .base_path              = LFS_BASE_PATH,
        .partition_label        = "config",
        .format_if_mount_failed = true,
        .dont_mount             = false,
    };
    esp_err_t err = esp_vfs_littlefs_register(&conf);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "LittleFS mount failed: 0x%x", err);
        return err;
    }

    load_from_disk();
    return ESP_OK;
}

uint64_t device_manager_next_node_id(void)
{
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    uint64_t id = s_next_node_id++;
    xSemaphoreGive(s_mutex);
    return id;
}

esp_err_t device_manager_add_device(uint64_t node_id)
{
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    if (!find_device(node_id)) {
        device_entry_t dev;
        dev.node_id = node_id;
        s_devices.push_back(dev);
    }
    xSemaphoreGive(s_mutex);
    return ESP_OK;
}

esp_err_t device_manager_set_vendor_name(uint64_t node_id, const char *name, size_t len)
{
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    auto *dev = find_device(node_id);
    if (dev) dev->vendor_name = std::string(name, len);
    xSemaphoreGive(s_mutex);
    return dev ? ESP_OK : ESP_ERR_NOT_FOUND;
}

esp_err_t device_manager_set_product_name(uint64_t node_id, const char *name, size_t len)
{
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    auto *dev = find_device(node_id);
    if (dev) dev->product_name = std::string(name, len);
    xSemaphoreGive(s_mutex);
    return dev ? ESP_OK : ESP_ERR_NOT_FOUND;
}

esp_err_t device_manager_add_endpoint(uint64_t node_id, uint16_t endpoint_id)
{
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    auto *dev = find_device(node_id);
    if (dev && !find_endpoint(dev, endpoint_id)) {
        endpoint_entry_t ep;
        ep.endpoint_id = endpoint_id;
        ep.included = false;
        dev->endpoints.push_back(ep);
    }
    xSemaphoreGive(s_mutex);
    return dev ? ESP_OK : ESP_ERR_NOT_FOUND;
}

esp_err_t device_manager_add_device_type(uint64_t node_id, uint16_t endpoint_id, uint32_t device_type_id)
{
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    auto *dev = find_device(node_id);
    auto *ep  = find_endpoint(dev, endpoint_id);
    if (ep) {
        bool found = false;
        for (auto dt : ep->device_types) { if (dt == device_type_id) { found = true; break; } }
        if (!found) ep->device_types.push_back(device_type_id);
    }
    xSemaphoreGive(s_mutex);
    return ep ? ESP_OK : ESP_ERR_NOT_FOUND;
}

esp_err_t device_manager_set_endpoint_label(uint64_t node_id, uint16_t endpoint_id,
                                             const char *label, size_t len)
{
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    auto *dev = find_device(node_id);
    auto *ep  = find_endpoint(dev, endpoint_id);
    if (ep) ep->label = std::string(label, len);
    xSemaphoreGive(s_mutex);
    return ep ? ESP_OK : ESP_ERR_NOT_FOUND;
}

esp_err_t device_manager_set_endpoint_included(uint64_t node_id, uint16_t endpoint_id, bool included)
{
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    auto *dev = find_device(node_id);
    auto *ep  = find_endpoint(dev, endpoint_id);
    if (ep) ep->included = included;
    xSemaphoreGive(s_mutex);
    return ep ? ESP_OK : ESP_ERR_NOT_FOUND;
}

char *device_manager_get_all_json(void)
{
    xSemaphoreTake(s_mutex, portMAX_DELAY);

    cJSON *root = cJSON_CreateObject();
    cJSON *arr  = cJSON_AddArrayToObject(root, "devices");

    for (const auto &dev : s_devices) {
        cJSON *dobj = cJSON_CreateObject();
        cJSON_AddNumberToObject(dobj, "nodeId", (double)dev.node_id);
        cJSON_AddStringToObject(dobj, "vendorName",  dev.vendor_name.c_str());
        cJSON_AddStringToObject(dobj, "productName", dev.product_name.c_str());

        cJSON *eps = cJSON_AddArrayToObject(dobj, "endpoints");
        for (const auto &ep : dev.endpoints) {
            cJSON *eobj = cJSON_CreateObject();
            cJSON_AddNumberToObject(eobj, "endpointId", ep.endpoint_id);
            cJSON_AddStringToObject(eobj, "label", ep.label.c_str());
            cJSON_AddBoolToObject(eobj, "included", ep.included);
            cJSON *dts = cJSON_AddArrayToObject(eobj, "deviceTypes");
            for (auto dt : ep.device_types) {
                cJSON_AddItemToArray(dts, cJSON_CreateNumber((double)dt));
            }
            cJSON *parts = cJSON_AddArrayToObject(eobj, "parts");
            for (auto p : ep.parts) {
                cJSON_AddItemToArray(parts, cJSON_CreateNumber((double)p));
            }
            cJSON_AddItemToArray(eps, eobj);
        }
        cJSON_AddItemToArray(arr, dobj);
    }

    xSemaphoreGive(s_mutex);

    char *text = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    return text;
}

static constexpr uint32_t kDevTypeAggregator      = 0x000E;
static constexpr uint32_t kDevTypeBridgedNode     = 0x0013;
static constexpr uint32_t kDevTypeSolarPower      = 0x0017;
static constexpr uint32_t kDevTypeElectricalSensor = 0x0510;

size_t device_manager_get_electrical_sensor_endpoints(uint64_t *node_ids, uint16_t *endpoint_ids, size_t max)
{
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    size_t count = 0;
    for (const auto &dev : s_devices) {
        for (const auto &ep : dev.endpoints) {
            if (count >= max) break;
            for (auto dt : ep.device_types) {
                if (dt == kDevTypeElectricalSensor) {
                    node_ids[count]     = dev.node_id;
                    endpoint_ids[count] = ep.endpoint_id;
                    count++;
                    break;
                }
            }
        }
    }
    xSemaphoreGive(s_mutex);
    return count;
}

void device_manager_log_structure(uint64_t node_id)
{
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    auto *dev = find_device(node_id);
    if (!dev) {
        xSemaphoreGive(s_mutex);
        ESP_LOGW(TAG, "Node 0x%llx not found in device manager", (unsigned long long)node_id);
        return;
    }

    bool is_bridge = false;
    for (const auto &ep : dev->endpoints) {
        for (auto dt : ep.device_types) {
            if (dt == kDevTypeAggregator) { is_bridge = true; break; }
        }
        if (is_bridge) break;
    }

    if (is_bridge) {
        ESP_LOGI(TAG, "Node 0x%llx is a BRIDGE (%s %s)",
                 (unsigned long long)node_id,
                 dev->vendor_name.c_str(), dev->product_name.c_str());

        for (const auto &ep : dev->endpoints) {
            bool bridged = false, solar = false, elecSensor = false;
            for (auto dt : ep.device_types) {
                if (dt == kDevTypeBridgedNode)      bridged    = true;
                if (dt == kDevTypeSolarPower)       solar      = true;
                if (dt == kDevTypeElectricalSensor) elecSensor = true;
            }
            if (bridged) {
                const char *tag = solar      ? " [SOLAR POWER 0x0017]"       :
                                  elecSensor ? " [ELECTRICAL SENSOR 0x0510]" : "";
                ESP_LOGI(TAG, "  endpoint %u: Bridged Node%s", ep.endpoint_id, tag);
            }
        }
    } else {
        ESP_LOGI(TAG, "Node 0x%llx is a direct device (%s %s)",
                 (unsigned long long)node_id,
                 dev->vendor_name.c_str(), dev->product_name.c_str());
        for (const auto &ep : dev->endpoints) {
            for (auto dt : ep.device_types) {
                ESP_LOGI(TAG, "  endpoint %u: device type 0x%04x", ep.endpoint_id, (unsigned)dt);
            }
        }
    }

    xSemaphoreGive(s_mutex);
}

esp_err_t device_manager_add_endpoint_part(uint64_t node_id, uint16_t parent_endpoint_id, uint16_t child_endpoint_id)
{
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    auto *dev = find_device(node_id);
    auto *ep  = find_endpoint(dev, parent_endpoint_id);
    if (ep) {
        bool found = false;
        for (auto p : ep->parts) { if (p == child_endpoint_id) { found = true; break; } }
        if (!found) ep->parts.push_back(child_endpoint_id);
    }
    xSemaphoreGive(s_mutex);
    return ep ? ESP_OK : ESP_ERR_NOT_FOUND;
}

esp_err_t device_manager_remove_device(uint64_t node_id)
{
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    auto it = s_devices.begin();
    while (it != s_devices.end()) {
        if (it->node_id == node_id) { it = s_devices.erase(it); break; }
        ++it;
    }
    xSemaphoreGive(s_mutex);
    return device_manager_persist();
}

esp_err_t device_manager_clear_device_endpoints(uint64_t node_id)
{
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    auto *dev = find_device(node_id);
    if (dev) dev->endpoints.clear();
    xSemaphoreGive(s_mutex);
    return dev ? ESP_OK : ESP_ERR_NOT_FOUND;
}

esp_err_t device_manager_clear(void)
{
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    s_devices.clear();
    s_next_node_id = FIRST_NODE_ID;
    xSemaphoreGive(s_mutex);
    return device_manager_persist();
}

esp_err_t device_manager_persist(void)
{
    xSemaphoreTake(s_mutex, portMAX_DELAY);

    cJSON *root = cJSON_CreateObject();
    cJSON_AddNumberToObject(root, "nextNodeId", (double)s_next_node_id);
    cJSON *arr = cJSON_AddArrayToObject(root, "devices");

    for (const auto &dev : s_devices) {
        cJSON *dobj = cJSON_CreateObject();
        cJSON_AddNumberToObject(dobj, "nodeId", (double)dev.node_id);
        cJSON_AddStringToObject(dobj, "vendorName",  dev.vendor_name.c_str());
        cJSON_AddStringToObject(dobj, "productName", dev.product_name.c_str());

        cJSON *eps = cJSON_AddArrayToObject(dobj, "endpoints");
        for (const auto &ep : dev.endpoints) {
            cJSON *eobj = cJSON_CreateObject();
            cJSON_AddNumberToObject(eobj, "endpointId", ep.endpoint_id);
            cJSON_AddStringToObject(eobj, "label", ep.label.c_str());
            cJSON_AddBoolToObject(eobj, "included", ep.included);
            cJSON *dts = cJSON_AddArrayToObject(eobj, "deviceTypes");
            for (auto dt : ep.device_types) {
                cJSON_AddItemToArray(dts, cJSON_CreateNumber((double)dt));
            }
            cJSON *parts = cJSON_AddArrayToObject(eobj, "parts");
            for (auto p : ep.parts) {
                cJSON_AddItemToArray(parts, cJSON_CreateNumber((double)p));
            }
            cJSON_AddItemToArray(eps, eobj);
        }
        cJSON_AddItemToArray(arr, dobj);
    }

    xSemaphoreGive(s_mutex);

    char *text = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!text) return ESP_ERR_NO_MEM;

    esp_err_t result = ESP_OK;
    FILE *f = fopen(DEVICES_TMP, "w");
    if (!f) { result = ESP_FAIL; goto done; }
    if (fputs(text, f) == EOF) { fclose(f); result = ESP_FAIL; goto done; }
    fflush(f);
    fsync(fileno(f));
    fclose(f);
    if (rename(DEVICES_TMP, DEVICES_PATH) != 0) result = ESP_FAIL;
done:
    free(text);
    if (result == ESP_OK) {
        ESP_LOGI(TAG, "Persisted %u device(s)", (unsigned)s_devices.size());
    } else {
        ESP_LOGE(TAG, "Failed to persist devices");
    }
    return result;
}
