#include "commodity_price.h"

#include <stdio.h>
#include <string.h>
#include <time.h>
#include <array>
#include <map>
#include <mutex>
#include <string>

#include "esp_log.h"
#include "esp_timer.h"

#include <app-common/zap-generated/cluster-objects.h>
#include <app/data-model/Decode.h>
#include <lib/support/TimeUtils.h>

using namespace chip;
using namespace chip::app::Clusters;

#define TAG     "commodity_price"
#define SD_BASE "/sdcard"

static constexpr uint32_t kFileMagic     = 0x31435250; // "PRC1"
static constexpr uint64_t kFlushDelayUs  = 2 * 1000000ULL;
// Re-stamps the current slot so an open-ended price keeps being recorded across
// slot and day boundaries when the device only reports changes.
static constexpr uint64_t kTickPeriodUs  = 5 * 60 * 1000000ULL;
// Ignore any part of a reported period older than this (e.g. replayed events).
static constexpr time_t   kMaxBackfillS  = 2 * 24 * 3600;

struct file_header_t {
    uint32_t magic;
    uint16_t currency;
    uint8_t  decimals;
    uint8_t  unit;
};

struct current_price_t {
    bool    valid = false;
    time_t  start = 0;
    bool    has_end = false;
    time_t  end = 0;
    int64_t price = 0;
};

// Guarded by s_mutex: written on the CHIP thread, read by the timers.
static std::mutex      s_mutex;
static bool            s_has_source  = false; // source the state below belongs to
static uint64_t        s_node_id     = 0;
static uint16_t        s_endpoint_id = 0;
static uint16_t        s_currency    = 0;
static uint8_t         s_decimals    = 0;
static uint8_t         s_unit        = 0;
static current_price_t s_current;
// Slots stamped since the last flush, per date. TARIFF_NO_PRICE = untouched.
static std::map<std::string, std::array<int64_t, TARIFF_SLOTS>> s_pending;

static esp_timer_handle_t s_flush_timer = nullptr;
static esp_timer_handle_t s_tick_timer  = nullptr;

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

static void day_path(const char *date, char *buf, size_t len)
{
    snprintf(buf, len, "%s/price-%s", SD_BASE, date);
}

// ---------------------------------------------------------------------------
// Recording
// ---------------------------------------------------------------------------

// Stamp one 15-minute slot (the one containing t) with price. Call with s_mutex held.
static void stamp_slot(time_t t, int64_t price)
{
    struct tm lt;
    localtime_r(&t, &lt);
    char date[11];
    strftime(date, sizeof(date), "%Y-%m-%d", &lt);
    int slot = (lt.tm_hour * 60 + lt.tm_min) / TARIFF_SLOT_MINUTES;
    if (slot < 0 || slot >= TARIFF_SLOTS) return;

    auto it = s_pending.find(date);
    if (it == s_pending.end()) {
        std::array<int64_t, TARIFF_SLOTS> blank;
        blank.fill(TARIFF_NO_PRICE);
        it = s_pending.emplace(date, blank).first;
    }
    it->second[slot] = price;
}

// Stamp every slot the price covers, from its start up to its end or now,
// whichever is first. Future slots are left alone: they are recorded as they
// arrive. Call with s_mutex held.
static bool stamp_period(const current_price_t &p)
{
    if (!p.valid) return false;
    time_t now   = time(NULL);
    time_t start = p.start;
    if (start < now - kMaxBackfillS) start = now - kMaxBackfillS;
    time_t end = p.has_end && p.end < now ? p.end : now;
    if (start > now || (p.has_end && p.end <= start)) return false;

    // Always stamp the slot holding the start; then each later slot that begins
    // before the end (the end is exclusive).
    time_t t = start;
    do {
        stamp_slot(t, p.price);
        struct tm lt;
        localtime_r(&t, &lt);
        t += (TARIFF_SLOT_MINUTES - lt.tm_min % TARIFF_SLOT_MINUTES) * 60 - lt.tm_sec;
    } while (t < end);
    return true;
}

// Merge the pending slots into the day files. Runs on the esp_timer task.
static void on_flush_timer(void *)
{
    std::map<std::string, std::array<int64_t, TARIFF_SLOTS>> pending;
    file_header_t hdr;
    {
        std::lock_guard<std::mutex> lock(s_mutex);
        pending.swap(s_pending);
        hdr = { kFileMagic, s_currency, s_decimals, s_unit };
    }

    for (const auto &[date, slots] : pending) {
        tariff_day_t day;
        if (!commodity_price_load_day(date.c_str(), &day)) {
            for (int i = 0; i < TARIFF_SLOTS; i++) day.price[i] = TARIFF_NO_PRICE;
        }
        for (int i = 0; i < TARIFF_SLOTS; i++) {
            if (slots[i] != TARIFF_NO_PRICE) day.price[i] = slots[i];
        }

        char path[48];
        day_path(date.c_str(), path, sizeof(path));
        FILE *f = fopen(path, "wb");
        if (!f) {
            ESP_LOGW(TAG, "Cannot open %s for write", path);
            continue;
        }
        fwrite(&hdr, sizeof(hdr), 1, f);
        fwrite(day.price, sizeof(int64_t), TARIFF_SLOTS, f);
        fclose(f);
        ESP_LOGI(TAG, "Recorded price for %s", date.c_str());
    }
}

