#include "node_manager.h"

#include <stdio.h>
#include <unistd.h>
#include <vector>
#include <string>

#include "esp_log.h"
#include "esp_littlefs.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "cJSON.h"

#include "value_cache.h"

static const char *TAG = "node_config_manager";

#define LFS_BASE_PATH "/littlefs"
#define NODES_PATH    LFS_BASE_PATH "/nodes.json"
#define NODES_TMP     LFS_BASE_PATH "/nodes.json.tmp"

struct node_config_t {
    std::string id;
    float x = 0.0f;
    float y = 0.0f;
    std::string settings_json = "{}";
};

struct edge_config_t {
    std::string id;
    std::string source;
    std::string target;
    std::string source_handle;
    std::string target_handle;
};

static std::vector<node_config_t> s_nodes;
static std::vector<edge_config_t> s_edges;
static SemaphoreHandle_t s_mutex = nullptr;

static node_config_t *find_node(const char *id)
{
    for (auto &n : s_nodes) {
        if (n.id == id) return &n;
    }
    return nullptr;
}

// When values is non-null, each node that carries a Matter identity
// (settings.nodeId + settings.endpointId) gets a transient "values" array of
// {clusterId, attributeId, value} from the cache. Persisted JSON passes null so
// these volatile values are never written to disk.
static cJSON *build_json(const std::vector<ValueCacheEntry> *values)
{
    cJSON *root = cJSON_CreateObject();
    cJSON *arr  = cJSON_AddArrayToObject(root, "nodes");
    for (const auto &nc : s_nodes) {
        cJSON *obj = cJSON_CreateObject();
        cJSON_AddStringToObject(obj, "id", nc.id.c_str());
        cJSON_AddNumberToObject(obj, "x", nc.x);
        cJSON_AddNumberToObject(obj, "y", nc.y);
        cJSON *settings = cJSON_Parse(nc.settings_json.c_str());
        if (!settings) settings = cJSON_CreateObject();
        cJSON_AddItemToObject(obj, "settings", settings);

        if (values) {
            cJSON *nid_j = cJSON_GetObjectItemCaseSensitive(settings, "nodeId");
            cJSON *eid_j = cJSON_GetObjectItemCaseSensitive(settings, "endpointId");
            if (cJSON_IsNumber(nid_j) && cJSON_IsNumber(eid_j)) {
                uint64_t node_id     = (uint64_t)nid_j->valuedouble;
                uint16_t endpoint_id = (uint16_t)eid_j->valuedouble;
                cJSON *vals = cJSON_CreateArray();
                for (const auto &e : *values) {
                    if (!e.valid || e.node_id != node_id || e.endpoint_id != endpoint_id) continue;
                    cJSON *v = cJSON_CreateObject();
                    cJSON_AddNumberToObject(v, "clusterId", (double)e.cluster_id);
                    cJSON_AddNumberToObject(v, "attributeId", (double)e.attribute_id);
                    cJSON_AddNumberToObject(v, "value", (double)e.value);
                    cJSON_AddItemToArray(vals, v);
                }
                if (cJSON_GetArraySize(vals) > 0) cJSON_AddItemToObject(obj, "values", vals);
                else cJSON_Delete(vals);
            }
        }

        cJSON_AddItemToArray(arr, obj);
    }
    cJSON *edges = cJSON_AddArrayToObject(root, "edges");
    for (const auto &ec : s_edges) {
        cJSON *obj = cJSON_CreateObject();
        cJSON_AddStringToObject(obj, "id", ec.id.c_str());
        cJSON_AddStringToObject(obj, "source", ec.source.c_str());
        cJSON_AddStringToObject(obj, "target", ec.target.c_str());
        if (!ec.source_handle.empty()) cJSON_AddStringToObject(obj, "sourceHandle", ec.source_handle.c_str());
        if (!ec.target_handle.empty()) cJSON_AddStringToObject(obj, "targetHandle", ec.target_handle.c_str());
        cJSON_AddItemToArray(edges, obj);
    }
    return root;
}

