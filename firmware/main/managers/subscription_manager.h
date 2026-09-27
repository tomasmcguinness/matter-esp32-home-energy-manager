#pragma once

#include <stdint.h>
#include "esp_err.h"

// Serialises Matter subscriptions through a queue drained by a single worker task. Ported from
// matter-esp32-heating-monitor's subscription_manager.
//
// Every trigger (startup, the periodic sweep, an ICD Check-In, a subscription terminating or
// failing, or a new tariff assignment) just enqueues a node id. The worker decides what to
// subscribe to from what the HEM needs of that node -- ElectricalPowerMeasurement on metered
// nodes, and the Commodity Tariff cluster on the tariff source -- so callers do not have to know.
//
// Pacing alone is not enough. An attempt holds one of the controller's CASE session-setup slots
// (CHIP_CONFIG_CONTROLLER_MAX_ACTIVE_CASE_CLIENTS) until it resolves, and an attempt at an
// unreachable node can take tens of seconds to time out. Starting one a second therefore piles
// them up until the pool is empty, and every other connection then fails with
// CHIP_ERROR_NO_MEMORY at OperationalSessionSetup::EstablishConnection. So the worker also caps how
// many attempts are in flight, and waits for one to finish before starting another.
//
// ICDs (nodes that registered us as their Check-In client at commissioning) are not chased when an
// attempt fails or by the sweep: they get a subscription when they next check in.

#ifdef __cplusplus
extern "C" {
#endif

// Create the queue and worker, register for ICD Check-Ins, and queue every node the HEM needs a
// subscription to. Call once the Matter controller has started.
esp_err_t subscription_manager_start(void);

// Queue a subscription attempt for a node. Marks the node pending, so a node that is already
// queued or in flight is not queued twice.
esp_err_t enqueue_subscription(uint64_t node_id);

#ifdef __cplusplus
}
#endif