static void schedule_flush(void)
{
    if (!s_flush_timer) return;
    esp_timer_stop(s_flush_timer); // restart the debounce window; harmless if not running
    esp_timer_start_once(s_flush_timer, kFlushDelayUs);
}

static void on_tick_timer(void *)
{
    bool stamped;
    {
        std::lock_guard<std::mutex> lock(s_mutex);
        time_t now = time(NULL);
        stamped = s_current.valid && s_current.start <= now && (!s_current.has_end || now < s_current.end);
        if (stamped) stamp_slot(now, s_current.price);
    }
    if (stamped) schedule_flush();
}

// ---------------------------------------------------------------------------
// Decoding
// ---------------------------------------------------------------------------

// Is this report from the tariff source? A change of source drops the state
// gathered from the previous one; recorded days stay as history.
static bool from_source(uint64_t node_id, EndpointId endpoint_id)
{
    uint64_t node = 0;
    uint16_t ep   = 0;
    bool has = tariff_get_source(&node, &ep);

    std::lock_guard<std::mutex> lock(s_mutex);
    if (has != s_has_source || node != s_node_id || ep != s_endpoint_id) {
        s_has_source  = has;
        s_node_id     = node;
        s_endpoint_id = ep;
        s_currency = s_decimals = s_unit = 0;
        s_current  = current_price_t{};
    }
    return has && node_id == node && endpoint_id == ep;
}

template <typename T>
static bool decode(TLV::TLVReader *data, T &out)
{
    CHIP_ERROR err = app::DataModel::Decode(*data, out);
    if (err != CHIP_NO_ERROR) {
        ESP_LOGW(TAG, "Failed to decode commodity price: %" CHIP_ERROR_FORMAT, err.Format());
        return false;
    }
    return true;
}

// Make a CurrentPrice (from the attribute or a PriceChange event) the price in
// force and record the slots it covers. A null price, or one carrying only a
// price level, ends the current price without recording anything.
static void apply_price(const app::DataModel::Nullable<CommodityPrice::Structs::CommodityPriceStruct::DecodableType> &v)
{
    current_price_t p;
    if (!v.IsNull() && v.Value().price.HasValue()) {
        const auto &s = v.Value();
        p.valid   = true;
        p.start   = (time_t)s.periodStart + kChipEpochSecondsSinceUnixEpoch;
        p.has_end = !s.periodEnd.IsNull();
        p.end     = p.has_end ? (time_t)s.periodEnd.Value() + kChipEpochSecondsSinceUnixEpoch : 0;
        p.price   = s.price.Value();
    }

    bool stamped;
    {
        std::lock_guard<std::mutex> lock(s_mutex);
        // A replayed event older than the price we already hold is history:
        // record its slots but keep the newer price as current.
        if (!s_current.valid || !p.valid || p.start >= s_current.start) s_current = p;
        stamped = stamp_period(p);
    }
    if (stamped) {
        ESP_LOGI(TAG, "Price %lld from %lld", (long long)p.price, (long long)p.start);
        schedule_flush();
    }
}

void commodity_price_on_attribute(uint64_t node_id, const app::ConcreteDataAttributePath &path, TLV::TLVReader *data)
{
    namespace Attr = CommodityPrice::Attributes;
    if (!from_source(node_id, path.mEndpointId)) return;

    switch (path.mAttributeId) {
    case Attr::Currency::Id: {
        Attr::Currency::TypeInfo::DecodableType v;
        if (!decode(data, v)) break;
        std::lock_guard<std::mutex> lock(s_mutex);
        s_currency = v.IsNull() ? 0 : v.Value().currency;
        s_decimals = v.IsNull() ? 0 : v.Value().decimalPoints;
        break;
    }
    case Attr::TariffUnit::Id: {
        Attr::TariffUnit::TypeInfo::DecodableType v;
        if (!decode(data, v)) break;
        std::lock_guard<std::mutex> lock(s_mutex);
        s_unit = static_cast<uint8_t>(v);
        break;
    }
    case Attr::CurrentPrice::Id: {
        Attr::CurrentPrice::TypeInfo::DecodableType v;
        if (decode(data, v)) apply_price(v);
        break;
    }
    default:
        break;
    }
}

void commodity_price_on_event(uint64_t node_id, const app::EventHeader &header, TLV::TLVReader *data)
{
    if (header.mPath.mEventId != CommodityPrice::Events::PriceChange::Id) return;
    if (!from_source(node_id, header.mPath.mEndpointId)) return;

    CommodityPrice::Events::PriceChange::DecodableType ev;
    if (decode(data, ev)) apply_price(ev.currentPrice);
}

// ---------------------------------------------------------------------------
// Init + read API
// ---------------------------------------------------------------------------

esp_err_t commodity_price_init(void)
{
    esp_timer_create_args_t args = {};
    args.callback = on_flush_timer;
    args.name     = "price_flush";
    esp_err_t err = esp_timer_create(&args, &s_flush_timer);
    if (err != ESP_OK) return err;

    args.callback = on_tick_timer;
    args.name     = "price_tick";
    err = esp_timer_create(&args, &s_tick_timer);
    if (err != ESP_OK) return err;
    return esp_timer_start_periodic(s_tick_timer, kTickPeriodUs);
}

bool commodity_price_load_day(const char *date_str, tariff_day_t *out)
{
    if (!valid_date(date_str) || !out) return false;
    char path[48];
    day_path(date_str, path, sizeof(path));
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