static void load_from_disk(void)
{
    FILE *f = fopen(NODES_PATH, "r");
    if (!f) { ESP_LOGI(TAG, "No nodes file; starting fresh"); return; }

    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (size <= 0 || size > 16384) { fclose(f); return; }

    char *buf = (char *)malloc(size + 1);
    if (!buf) { fclose(f); return; }
    size_t n = fread(buf, 1, size, f);
    fclose(f);
    buf[n] = '\0';

    cJSON *root = cJSON_Parse(buf);
    free(buf);
    if (!root) return;

    cJSON *nodes = cJSON_GetObjectItemCaseSensitive(root, "nodes");
    cJSON *item  = nullptr;
    cJSON_ArrayForEach(item, nodes) {
        cJSON *id = cJSON_GetObjectItemCaseSensitive(item, "id");
        if (!cJSON_IsString(id)) continue;

        node_config_t nc;
        nc.id = id->valuestring;

        cJSON *x = cJSON_GetObjectItemCaseSensitive(item, "x");
        if (cJSON_IsNumber(x)) nc.x = (float)x->valuedouble;

        cJSON *y = cJSON_GetObjectItemCaseSensitive(item, "y");
        if (cJSON_IsNumber(y)) nc.y = (float)y->valuedouble;

        cJSON *settings = cJSON_GetObjectItemCaseSensitive(item, "settings");
        if (settings) {
            char *s = cJSON_PrintUnformatted(settings);
            if (s) { nc.settings_json = s; free(s); }
        }
        s_nodes.push_back(nc);
    }
    cJSON *edges = cJSON_GetObjectItemCaseSensitive(root, "edges");
    cJSON *edge_item = nullptr;
    cJSON_ArrayForEach(edge_item, edges) {
        cJSON *eid = cJSON_GetObjectItemCaseSensitive(edge_item, "id");
        if (!cJSON_IsString(eid)) continue;
        edge_config_t ec;
        ec.id = eid->valuestring;
        cJSON *src = cJSON_GetObjectItemCaseSensitive(edge_item, "source");
        if (cJSON_IsString(src)) ec.source = src->valuestring;
        cJSON *tgt = cJSON_GetObjectItemCaseSensitive(edge_item, "target");
        if (cJSON_IsString(tgt)) ec.target = tgt->valuestring;
        cJSON *sh = cJSON_GetObjectItemCaseSensitive(edge_item, "sourceHandle");
        if (cJSON_IsString(sh)) ec.source_handle = sh->valuestring;
        cJSON *th = cJSON_GetObjectItemCaseSensitive(edge_item, "targetHandle");
        if (cJSON_IsString(th)) ec.target_handle = th->valuestring;
        s_edges.push_back(ec);
    }

    cJSON_Delete(root);
    ESP_LOGI(TAG, "Loaded %u node config(s), %u edge(s)", (unsigned)s_nodes.size(), (unsigned)s_edges.size());
}

esp_err_t node_manager_init(void)
{
    s_mutex = xSemaphoreCreateMutex();
    if (!s_mutex) return ESP_ERR_NO_MEM;

    // LittleFS is already mounted by device_manager; tolerate already-mounted error
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

    if (!find_node("consumer_unit")) {
        node_config_t nc;
        nc.id            = "consumer_unit";
        nc.x             = 0.0f;
        nc.y             = 0.0f;
        nc.settings_json = "{\"label\":\"🏠 Consumer Unit\",\"type\":\"consumerUnit\",\"deletable\":false}";
        s_nodes.push_back(nc);
        node_manager_persist();
    }

    return ESP_OK;
}

esp_err_t node_manager_upsert(const char *node_id, float x, float y, const char *settings_json)
{
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    auto *nc = find_node(node_id);
    if (nc) {
        nc->x = x;
        nc->y = y;
        if (settings_json) {
            cJSON *parsed = cJSON_Parse(settings_json);
            if (parsed) {
                char *canonical = cJSON_PrintUnformatted(parsed);
                cJSON_Delete(parsed);
                if (canonical) { nc->settings_json = canonical; free(canonical); }
            }
        }
    } else {
        node_config_t n;
        n.id = node_id;
        n.x  = x;
        n.y  = y;
        if (settings_json) {
            cJSON *parsed = cJSON_Parse(settings_json);
            if (parsed) {
                char *canonical = cJSON_PrintUnformatted(parsed);
                cJSON_Delete(parsed);
                if (canonical) { n.settings_json = canonical; free(canonical); }
            }
        }
        s_nodes.push_back(n);
    }
    xSemaphoreGive(s_mutex);
    return node_manager_persist();
}

