#pragma once

#include <cstdint>
#include <cstddef>
#include <mutex>
#include <vector>

// Generic in-memory cache of the latest value of an attribute, keyed by
// node + endpoint + cluster + attribute. Matter subscriptions only report on
// change, so this holds the most recent value between reports for the HTTP API
// to serve on load. This class is deliberately not Matter-aware.

struct ValueCacheEntry {
    uint64_t node_id;
    uint16_t endpoint_id;
    uint32_t cluster_id;
    uint32_t attribute_id;
    int64_t  value;             // raw attribute value as delivered (e.g. mW, mV, mA)
    bool     valid;             // false until first real update (seeded entries start invalid)
    uint32_t last_update_unix;  // time of last put() for this entry
};

class ValueCache {
public:
    static ValueCache &instance();

    // Upsert the value for a key. Marks the entry valid and stamps last_update_unix.
    void put(uint64_t node_id, uint16_t endpoint_id, uint32_t cluster_id,
             uint32_t attribute_id, int64_t value);

    // Create an entry for a key if absent, leaving it invalid (no value yet).
    void seed(uint64_t node_id, uint16_t endpoint_id, uint32_t cluster_id,
              uint32_t attribute_id);

    // Copy of all entries.
    std::vector<ValueCacheEntry> snapshot() const;

    // Drop all entries.
    void clear();

private:
    ValueCache();
    ValueCache(const ValueCache &) = delete;
    ValueCache &operator=(const ValueCache &) = delete;

    // Locate the entry for a key, optionally claiming a slot if absent. Returns
    // nullptr if absent and not creating, or if the cache is full. Caller holds m_mutex.
    ValueCacheEntry *find_or_claim(uint64_t node_id, uint16_t endpoint_id,
                                   uint32_t cluster_id, uint32_t attribute_id, bool create);

    static constexpr size_t kMaxEntries = 96; // 32 endpoints x 3 ElectricalPowerMeasurement attrs

    mutable std::mutex           m_mutex;
    std::vector<ValueCacheEntry> m_entries; // holds only real entries; capped at kMaxEntries
};
