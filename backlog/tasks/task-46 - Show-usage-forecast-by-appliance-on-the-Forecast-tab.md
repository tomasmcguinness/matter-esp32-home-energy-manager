---
id: TASK-46
title: Show usage forecast by appliance on the Forecast tab
status: In Progress
assignee: []
created_date: '2026-10-10 13:35'
updated_date: '2026-10-10 13:38'
labels:
  - firmware
  - web-ui
dependencies: []
references:
  - firmware/main/node_power_logger.cpp
  - firmware/main/openadr_ven.c
  - firmware/main/web_server.c
  - firmware/html_app/src/Forecast.tsx
  - firmware/html_app/src/mocks/handlers.ts
---

## Description

<!-- SECTION:DESCRIPTION:BEGIN -->
Add a "Usage Forecast" section to the Forecast tab, between Surplus Forecast and Tariff. It shows today's expected usage per hour as stacked bars, one colour per appliance plus a grey "Other" for unmonitored load, with the demand forecast that OpenADR sends to the VTN drawn over the bars as a line.

Agreed with the user:
- The bars are expected usage; the VTN value is an overlaid line (whole-house net grid figure, can go below the bars or below zero). What OpenADR sends does not change.
- Each appliance's expected usage for an hour is the average of its logged power for that hour on the same weekday over the last 4 weeks, falling back to the most recent days with data.
<!-- SECTION:DESCRIPTION:END -->

## Acceptance Criteria
<!-- AC:BEGIN -->
- [ ] #1 GET /api/forecast/usage returns, per local hour, each appliance's expected power from same-weekday history (recent-days fallback) and the unmonitored remainder
- [ ] #2 The response includes the demand forecast OpenADR sends (same function the VEN uses), with the source used and whether OpenADR is enabled
- [ ] #3 The Forecast tab shows a Usage Forecast section between Surplus Forecast and Tariff with stacked bars coloured per appliance, a legend with daily kWh, and the VTN forecast as a line
- [x] #4 MSW mock for /api/forecast/usage exists
- [x] #5 Web app passes tsc and vite build with no new lint errors, and the compiled app is rebuilt
- [x] #6 Firmware builds cleanly with idf.py and the max_uri_handlers count comment is updated
- [ ] #7 On the device, / still loads, the endpoint returns today's appliances, and vtn.demand_w matches the forecast report received by the VTN
<!-- AC:END -->

## Implementation Notes

<!-- SECTION:NOTES:BEGIN -->
Implemented and committed on open-adr-support. Firmware: node_power_logger_usage_forecast_json() (on-demand, same-weekday average of nodeh-*/grid-hourly-* over 4 weeks, recent-days fallback, Other = max(grid + solar + battery supplied - appliances, 0)); openadr_ven_forecast_demand_w() shares forecast_demand_kw() with send_forecast(); GET /api/forecast/usage is route 60 of 64. UI: UsageSection/UsageChart in Forecast.tsx between Surplus Forecast and Tariff; mock added; app.js rebuilt. idf.py build passes on ESP-IDF 5.5.5; tsc and vite build pass; lint on the two changed files shows only the error that was already in Forecast.tsx. Not verified: the section has not been viewed in a browser (AC #3) and nothing has been flashed, so the endpoint's output on real SD-card history and the match against the VTN are untested (AC #1, #2, #7). The hem MCP server was unreachable, so the design was not checked against the files on the card.

Observation, not changed: the OpenADR 'gross' source (consumption_forecast_json) is built from grid-hourly-*, so it is a net-at-the-grid figure averaged by weekday, not gross household consumption.
<!-- SECTION:NOTES:END -->
