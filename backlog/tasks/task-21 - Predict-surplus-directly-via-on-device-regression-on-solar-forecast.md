---
id: TASK-21
title: Predict surplus directly via on-device regression on solar forecast
status: Done
assignee: []
created_date: '2026-06-08 05:42'
updated_date: '2026-06-08 06:02'
labels:
  - forecasting
  - ml
dependencies: []
---

## Description

<!-- SECTION:DESCRIPTION:BEGIN -->
Replace the indirect surplus computation (solar_forecast − consumption_forecast) with an on-device linear regression that maps the issued solar forecast directly to actual surplus, conditioned on hour-of-day and day-of-week.

Model: surplus_pred[h] = a[h] * solar_forecast[h] + b[h][dow]
- a[h]: 24 per-hour OLS slopes pooled over all training days
- b[h][dow]: 24x7 intercepts (mean residual) capturing baseload per hour/day-type
- Label = negated net grid power (−grid_hourly_power)
- Cold-start fallback to existing solar−consumption path

New module surplus_model.{c,h}; surplus_forecast_compute rewritten to use the model with fallback; training wired into solar_forecast_run_daily_job. On-disk surplus file format unchanged so web UI is untouched.

Plan: /home/tomasmcguinness/.claude/plans/change-of-approach-i-distributed-gem.md
<!-- SECTION:DESCRIPTION:END -->

## Acceptance Criteria
<!-- AC:BEGIN -->
- [x] #1 New surplus_model.c/.h trains per-hour slope + per-(hour,dow) intercept from paired solar-forecast-* and grid-hourly-* files and persists /littlefs/surplus-model
- [x] #2 surplus_forecast_compute uses the model when trained and falls back to solar−consumption during cold start, keeping the surplus-YYYY-MM-DD file format unchanged
- [x] #3 Training is invoked from solar_forecast_run_daily_job and consumption_forecast failure is non-fatal
- [x] #4 surplus_model.c registered in CMakeLists and firmware builds
- [x] #5 CLAUDE.md forecasting/ML sections updated to reflect direct surplus prediction
<!-- AC:END -->

## Implementation Notes

<!-- SECTION:NOTES:BEGIN -->
Refinement after review: removed the hard 14-day fallback cutoff. The regression is now used as soon as it has SURPLUS_MODEL_MIN_DAYS (4) of history — the point at which it can fit a solar slope. Below that it still falls back to solar-consumption (which also uses the solar forecast), not nothing. 14 days is repurposed as SURPLUS_MODEL_MATURE_DAYS, a pure UI maturity label. Added surplus_model_status(); surplus JSON now carries usable_days/mature_days/learning, and Forecast.tsx shows a 'Model still learning - N of 14 days' banner above the surplus chart until mature.
<!-- SECTION:NOTES:END -->

## Final Summary

<!-- SECTION:FINAL_SUMMARY:BEGIN -->
Implemented direct surplus prediction via an on-device linear regression on the solar forecast.

New module `firmware/main/surplus_model.{c,h}`:
- `surplus_model_train(window_days)` walks back the window (default 56 days), pairs each day that has BOTH `solar-forecast-*` and `grid-hourly-*` files, and fits `surplus[h] = a[h]*solar_forecast[h] + b[h][dow]`. Slope `a[h]` is a per-hour OLS fit pooled over all days (with a variance guard so night/zero-solar hours degrade to slope 0); intercept `b[h][dow]` is the mean residual per (hour, day-of-week), falling back to the hour-global intercept for empty buckets. Label = negated net grid power. Persists `surplus_model_t` to `/littlefs/surplus-model`. Returns usable-day count.
- `surplus_model_predict()` loads the model + that day's solar forecast and produces 24 hourly surplus values; returns false (cold start) if the model is absent or has < 14 usable days.
- `surplus_model_json()` dumps the fitted coefficients for inspection.

`surplus_forecast.c`: `surplus_forecast_compute` now tries the model first and falls back to the previous `solar - consumption` subtraction; on-disk `surplus-YYYY-MM-DD` format is unchanged so the web UI is untouched.

`solar_forecast.c`: nightly job now trains the model after the solar fetch and treats consumption-forecast failure as non-fatal (it's only the fallback).

`web_server.c`: manual `/api/forecast/solar/fetch` mirrors the nightly job (train + consumption + surplus for tomorrow); added `GET /api/forecast/surplus/model` (inspect) and `POST /api/test/surplus-model/train` (force refit).

`CMakeLists.txt`: registered `surplus_model.c`. `CLAUDE.md`: updated Forecasting / ML approach / Build order to record the deliberate switch to direct surplus prediction (supersedes the old "do not predict surplus directly" rule).

Verified: full ESP-IDF build (esp-idf 5.5.4 + esp-matter-tom) links successfully, 7% app partition free.
<!-- SECTION:FINAL_SUMMARY:END -->
