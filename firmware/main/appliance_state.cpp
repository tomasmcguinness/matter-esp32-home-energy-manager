#include "appliance_state.h"
#include "value_cache.h"
#include "managers/device_manager.h"
#include "managers/node_manager.h"
#include "managers/subscription_manager.h"

#include <stdio.h>
#include <string.h>
#include <time.h>
#include <algorithm>
#include <mutex>
#include <string>
#include <vector>

#include "esp_log.h"
#include "cJSON.h"

#include <app-common/zap-generated/cluster-objects.h>
#include <app/data-model/Decode.h>

using namespace chip;
using namespace chip::app::Clusters;

#define TAG     "appliance_state"
#define SD_BASE "/sdcard"

static constexpr uint32_t kDeviceTypeDishwasher    = 0x0075;
static constexpr uint32_t kDeviceTypeLaundryWasher = 0x0073;

struct mode_option_t { uint8_t mode; std::string label; };

// One linked appliance: where its device lives, what it last reported, and the
// program catalogue its history is keyed by. Guarded by s_mutex: reports land on
// the CHIP thread, the log runs on the flush timer, the HTTP handlers read.
struct link_t {
    std::string graph_id;
    uint64_t    node_id      = 0;
    uint16_t    endpoint_id  = 0;
    uint32_t    device_type  = 0;
    uint32_t    mode_cluster = 0;

    uint8_t                    op_state     = APPLIANCE_UNKNOWN;
    uint8_t                    phase        = APPLIANCE_UNKNOWN;
    int32_t                    countdown_s  = -1;
    bool                       has_mode     = false;
    uint8_t                    current_mode = 0;
    bool                       bridged_reachable = true; // only bridges report it
    std::vector<std::string>   phases;
    std::vector<mode_option_t> modes;
    uint32_t                   last_report  = 0;

    std::vector<std::string> catalogue;
    bool                     catalogue_dirty = false;
};

static std::mutex          s_mutex;
static std::vector<link_t> s_links;

static const char *json_str(cJSON *obj, const char *key)
{
    cJSON *j = cJSON_GetObjectItemCaseSensitive(obj, key);
    return cJSON_IsString(j) ? j->valuestring : nullptr;
}

// Same guard the loggers use before building an /sdcard path from a graph id.
static bool sanitize_token(const char *tok, char *out, size_t out_len)
{
    if (!tok) return false;
    size_t n = strlen(tok);
    if (n == 0 || n > 32 || n >= out_len) return false;
    for (size_t i = 0; i < n; i++) {
        char c = tok[i];
        bool ok = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                  (c >= '0' && c <= '9') || c == '_' || c == '-';
        if (!ok) return false;
        out[i] = c;
    }
    out[n] = '\0';
    return true;
}

static uint32_t mode_cluster_for(uint32_t device_type)
{
    switch (device_type) {
    case kDeviceTypeDishwasher:    return DishwasherMode::Id;
    case kDeviceTypeLaundryWasher: return LaundryWasherMode::Id;
    default:                       return 0;
    }
}

// ---------------------------------------------------------------------------
// Program catalogue
// ---------------------------------------------------------------------------

static void load_catalogue(link_t &l)
{
    l.catalogue.clear();
    char path[96];
    snprintf(path, sizeof(path), "%s/programs-%s", SD_BASE, l.graph_id.c_str());
    FILE *f = fopen(path, "rb");
    if (!f) return;

    char buf[APPLIANCE_PROGRAM_MAX * (APPLIANCE_LABEL_LEN + 4) + 16];
    size_t n = fread(buf, 1, sizeof(buf) - 1, f);
    fclose(f);
    buf[n] = '\0';

    cJSON *arr = cJSON_Parse(buf);
    cJSON *item = nullptr;
    cJSON_ArrayForEach(item, arr) {
        if (l.catalogue.size() >= APPLIANCE_PROGRAM_MAX) break;
        l.catalogue.push_back(cJSON_IsString(item) ? item->valuestring : "");
    }
    cJSON_Delete(arr);
}

static void save_catalogue(const std::string &graph_id, const std::vector<std::string> &catalogue)
{
    cJSON *arr = cJSON_CreateArray();
    for (const auto &label : catalogue) cJSON_AddItemToArray(arr, cJSON_CreateString(label.c_str()));
    char *json = cJSON_PrintUnformatted(arr);
    cJSON_Delete(arr);
    if (!json) return;

    char path[96];
    snprintf(path, sizeof(path), "%s/programs-%s", SD_BASE, graph_id.c_str());
    FILE *f = fopen(path, "wb");
    if (f) {
        fputs(json, f);
        fclose(f);
    } else {
        ESP_LOGW(TAG, "Cannot write %s", path);
    }
    cJSON_free(json);
}

