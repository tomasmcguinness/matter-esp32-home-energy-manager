---
id: TASK-49
title: Hourly usage heatmap on the Power page
status: In Progress
assignee: []
created_date: '2026-10-29 08:52'
updated_date: '2026-10-29 08:55'
labels:
  - web-ui
  - firmware
dependencies: []
---

## Description

<!-- SECTION:DESCRIPTION:BEGIN -->
Add a day × hour heatmap card to the Power page showing each metered stream's hourly energy (Wh / mean) or peak power, selectable per stream, over 7/14/30/60 days. Served by GET /api/data/node?id=<graphId>&days=N (no new URI handler slot).
<!-- SECTION:DESCRIPTION:END -->

## Acceptance Criteria
<!-- AC:BEGIN -->
- [ ] #1 GET /api/data/node?id=X&days=N returns per-day, per-hour Wh and peak W for the stream (grid included), oldest first
- [x] #2 Power page shows a heatmap card with stream chips, day range and Wh/Peak toggle, styled like the existing cards
- [x] #3 Hovering a cell shows day, hour, Wh and peak; future hours today are distinguished from missing data
- [x] #4 Web UI builds
<!-- AC:END -->

## Implementation Notes

<!-- SECTION:NOTES:BEGIN -->
- Firmware: `node_power_logger_heatmap_json()` buckets each day's minute file (grid-* for the grid stream, node-* otherwise) by local hour into mean (= Wh) and largest-magnitude peak; falls back to the hourly rollup (peak null) when the minute file is gone. Served from the existing `/api/data/node` route when `days=N` (1..60) is present, so no extra URI handler slot.
- UI: `HourlyHeatmap` in Power.tsx, below Usage history. Stream buttons, 7/14/30/60d, Wh/mean vs W peak. Solar is sign-flipped so generation reads positive; negative values (grid export, battery discharge) use a green scale. Future hours today are hatched; missing hours are outlined white cells. Right column = daily import/consumption total.
- Mock: handlers.ts serves the days=N shape. Verified via mock dev server screenshots; firmware builds with IDF 5.5.5. AC #1 still needs checking on the device.
- Not included: the unmonitored remainder as a stream (needs the grid+solar+battery−loads minute calculation).
<!-- SECTION:NOTES:END -->
