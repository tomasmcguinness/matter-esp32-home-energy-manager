#include "tariff.h"
#include "managers/node_manager.h"

#include <stdio.h>
#include <string.h>
#include <time.h>
#include <mutex>
#include <string>
#include <vector>

#include "esp_log.h"
#include "esp_timer.h"
#include "cJSON.h"

#include <app-common/zap-generated/cluster-objects.h>
#include <app/data-model/Decode.h>
#include <lib/support/TimeUtils.h>

using namespace chip;
using namespace chip::app::Clusters;

#define TAG     "tariff"
#define SD_BASE "/sdcard"

static constexpr uint32_t kFileMagic      = 0x31465254; // "TRF1"
static constexpr uint64_t kFlushDelayUs   = 2 * 1000000ULL;
static const char        *CONSUMER_UNIT_ID = "consumer_unit";
static const char        *TARIFF_HANDLE    = "tariff";

struct file_header_t {
    uint32_t magic;
    uint16_t currency;
    uint8_t  decimals;
    uint8_t  unit;
};

struct day_entry_t  { uint32_t id; uint16_t start; int32_t duration; }; // duration -1 = until next entry
struct period_t     { std::vector<uint32_t> entry_ids; std::vector<uint32_t> component_ids; };
struct component_t  { uint32_t id; bool has_price; int64_t price; bool has_threshold; int64_t threshold; };
struct day_t        { bool valid = false; uint32_t unix_date = 0; std::vector<uint32_t> entry_ids; };

// Decoded subset of the cluster, plus the assigned source. Guarded by s_mutex:
// written on the CHIP thread, read by the flush timer and the HTTP handlers.
static std::mutex               s_mutex;
static uint64_t                 s_node_id     = 0;
static uint16_t                 s_endpoint_id = 0;
static bool                     s_has_source  = false;
static uint16_t                 s_currency    = 0;
static uint8_t                  s_decimals    = 0;
static uint8_t                  s_unit        = 0;
static std::string              s_provider;
static std::string              s_label;
static std::vector<day_entry_t> s_entries;
static std::vector<period_t>    s_periods;
static std::vector<component_t> s_components;
static day_t                    s_current_day;
static day_t                    s_next_day;

static esp_timer_handle_t s_flush_timer = nullptr;

static bool valid_date(const char *d)
{
    // YYYY-MM-DD only; keeps the token filesystem-safe.
    if (!d || strlen(d) != 10) return false;
    for (int i = 0; i < 10; i++) {
        if (i == 4 || i == 7) { if (d[i] != '-') return false; }
        else if (d[i] < '0' || d[i] > '9') return false;
    }
    return true;
}

// DayStruct.Date is an epoch_s for the start of the day. Servers may send local
// or UTC midnight, so format the local date of midday to land on the intended
// calendar day either way.
static void date_of_day(uint32_t unix_date, char *buf, size_t len)
{
    time_t t = (time_t)unix_date + 12 * 3600;
    struct tm tm_info;
    localtime_r(&t, &tm_info);
    strftime(buf, len, "%Y-%m-%d", &tm_info);
}

static std::string span_str(const chip::CharSpan &s)
{
    return std::string(s.data(), s.size());
}

// ---------------------------------------------------------------------------
// Resolution
// ---------------------------------------------------------------------------

// Price for one day entry: the sum of its period's un-thresholded components.
// Block tariffs (every component thresholded) fall back to the lowest block.
// Call with s_mutex held.
static int64_t entry_price(uint32_t entry_id)
{
    bool    found = false;
    int64_t total = 0;
    bool    have_block = false;
    int64_t block_threshold = 0, block_price = 0;

    for (const auto &p : s_periods) {
        bool in_period = false;
        for (uint32_t id : p.entry_ids) if (id == entry_id) { in_period = true; break; }
        if (!in_period) continue;

        for (uint32_t cid : p.component_ids) {
            for (const auto &c : s_components) {
                if (c.id != cid || !c.has_price) continue;
                if (!c.has_threshold) {
                    total += c.price;
                    found = true;
                } else if (!have_block || c.threshold < block_threshold) {
                    have_block      = true;
                    block_threshold = c.threshold;
                    block_price     = c.price;
                }
            }
        }
    }
    if (found) return total;
    if (have_block) return block_price;
    return TARIFF_NO_PRICE;
}