// Catalogue id of the device's current mode, appending its label if it is new.
// Caller holds s_mutex.
static uint8_t current_program_id_locked(link_t &l)
{
    if (!l.has_mode) return APPLIANCE_UNKNOWN;
    const std::string *label = nullptr;
    for (const auto &m : l.modes) if (m.mode == l.current_mode) { label = &m.label; break; }
    if (!label || label->empty()) return APPLIANCE_UNKNOWN;

    for (size_t i = 0; i < l.catalogue.size(); i++)
        if (l.catalogue[i] == *label) return (uint8_t)i;

    if (l.catalogue.size() >= APPLIANCE_PROGRAM_MAX) return APPLIANCE_UNKNOWN;
    l.catalogue.push_back(*label);
    l.catalogue_dirty = true; // written by the next log tick, off the CHIP thread
    ESP_LOGI(TAG, "%s: new program \"%s\" is id %u", l.graph_id.c_str(), label->c_str(),
             (unsigned)(l.catalogue.size() - 1));
    return (uint8_t)(l.catalogue.size() - 1);
}

static bool reachable_locked(const link_t &l)
{
    return l.bridged_reachable && device_manager_is_subscribed(l.node_id);
}

// ---------------------------------------------------------------------------
// Links
// ---------------------------------------------------------------------------

void appliance_state_refresh_links(bool resubscribe)
{
    std::vector<link_t> next;

    char *raw = node_manager_get_all_json();
    cJSON *root = raw ? cJSON_Parse(raw) : nullptr;
    free(raw);
    if (root) {
        cJSON *n = nullptr;
        cJSON_ArrayForEach(n, cJSON_GetObjectItemCaseSensitive(root, "nodes")) {
            cJSON *settings = cJSON_GetObjectItemCaseSensitive(n, "settings");
            cJSON *device   = cJSON_GetObjectItemCaseSensitive(settings, "device");
            cJSON *nid      = cJSON_GetObjectItemCaseSensitive(device, "nodeId");
            cJSON *eid      = cJSON_GetObjectItemCaseSensitive(device, "endpointId");
            if (!cJSON_IsNumber(nid) || !cJSON_IsNumber(eid)) continue;

            char clean[40];
            if (!sanitize_token(json_str(n, "id"), clean, sizeof(clean))) continue;
            if (next.size() >= APPLIANCE_STATE_MAX_LINKS) {
                ESP_LOGW(TAG, "More than %d linked appliances; ignoring %s", APPLIANCE_STATE_MAX_LINKS, clean);
                continue;
            }

            cJSON *dt = cJSON_GetObjectItemCaseSensitive(device, "deviceType");
            link_t l;
            l.graph_id     = clean;
            l.node_id      = (uint64_t)nid->valuedouble;
            l.endpoint_id  = (uint16_t)eid->valuedouble;
            l.device_type  = cJSON_IsNumber(dt) ? (uint32_t)dt->valuedouble : 0;
            l.mode_cluster = mode_cluster_for(l.device_type);
            next.push_back(std::move(l));
        }
        cJSON_Delete(root);
    }

    std::vector<uint64_t> changed_nodes;
    auto note_changed = [&](uint64_t node) {
        for (uint64_t c : changed_nodes) if (c == node) return;
        changed_nodes.push_back(node);
    };

    {
        std::lock_guard<std::mutex> lock(s_mutex);
        for (auto &l : next) {
            bool kept = false;
            for (auto &old : s_links) {
                if (old.graph_id == l.graph_id && old.node_id == l.node_id && old.endpoint_id == l.endpoint_id) {
                    // Same link: keep what the device has reported so far.
                    old.device_type  = l.device_type;
                    old.mode_cluster = l.mode_cluster;
                    l = std::move(old);
                    kept = true;
                    break;
                }
            }
            if (!kept) {
                load_catalogue(l);
                note_changed(l.node_id);
            }
        }
        for (const auto &old : s_links) {
            bool still = false;
            for (const auto &l : next)
                if (old.node_id == l.node_id && old.endpoint_id == l.endpoint_id) { still = true; break; }
            if (!still) note_changed(old.node_id);
        }
        s_links = std::move(next);
    }

    ESP_LOGI(TAG, "%u linked appliance device(s)", (unsigned)s_links.size());

    if (resubscribe)
        for (uint64_t node : changed_nodes) enqueue_subscription(node);
}

