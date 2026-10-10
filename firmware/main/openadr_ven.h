#pragma once

#include <stdbool.h>

#include "esp_err.h"
#include "cJSON.h"

#ifdef __cplusplus
extern "C" {
#endif

// OpenADR 3.1 VEN. Registers with a VTN and posts the household demand forecast
// (and the last hour's actual) every hour, following docs/openadr-ven-flow.md.
//
// All VTN traffic runs on the module's own task. The functions below only touch
// NVS and in-memory state and hand work to that task, so they are safe to call
// from HTTP handlers.

// Load the config from NVS and start the VEN task. Call once at boot.
esp_err_t openadr_ven_start(void);

// Config as JSON, with `secret_set` in place of the client secret. Caller frees.
char *openadr_ven_config_json(void);

// Apply a config object (same keys as openadr_ven_config_json, plus
// `client_secret`), save it to NVS and restart the state machine. Keys that are
// absent keep their value, as does an empty `client_secret`.
// Returns ESP_ERR_INVALID_ARG if a value is malformed.
esp_err_t openadr_ven_config_update(const cJSON *body);

// State, IDs, last/next report and the activity log as JSON. Caller frees.
char *openadr_ven_status_json(void);

// The demand forecast the VEN reports for a local day ("YYYY-MM-DD"), in watts
// per local hour (positive is import), from the configured forecast source.
// `source_used` is set to "net", "gross" or "gross_fallback" and `enabled` to
// whether the VEN is switched on. False if no forecast is stored for the date.
bool openadr_ven_forecast_demand_w(const char *date, double w[24], const char **source_used, bool *enabled);

// Post the forecast report as soon as the VEN is running.
void openadr_ven_send_now(void);

// Forget the cached ven/program/event IDs and register again.
void openadr_ven_reset(void);

#ifdef __cplusplus
}
#endif