// Resolve a DayStruct to 96 x 15-minute prices. Each slot takes the entry with
// the latest StartTime at or before the slot start, unless that entry's
// Duration has already elapsed. Call with s_mutex held.
static void resolve_day(const day_t &day, int64_t out[TARIFF_SLOTS])
{
    for (int slot = 0; slot < TARIFF_SLOTS; slot++) {
        int minute = slot * TARIFF_SLOT_MINUTES;
        const day_entry_t *best = nullptr;
        for (uint32_t id : day.entry_ids) {
            for (const auto &e : s_entries) {
                if (e.id != id || e.start > minute) continue;
                if (!best || e.start > best->start) best = &e;
            }
        }
        if (!best || (best->duration >= 0 && minute >= best->start + best->duration)) {
            out[slot] = TARIFF_NO_PRICE;
            continue;
        }
        out[slot] = entry_price(best->id);
    }
}

static void write_day(const char *date, const file_header_t &hdr, const int64_t price[TARIFF_SLOTS])
{
    char path[48];
    snprintf(path, sizeof(path), "%s/tariff-%s", SD_BASE, date);
    FILE *f = fopen(path, "wb");
    if (!f) {
        ESP_LOGW(TAG, "Cannot open %s for write", path);
        return;
    }
    fwrite(&hdr, sizeof(hdr), 1, f);
    fwrite(price, sizeof(int64_t), TARIFF_SLOTS, f);
    fclose(f);
}

// Resolve CurrentDay and NextDay and persist them. Runs on the esp_timer task a
// moment after the last attribute report, so a priming report's burst of
// attributes produces one write per day rather than one per attribute.
static void on_flush_timer(void *)
{
    struct pending_t { char date[11]; int64_t price[TARIFF_SLOTS]; };
    std::vector<pending_t> pending;
    file_header_t hdr = {};
    bool kvah = false;

    {
        std::lock_guard<std::mutex> lock(s_mutex);
        hdr = { kFileMagic, s_currency, s_decimals, s_unit };
        kvah = s_unit == 1;
        for (const day_t *d : { &s_current_day, &s_next_day }) {
            if (!d->valid) continue;
            pending_t p;
            date_of_day(d->unix_date, p.date, sizeof(p.date));
            resolve_day(*d, p.price);
            pending.push_back(p);
        }
    }

    if (kvah) ESP_LOGW(TAG, "Tariff is priced per kVAh; treating it as per kWh");
    for (const auto &p : pending) {
        write_day(p.date, hdr, p.price);
        ESP_LOGI(TAG, "Wrote tariff for %s", p.date);
    }
}

static void schedule_flush(void)
{
    if (!s_flush_timer) return;
    esp_timer_stop(s_flush_timer); // restart the debounce window; harmless if not running
    esp_timer_start_once(s_flush_timer, kFlushDelayUs);
}

// ---------------------------------------------------------------------------
// Attribute decoding
// ---------------------------------------------------------------------------

template <typename T>
static bool decode(TLV::TLVReader *data, T &out)
{
    CHIP_ERROR err = app::DataModel::Decode(*data, out);
    if (err != CHIP_NO_ERROR) {
        ESP_LOGW(TAG, "Failed to decode tariff attribute: %" CHIP_ERROR_FORMAT, err.Format());
        return false;
    }
    return true;
}

static bool decode_day(TLV::TLVReader *data, day_t &out)
{
    CommodityTariff::Attributes::CurrentDay::TypeInfo::DecodableType v; // same type as NextDay
    if (!decode(data, v)) return false;
    day_t d;
    if (!v.IsNull()) {
        d.valid     = true;
        d.unix_date = v.Value().date + kChipEpochSecondsSinceUnixEpoch;
        auto it = v.Value().dayEntryIDs.begin();
        while (it.Next()) d.entry_ids.push_back(it.GetValue());
        if (it.GetStatus() != CHIP_NO_ERROR) return false;
    }
    std::lock_guard<std::mutex> lock(s_mutex);
    out = std::move(d);
    return true;
}