size_t appliance_state_linked_nodes(uint64_t *out, size_t max)
{
    std::lock_guard<std::mutex> lock(s_mutex);
    size_t n = 0;
    for (const auto &l : s_links) {
        bool seen = false;
        for (size_t i = 0; i < n; i++) if (out[i] == l.node_id) { seen = true; break; }
        if (!seen && n < max) out[n++] = l.node_id;
    }
    return n;
}

size_t appliance_state_linked_endpoints(uint64_t node_id, uint16_t *endpoints,
                                        uint32_t *mode_clusters, size_t max)
{
    std::lock_guard<std::mutex> lock(s_mutex);
    size_t n = 0;
    for (const auto &l : s_links) {
        if (l.node_id != node_id || n >= max) continue;
        endpoints[n]     = l.endpoint_id;
        mode_clusters[n] = l.mode_cluster;
        n++;
    }
    return n;
}

bool appliance_state_get(const char *graph_id, appliance_live_t *out)
{
    if (!graph_id || !out) return false;
    std::lock_guard<std::mutex> lock(s_mutex);
    for (auto &l : s_links) {
        if (l.graph_id != graph_id) continue;
        memset(out, 0, sizeof(*out));
        out->node_id          = l.node_id;
        out->endpoint_id      = l.endpoint_id;
        out->device_type      = l.device_type;
        out->reachable        = reachable_locked(l);
        out->op_state         = l.op_state;
        out->program_id       = current_program_id_locked(l);
        out->phase            = l.phase;
        out->countdown_s      = l.countdown_s;
        out->last_report_unix = l.last_report;
        if (out->program_id != APPLIANCE_UNKNOWN)
            snprintf(out->program, sizeof(out->program), "%s", l.catalogue[out->program_id].c_str());
        if (l.phase != APPLIANCE_UNKNOWN && l.phase < l.phases.size())
            snprintf(out->phase_name, sizeof(out->phase_name), "%s", l.phases[l.phase].c_str());
        return true;
    }
    return false;
}

bool appliance_state_program_name(const char *graph_id, uint8_t program_id, char *buf, size_t len)
{
    if (!graph_id || !buf || len == 0) return false;
    {
        std::lock_guard<std::mutex> lock(s_mutex);
        for (const auto &l : s_links) {
            if (l.graph_id != graph_id) continue;
            if (program_id >= l.catalogue.size()) return false;
            snprintf(buf, len, "%s", l.catalogue[program_id].c_str());
            return true;
        }
    }

    // Not linked any more, but its history may still name programs.
    char clean[40];
    if (!sanitize_token(graph_id, clean, sizeof(clean))) return false;
    link_t tmp;
    tmp.graph_id = clean;
    load_catalogue(tmp);
    if (program_id >= tmp.catalogue.size()) return false;
    snprintf(buf, len, "%s", tmp.catalogue[program_id].c_str());
    return true;
}

// ---------------------------------------------------------------------------
// Logging
// ---------------------------------------------------------------------------

void appliance_state_log_minute(void)
{
    struct pending_t {
        std::string graph_id;
        appliance_state_record_t rec;
        bool save_catalogue;
        std::vector<std::string> catalogue;
    };
    std::vector<pending_t> pending;

    time_t now = time(NULL);
    // The minute just finished, matching the power record the same flush writes.
    uint32_t minute = (uint32_t)(now - (now % 60)) - 60;

    {
        std::lock_guard<std::mutex> lock(s_mutex);
        for (auto &l : s_links) {
            pending_t p;
            p.graph_id = l.graph_id;
            bool reachable = reachable_locked(l);
            p.rec.unix_minute = minute;
            p.rec.op_state    = reachable ? l.op_state : APPLIANCE_UNKNOWN;
            p.rec.program_id  = current_program_id_locked(l);
            p.rec.phase       = reachable ? l.phase : APPLIANCE_UNKNOWN;
            p.rec.flags       = reachable ? APPLIANCE_STATE_FLAG_REACHABLE : 0;
            p.save_catalogue  = l.catalogue_dirty;
            if (l.catalogue_dirty) {
                p.catalogue = l.catalogue;
                l.catalogue_dirty = false;
            }
            pending.push_back(std::move(p));
        }
    }

    for (const auto &p : pending) {
        if (p.save_catalogue) save_catalogue(p.graph_id, p.catalogue);

        char date[16];
        time_t t = (time_t)p.rec.unix_minute;
        struct tm tm_info;
        localtime_r(&t, &tm_info);
        strftime(date, sizeof(date), "%Y-%m-%d", &tm_info);

        char path[96];
        snprintf(path, sizeof(path), "%s/state-%s-%s", SD_BASE, p.graph_id.c_str(), date);
        FILE *f = fopen(path, "ab");
        if (!f) {
            ESP_LOGW(TAG, "Cannot open %s for write", path);
            continue;
        }
        fwrite(&p.rec, sizeof(p.rec), 1, f);
        fclose(f);
    }
}

