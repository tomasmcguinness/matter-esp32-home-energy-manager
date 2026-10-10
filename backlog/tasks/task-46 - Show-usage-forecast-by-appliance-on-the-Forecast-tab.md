---
id: TASK-46
title: Show usage forecast by appliance on the Forecast tab
status: In Progress
assignee: []
created_date: '2026-10-10 13:35'
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
- [ ] #4 MSW mock for /api/forecast/usage exists
- [ ] #5 Web app passes tsc and vite build with no new lint errors, and the compiled app is rebuilt
- [ ] #6 Firmware builds cleanly with idf.py and the max_uri_handlers count comment is updated
- [ ] #7 On the device, / still loads, the endpoint returns today's appliances, and vtn.demand_w matches the forecast report received by the VTN
<!-- AC:END -->