void tariff_on_attribute(uint64_t node_id, const app::ConcreteDataAttributePath &path, TLV::TLVReader *data)
{
    namespace Attr = CommodityTariff::Attributes;

    {
        std::lock_guard<std::mutex> lock(s_mutex);
        if (!s_has_source || node_id != s_node_id || path.mEndpointId != s_endpoint_id) return;
    }

    bool changed = false;
    switch (path.mAttributeId) {
    case Attr::TariffInfo::Id: {
        Attr::TariffInfo::TypeInfo::DecodableType v;
        if (!decode(data, v)) break;
        std::lock_guard<std::mutex> lock(s_mutex);
        s_provider.clear();
        s_label.clear();
        s_currency = 0;
        s_decimals = 0;
        if (!v.IsNull()) {
            const auto &info = v.Value();
            if (!info.providerName.IsNull()) s_provider = span_str(info.providerName.Value());
            if (!info.tariffLabel.IsNull()) s_label = span_str(info.tariffLabel.Value());
            if (info.currency.HasValue() && !info.currency.Value().IsNull()) {
                s_currency = info.currency.Value().Value().currency;
                s_decimals = info.currency.Value().Value().decimalPoints;
            }
            if (!info.blockMode.IsNull() && info.blockMode.Value() != CommodityTariff::BlockModeEnum::kNoBlock)
                ESP_LOGW(TAG, "Block tariff: costs use the lowest block's price");
        }
        changed = true;
        break;
    }
    case Attr::TariffUnit::Id: {
        Attr::TariffUnit::TypeInfo::DecodableType v;
        if (!decode(data, v)) break;
        std::lock_guard<std::mutex> lock(s_mutex);
        s_unit = v.IsNull() ? 0 : static_cast<uint8_t>(v.Value());
        changed = true;
        break;
    }
    case Attr::DayEntries::Id: {
        Attr::DayEntries::TypeInfo::DecodableType v;
        if (!decode(data, v)) break;
        std::vector<day_entry_t> entries;
        if (!v.IsNull()) {
            auto it = v.Value().begin();
            while (it.Next()) {
                const auto &e = it.GetValue();
                entries.push_back({ e.dayEntryID, e.startTime,
                                    e.duration.HasValue() ? (int32_t)e.duration.Value() : -1 });
            }
            if (it.GetStatus() != CHIP_NO_ERROR) break;
        }
        std::lock_guard<std::mutex> lock(s_mutex);
        s_entries = std::move(entries);
        changed = true;
        break;
    }
    case Attr::TariffPeriods::Id: {
        Attr::TariffPeriods::TypeInfo::DecodableType v;
        if (!decode(data, v)) break;
        std::vector<period_t> periods;
        bool ok = true;
        if (!v.IsNull()) {
            auto it = v.Value().begin();
            while (it.Next()) {
                const auto &p = it.GetValue();
                period_t out;
                auto ei = p.dayEntryIDs.begin();
                while (ei.Next()) out.entry_ids.push_back(ei.GetValue());
                auto ci = p.tariffComponentIDs.begin();
                while (ci.Next()) out.component_ids.push_back(ci.GetValue());
                if (ei.GetStatus() != CHIP_NO_ERROR || ci.GetStatus() != CHIP_NO_ERROR) ok = false;
                periods.push_back(std::move(out));
            }
            if (it.GetStatus() != CHIP_NO_ERROR) ok = false;
        }
        if (!ok) break;
        std::lock_guard<std::mutex> lock(s_mutex);
        s_periods = std::move(periods);
        changed = true;
        break;
    }
    case Attr::TariffComponents::Id: {
        Attr::TariffComponents::TypeInfo::DecodableType v;
        if (!decode(data, v)) break;
        std::vector<component_t> comps;
        if (!v.IsNull()) {
            auto it = v.Value().begin();
            while (it.Next()) {
                const auto &c = it.GetValue();
                component_t out = { c.tariffComponentID, false, 0, !c.threshold.IsNull(),
                                    c.threshold.IsNull() ? 0 : c.threshold.Value() };
                if (c.price.HasValue() && !c.price.Value().IsNull() && c.price.Value().Value().price.HasValue()) {
                    out.has_price = true;
                    out.price     = c.price.Value().Value().price.Value();
                }
                comps.push_back(out);
            }
            if (it.GetStatus() != CHIP_NO_ERROR) break;
        }
        std::lock_guard<std::mutex> lock(s_mutex);
        s_components = std::move(comps);
        changed = true;
        break;
    }
    case Attr::CurrentDay::Id:
        changed = decode_day(data, s_current_day);
        break;
    case Attr::NextDay::Id:
        changed = decode_day(data, s_next_day);
        break;
    default:
        break; // other attributes aren't needed to price a day
    }

    if (changed) schedule_flush();
}

// ---------------------------------------------------------------------------
// Source + read API
// ---------------------------------------------------------------------------

static const char *json_str(cJSON *obj, const char *key)
{
    cJSON *j = cJSON_GetObjectItemCaseSensitive(obj, key);
    return cJSON_IsString(j) ? j->valuestring : nullptr;
}

