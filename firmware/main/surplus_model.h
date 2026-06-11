#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define SURPLUS_MODEL_HOURS 24
#define SURPLUS_MODEL_DOWS  7

// Days of history before the model can fit a solar slope (below this it would
// ignore the forecast, so the caller keeps using the subtraction fallback).
#define SURPLUS_MODEL_MIN_DAYS    4
// Days of history at which the model is considered "mature" — purely a UI
// maturity label, it does not change behaviour.
#define SURPLUS_MODEL_MATURE_DAYS 14

// On-disk model: a per-hour slope (shared across weekdays) plus a per-(hour,
// day-of-week) intercept. Predicts surplus directly from the issued solar
// forecast:
//
//     surplus_mw[h] = a[h] * solar_forecast_mw[h] + b[h][dow]
//
// a[h] scales the forecast watts to the actual surplus contribution; b[h][dow]
// captures the typical baseload floor (negative surplus) for that hour on that
// day type. Trained on-device from paired solar-forecast-* and grid-hourly-*
// history, where the label is negated net grid power (-grid = export = surplus).
typedef struct {
    uint32_t magic;          // SURPLUS_MODEL_MAGIC
    uint16_t version;        // SURPLUS_MODEL_VERSION
    uint16_t usable_days;    // training days that had both inputs
    uint32_t trained_unix;   // when the model was fitted
    float    a[SURPLUS_MODEL_HOURS];                        // per-hour slope
    float    b[SURPLUS_MODEL_HOURS][SURPLUS_MODEL_DOWS];    // per-(hour,dow) intercept (mW)
    uint16_t n[SURPLUS_MODEL_HOURS][SURPLUS_MODEL_DOWS];    // samples per bucket
} surplus_model_t;

// Train from up to window_days of history ending yesterday and persist the model
// to /sdcard/surplus-model. Returns the number of usable training days (days
// that had both a solar forecast and grid-hourly actuals). A return < the
// cold-start threshold means callers should keep using the fallback.
int surplus_model_train(int window_days);

// Predict the hourly surplus for date_str using the persisted model and that
// day's solar-forecast file. Fills out_mw[24] and the matching hour-start
// timestamps out_ts[24]. Returns false (so the caller can fall back) if the
// model is absent, has fewer than SURPLUS_MODEL_MIN_DAYS of history, or the
// solar forecast is missing.
bool surplus_model_predict(const char *date_str,
                           int32_t out_mw[SURPLUS_MODEL_HOURS],
                           uint32_t out_ts[SURPLUS_MODEL_HOURS]);

// Report the persisted model's maturity for UI messaging. Returns false if no
// model exists yet. *usable_days = days of history behind the fit; *mature =
// the fit has reached SURPLUS_MODEL_MATURE_DAYS of history. Either pointer may
// be NULL.
bool surplus_model_status(uint16_t *usable_days, bool *mature);

// Read back the persisted model as JSON for inspection. Caller must free.
// Shape: {"trained":N,"usable_days":N,"hours":[{"hour":H,"slope":F,
//          "intercept":[{"dow":D,"value":F,"n":N},...]},...]}
char *surplus_model_json(void);

#ifdef __cplusplus
}
#endif
