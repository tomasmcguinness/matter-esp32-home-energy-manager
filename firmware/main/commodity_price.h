#pragma once

#include <stdbool.h>
#include "esp_err.h"
#include "tariff.h"

// Import price recorded from the Matter Commodity Price cluster (0x0095) of the
// tariff source (the device wired to the consumer unit's `tariff` handle). The
// cluster only reports the price in force now, so each CurrentPrice report and
// PriceChange event is stamped onto the 15-minute slots it covers in
// /sdcard/price-YYYY-MM-DD, building up a history of what was actually charged.
// tariff_load_day() overlays these slots on any Commodity Tariff schedule.

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t commodity_price_init(void);

// Load a recorded day from the SD card. Returns false if there is no file.
bool commodity_price_load_day(const char *date_str, tariff_day_t *out);

#ifdef __cplusplus
}

#include <app/ConcreteAttributePath.h>
#include <app/EventHeader.h>
#include <lib/core/TLVReader.h>

// Feed one Commodity Price attribute report (already filtered to cluster 0x0095).
// Reports from anything other than the tariff source are ignored. Runs on the
// CHIP thread; SD writes are deferred to a timer.
void commodity_price_on_attribute(uint64_t node_id, const chip::app::ConcreteDataAttributePath &path,
                                  chip::TLV::TLVReader *data);

// Feed one Commodity Price event (already filtered to cluster 0x0095).
void commodity_price_on_event(uint64_t node_id, const chip::app::EventHeader &header,
                              chip::TLV::TLVReader *data);
#endif