void tariff_refresh_source(void)
{
    bool     found = false;
    uint64_t node  = 0;
    uint16_t ep    = 0;

    char *raw = node_manager_get_all_json();
    cJSON *root = raw ? cJSON_Parse(raw) : nullptr;
    free(raw);
    if (root) {
        cJSON *nodes = cJSON_GetObjectItemCaseSensitive(root, "nodes");
        cJSON *edges = cJSON_GetObjectItemCaseSensitive(root, "edges");

        // The handle IS the role: whatever is wired to the CU's tariff handle.
        const char *tariff_id = nullptr;
        cJSON *e = nullptr;
        cJSON_ArrayForEach(e, edges) {
            const char *src = json_str(e, "source"), *tgt = json_str(e, "target");
            const char *sh = json_str(e, "sourceHandle"), *th = json_str(e, "targetHandle");
            if (tgt && src && strcmp(tgt, CONSUMER_UNIT_ID) == 0 && th && strcmp(th, TARIFF_HANDLE) == 0) { tariff_id = src; break; }
            if (src && tgt && strcmp(src, CONSUMER_UNIT_ID) == 0 && sh && strcmp(sh, TARIFF_HANDLE) == 0) { tariff_id = tgt; break; }
        }

        cJSON *n = nullptr;
        cJSON_ArrayForEach(n, nodes) {
            const char *id = json_str(n, "id");
            if (!tariff_id || !id || strcmp(id, tariff_id) != 0) continue;
            cJSON *settings = cJSON_GetObjectItemCaseSensitive(n, "settings");
            cJSON *nid = cJSON_GetObjectItemCaseSensitive(settings, "nodeId");
            cJSON *eid = cJSON_GetObjectItemCaseSensitive(settings, "endpointId");
            if (cJSON_IsNumber(nid) && cJSON_IsNumber(eid)) {
                found = true;
                node  = (uint64_t)nid->valuedouble;
                ep    = (uint16_t)eid->valuedouble;
            }
            break;
        }
        cJSON_Delete(root);
    }

    std::lock_guard<std::mutex> lock(s_mutex);
    bool changed = found != s_has_source || node != s_node_id || ep != s_endpoint_id;
    s_has_source  = found;
    s_node_id     = node;
    s_endpoint_id = ep;
    if (changed) {
        // A different source: forget the old tariff. Persisted days stay as history.
        s_provider.clear();
        s_label.clear();
        s_currency = s_decimals = s_unit = 0;
        s_entries.clear();
        s_periods.clear();
        s_components.clear();
        s_current_day = day_t{};
        s_next_day    = day_t{};
        if (found) ESP_LOGI(TAG, "Tariff source: node 0x%016llX endpoint %u", (unsigned long long)node, ep);
        else       ESP_LOGI(TAG, "No tariff source assigned");
    }
}

bool tariff_get_source(uint64_t *node_id, uint16_t *endpoint_id)
{
    std::lock_guard<std::mutex> lock(s_mutex);
    if (!s_has_source) return false;
    if (node_id) *node_id = s_node_id;
    if (endpoint_id) *endpoint_id = s_endpoint_id;
    return true;
}

esp_err_t tariff_init(void)
{
    esp_timer_create_args_t args = {};
    args.callback = on_flush_timer;
    args.name     = "tariff_flush";
    esp_err_t err = esp_timer_create(&args, &s_flush_timer);
    if (err != ESP_OK) return err;
    tariff_refresh_source();
    return ESP_OK;
}

bool tariff_load_day(const char *date_str, tariff_day_t *out)
{
    if (!valid_date(date_str) || !out) return false;
    char path[48];
    snprintf(path, sizeof(path), "%s/tariff-%s", SD_BASE, date_str);
    FILE *f = fopen(path, "rb");
    if (!f) return false;
    file_header_t hdr;
    bool ok = fread(&hdr, sizeof(hdr), 1, f) == 1 && hdr.magic == kFileMagic &&
              fread(out->price, sizeof(int64_t), TARIFF_SLOTS, f) == TARIFF_SLOTS;
    fclose(f);
    if (!ok) return false;
    out->currency = hdr.currency;
    out->decimals = hdr.decimals;
    out->unit     = hdr.unit;
    return true;
}

char *tariff_day_json(const char *date_str)
{
    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "date", valid_date(date_str) ? date_str : "");

    {
        std::lock_guard<std::mutex> lock(s_mutex);
        cJSON_AddBoolToObject(root, "assigned", s_has_source);
        if (!s_provider.empty()) cJSON_AddStringToObject(root, "provider", s_provider.c_str());
        if (!s_label.empty()) cJSON_AddStringToObject(root, "label", s_label.c_str());
    }

    tariff_day_t day;
    cJSON *slots = cJSON_AddArrayToObject(root, "slots");
    if (tariff_load_day(date_str, &day)) {
        cJSON_AddNumberToObject(root, "currency", day.currency);
        cJSON_AddNumberToObject(root, "decimals", day.decimals);
        cJSON_AddNumberToObject(root, "unit", day.unit);
        for (int i = 0; i < TARIFF_SLOTS; i++) {
            if (day.price[i] == TARIFF_NO_PRICE) cJSON_AddItemToArray(slots, cJSON_CreateNull());
            else cJSON_AddItemToArray(slots, cJSON_CreateNumber((double)day.price[i]));
        }
    }

    char *json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    return json; // caller must free
}