// ---------------------------------------------------------------------------
// Attribute reports (CHIP thread)
// ---------------------------------------------------------------------------

template <typename T>
static bool decode(TLV::TLVReader *data, T &out)
{
    return app::DataModel::Decode(*data, out) == CHIP_NO_ERROR;
}

void appliance_state_on_attribute(uint64_t node_id, const app::ConcreteDataAttributePath &path,
                                  TLV::TLVReader *data)
{
    std::lock_guard<std::mutex> lock(s_mutex);

    link_t *l = nullptr;
    for (auto &x : s_links)
        if (x.node_id == node_id && x.endpoint_id == path.mEndpointId) { l = &x; break; }
    if (!l) return;

    bool known = true;
    if (path.mClusterId == OperationalState::Id) {
        namespace Attr = OperationalState::Attributes;
        switch (path.mAttributeId) {
        case Attr::OperationalState::Id: {
            Attr::OperationalState::TypeInfo::DecodableType v;
            if (decode(data, v)) l->op_state = to_underlying(v);
            break;
        }
        case Attr::CurrentPhase::Id: {
            Attr::CurrentPhase::TypeInfo::DecodableType v;
            if (decode(data, v)) l->phase = v.IsNull() ? APPLIANCE_UNKNOWN : v.Value();
            break;
        }
        case Attr::CountdownTime::Id: {
            Attr::CountdownTime::TypeInfo::DecodableType v;
            if (decode(data, v)) l->countdown_s = v.IsNull() ? -1 : (int32_t)v.Value();
            break;
        }
        case Attr::PhaseList::Id: {
            Attr::PhaseList::TypeInfo::DecodableType v;
            if (!decode(data, v)) break;
            l->phases.clear();
            if (!v.IsNull()) {
                auto it = v.Value().begin();
                while (it.Next()) {
                    const CharSpan &s = it.GetValue();
                    l->phases.emplace_back(s.data(), std::min(s.size(), (size_t)APPLIANCE_LABEL_LEN - 1));
                }
            }
            break;
        }
        default:
            known = false;
            break;
        }
    } else if (path.mClusterId == l->mode_cluster && l->mode_cluster != 0) {
        // Dishwasher Mode and Laundry Washer Mode are both Mode Base clusters, so
        // their attributes share ids and types.
        namespace Attr = DishwasherMode::Attributes;
        switch (path.mAttributeId) {
        case Attr::SupportedModes::Id: {
            Attr::SupportedModes::TypeInfo::DecodableType v;
            if (!decode(data, v)) break;
            l->modes.clear();
            auto it = v.begin();
            while (it.Next()) {
                const auto &opt = it.GetValue();
                l->modes.push_back({ opt.mode, std::string(opt.label.data(),
                                     std::min(opt.label.size(), (size_t)APPLIANCE_LABEL_LEN - 1)) });
            }
            break;
        }
        case Attr::CurrentMode::Id: {
            Attr::CurrentMode::TypeInfo::DecodableType v;
            if (decode(data, v)) { l->current_mode = v; l->has_mode = true; }
            break;
        }
        default:
            known = false;
            break;
        }
    } else if (path.mClusterId == BridgedDeviceBasicInformation::Id &&
               path.mAttributeId == BridgedDeviceBasicInformation::Attributes::Reachable::Id) {
        bool v = true;
        if (decode(data, v)) l->bridged_reachable = v;
    } else {
        known = false;
    }

    if (!known) return;
    l->last_report = (uint32_t)time(NULL);

    // Mirror the integers into the ValueCache so the UI's attribute_batch frame
    // carries live state without a poll.
    if (path.mClusterId == OperationalState::Id &&
        path.mAttributeId == OperationalState::Attributes::OperationalState::Id && l->op_state != APPLIANCE_UNKNOWN)
        ValueCache::instance().put(node_id, path.mEndpointId, path.mClusterId, path.mAttributeId, l->op_state);
    else if (path.mClusterId == OperationalState::Id &&
             path.mAttributeId == OperationalState::Attributes::CurrentPhase::Id && l->phase != APPLIANCE_UNKNOWN)
        ValueCache::instance().put(node_id, path.mEndpointId, path.mClusterId, path.mAttributeId, l->phase);
    else if (path.mClusterId == l->mode_cluster &&
             path.mAttributeId == DishwasherMode::Attributes::CurrentMode::Id && l->has_mode)
        ValueCache::instance().put(node_id, path.mEndpointId, path.mClusterId, path.mAttributeId, l->current_mode);
}
