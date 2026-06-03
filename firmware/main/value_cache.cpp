#include "value_cache.h"

#include <ctime>

#include "esp_log.h"

static const char *TAG = "value_cache";

ValueCache &ValueCache::instance()
{
    static ValueCache s_instance;
    return s_instance;
}

ValueCache::ValueCache()
{
    m_entries.reserve(kMaxEntries);
}

ValueCacheEntry *ValueCache::find_or_claim(uint64_t node_id, uint16_t endpoint_id,
                                           uint32_t cluster_id, uint32_t attribute_id, bool create)
{
    for (auto &e : m_entries) {
        if (e.node_id == node_id && e.endpoint_id == endpoint_id &&
            e.cluster_id == cluster_id && e.attribute_id == attribute_id) {
            return &e;
        }
    }

    if (!create) return nullptr;

    if (m_entries.size() >= kMaxEntries) {
        ESP_LOGW(TAG, "Cache full (%u entries), dropping node 0x%llx ep %u cluster 0x%lx attr 0x%lx",
                 (unsigned)kMaxEntries, (unsigned long long)node_id, (unsigned)endpoint_id,
                 (unsigned long)cluster_id, (unsigned long)attribute_id);
        return nullptr;
    }

    m_entries.push_back(ValueCacheEntry{node_id, endpoint_id, cluster_id, attribute_id, 0, false, 0});
    return &m_entries.back();
}

void ValueCache::put(uint64_t node_id, uint16_t endpoint_id, uint32_t cluster_id,
                     uint32_t attribute_id, int64_t value)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    ValueCacheEntry *e = find_or_claim(node_id, endpoint_id, cluster_id, attribute_id, true);
    if (!e) return;
    e->value            = value;
    e->valid            = true;
    e->last_update_unix = (uint32_t)time(nullptr);
}

void ValueCache::seed(uint64_t node_id, uint16_t endpoint_id, uint32_t cluster_id,
                      uint32_t attribute_id)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    find_or_claim(node_id, endpoint_id, cluster_id, attribute_id, true);
}

std::vector<ValueCacheEntry> ValueCache::snapshot() const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_entries;
}

void ValueCache::clear()
{
    std::lock_guard<std::mutex> lock(m_mutex);
    m_entries.clear();
}
