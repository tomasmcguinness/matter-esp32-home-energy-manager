#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

// Electricity tariff, sourced from the Matter Commodity Tariff cluster (0x0700)
// of the device wired to the consumer unit's `tariff` handle on the topology
// canvas. The cluster only describes the current and next day, so every day it
// reports is resolved to fixed 15-minute price slots and persisted to
// /sdcard/tariff-YYYY-MM-DD. Those files are the price history that cost
// calculations use; days before a tariff was assigned simply have no file.
//
// Prices are Matter `money`: the value in currency units scaled by
// 10^decimals, per TariffUnit (kWh, or kVAh treated as kWh).

#ifdef __cplusplus
extern "C" {
#endif

#define TARIFF_SLOTS         96        // 15-minute slots per day
#define TARIFF_SLOT_MINUTES  15
#define TARIFF_NO_PRICE      INT64_MIN // slot with no resolvable price

typedef struct {
    uint16_t currency;  // ISO 4217 numeric code, 0 if unknown
    uint8_t  decimals;  // price = value / 10^decimals
    uint8_t  unit;      // 0 = kWh, 1 = kVAh
    int64_t  price[TARIFF_SLOTS];
} tariff_day_t;

esp_err_t tariff_init(void);

// Re-read the tariff source (Matter node + endpoint) from the topology graph.
// Call after the graph's tariff node changes.
void tariff_refresh_source(void);

// The Matter node/endpoint currently assigned as the tariff source. Returns
// false when no tariff node is wired to the consumer unit.
bool tariff_get_source(uint64_t *node_id, uint16_t *endpoint_id);

// Load a resolved day from the SD card. Returns false if there is no file.
bool tariff_load_day(const char *date_str, tariff_day_t *out);

// {"date","currency","decimals","unit","provider","label","slots":[n|null x96]}
// Slots are null when the day has no file. Caller must free.
char *tariff_day_json(const char *date_str);

#ifdef __cplusplus
}

#include <app/ConcreteAttributePath.h>
#include <lib/core/TLVReader.h>

// Feed one Commodity Tariff attribute report (already filtered to cluster
// 0x0700). Reports from anything other than the current tariff source are
// ignored. Runs on the CHIP thread; SD writes are deferred to a timer.
void tariff_on_attribute(uint64_t node_id, const chip::app::ConcreteDataAttributePath &path,
                         chip::TLV::TLVReader *data);
#endif
