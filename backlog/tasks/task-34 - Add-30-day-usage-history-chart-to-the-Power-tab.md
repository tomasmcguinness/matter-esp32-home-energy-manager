---
id: TASK-34
title: Add 30-day usage history chart to the Power tab
status: In Progress
assignee: []
created_date: '2026-09-26 09:16'
updated_date: '2026-09-26 09:18'
labels: []
dependencies: []
references:
  - firmware/html_app/src/Power.tsx
  - firmware/main/node_power_logger.cpp
  - firmware/main/web_server.c
---

## Description

<!-- SECTION:DESCRIPTION:BEGIN -->
Stacked bar chart of daily total consumption (kWh, grid + solar) over the past 30 days, split by appliance with an Unmonitored remainder. Legend table below with swatch, kWh and % per device, a Total row, and a Total cost row (N/A for now). Hovering a row highlights the matching segments, and vice versa. Backed by a new /api/data/daily-energy endpoint.
<!-- SECTION:DESCRIPTION:END -->

## Acceptance Criteria
<!-- AC:BEGIN -->
- [ ] #1 /api/data/daily-energy returns per-stream daily kWh for the last N days with roles
- [ ] #2 Power tab shows 30 stacked bars of daily total consumption coloured per appliance, plus Unmonitored
- [ ] #3 Legend lists each device with colour square, kWh and percentage, plus Total and Total cost (N/A) rows
- [ ] #4 Hovering a legend row highlights its chart segments and hovering a segment highlights its row
<!-- AC:END -->

## Implementation Notes

<!-- SECTION:NOTES:BEGIN -->
Implemented: node_power_logger_daily_energy_json() (hourly rollup preferred, minute-file fallback for today), GET /api/data/daily-energy?days=N (1..60), msw mock, and a UsageHistory component at the top of the Power tab. Web build and firmware build (IDF 5.5.4, 6% app partition free) both pass. Still to do: visual check in dev-mock and on the device (curl the endpoint after generate-sample-data + rollup-hourly).
<!-- SECTION:NOTES:END -->