esp_err_t node_manager_delete(const char *node_id)
{
    if (!node_id) return ESP_ERR_INVALID_ARG;
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    auto *nc = find_node(node_id);
    if (nc) {
        cJSON *settings = cJSON_Parse(nc->settings_json.c_str());
        cJSON *deletable_j = settings ? cJSON_GetObjectItemCaseSensitive(settings, "deletable") : nullptr;
        bool locked = cJSON_IsFalse(deletable_j);
        cJSON_Delete(settings);
        if (locked) {
            xSemaphoreGive(s_mutex);
            return ESP_ERR_NOT_SUPPORTED;
        }
    }
    auto it = s_nodes.begin();
    while (it != s_nodes.end()) {
        if (it->id == node_id) { it = s_nodes.erase(it); break; }
        ++it;
    }
    xSemaphoreGive(s_mutex);
    return node_manager_persist();
}

esp_err_t node_manager_update_settings(const char *node_id, const char *settings_json)
{
    if (!node_id || !settings_json) return ESP_ERR_INVALID_ARG;
    cJSON *parsed = cJSON_Parse(settings_json);
    if (!parsed) return ESP_ERR_INVALID_ARG;
    char *canonical = cJSON_PrintUnformatted(parsed);
    cJSON_Delete(parsed);
    if (!canonical) return ESP_ERR_NO_MEM;

    xSemaphoreTake(s_mutex, portMAX_DELAY);
    auto *nc = find_node(node_id);
    if (nc) {
        nc->settings_json = canonical;
    } else {
        node_config_t n;
        n.id = node_id;
        n.settings_json = canonical;
        s_nodes.push_back(n);
    }
    xSemaphoreGive(s_mutex);
    free(canonical);
    return node_manager_persist();
}

esp_err_t node_manager_upsert_edge(const char *id, const char *source, const char *target,
                                    const char *source_handle, const char *target_handle)
{
    if (!id || !source || !target) return ESP_ERR_INVALID_ARG;
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    edge_config_t *found = nullptr;
    for (auto &ec : s_edges) {
        if (ec.id == id) { found = &ec; break; }
    }
    if (found) {
        found->source        = source;
        found->target        = target;
        found->source_handle = source_handle ? source_handle : "";
        found->target_handle = target_handle ? target_handle : "";
    } else {
        edge_config_t ec;
        ec.id            = id;
        ec.source        = source;
        ec.target        = target;
        ec.source_handle = source_handle ? source_handle : "";
        ec.target_handle = target_handle ? target_handle : "";
        s_edges.push_back(ec);
    }
    xSemaphoreGive(s_mutex);
    return node_manager_persist();
}

esp_err_t node_manager_delete_edge(const char *id)
{
    if (!id) return ESP_ERR_INVALID_ARG;
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    for (auto it = s_edges.begin(); it != s_edges.end(); ++it) {
        if (it->id == id) { s_edges.erase(it); break; }
    }
    xSemaphoreGive(s_mutex);
    return node_manager_persist();
}

esp_err_t node_manager_clear(void)
{
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    s_nodes.clear();
    s_edges.clear();
    xSemaphoreGive(s_mutex);
    return node_manager_persist();
}

char *node_manager_get_all_json(void)
{
    // Snapshot the value cache before taking s_mutex to keep lock scopes separate.
    std::vector<ValueCacheEntry> values = ValueCache::instance().snapshot();
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    cJSON *root = build_json(&values);
    xSemaphoreGive(s_mutex);
    char *text = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    return text;
}

esp_err_t node_manager_persist(void)
{
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    cJSON *root = build_json(nullptr);
    xSemaphoreGive(s_mutex);

    char *text = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!text) return ESP_ERR_NO_MEM;

    esp_err_t result = ESP_OK;
    FILE *f = fopen(NODES_TMP, "w");
    if (!f) { result = ESP_FAIL; goto done; }
    if (fputs(text, f) == EOF) { fclose(f); result = ESP_FAIL; goto done; }
    fflush(f);
    fsync(fileno(f));
    fclose(f);
    if (rename(NODES_TMP, NODES_PATH) != 0) result = ESP_FAIL;
done:
    free(text);
    if (result == ESP_OK)
        ESP_LOGI(TAG, "Persisted %u node config(s)", (unsigned)s_nodes.size());
    else
        ESP_LOGE(TAG, "Failed to persist node configs");
    return result;
}
