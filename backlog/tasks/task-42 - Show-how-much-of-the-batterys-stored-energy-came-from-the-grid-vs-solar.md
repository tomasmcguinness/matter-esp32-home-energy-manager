---
id: TASK-42
title: Show how much of the battery's stored energy came from the grid vs solar
status: In Progress
assignee: []
created_date: '2026-10-09 12:52'
updated_date: '2026-10-09 15:00'
labels: []
dependencies: []
---

## Description

<!-- SECTION:DESCRIPTION:BEGIN -->
The home battery charges from two sources: the PV strings, and the grid (when the inverter draws AC power to charge it). When the battery later discharges into the house, that energy should count as self-generated or grid according to where it originally came from, so self-generated vs grid totals stay correct.

The per-load solar/grid split already attributes battery discharge by origin, using a battery ledger (energy held + solar fraction) chained from day to day in the node power logger. What is missing is visibility: the ledger is never exposed, so the UI cannot show what the battery currently holds.

Outcome: the battery node on the topology canvas shows the origin of the stored energy as two percentages (self-generated vs grid) in a horizontal stacked bar, and while the battery is discharging the live discharge power is flagged by source.

Known limitations to keep in mind (not in scope to fix): energy of unknown origin counts as grid; the ledger is not anchored to the reported state of charge; if the HEM was off over midnight the mix is unknown until the day chain is rebuilt.
<!-- SECTION:DESCRIPTION:END -->

## Acceptance Criteria
<!-- AC:BEGIN -->
- [ ] #1 The HEM exposes the battery's current stored-energy mix (solar % and grid %, summing to 100) over its web API
- [ ] #2 The battery node on the topology canvas shows the two percentages as a horizontal stacked bar
- [ ] #3 While the battery is discharging, the battery node shows the discharge power split into solar and grid
- [ ] #4 When the origin of the stored energy is unknown, the bar is hidden rather than showing a misleading value
- [ ] #5 The existing per-load Self-powered figures on the Power tab are unchanged
- [ ] #6 The mock server serves the new data so the bar is visible in dev-mock mode
<!-- AC:END -->

## Implementation Notes

<!-- SECTION:NOTES:BEGIN -->
2026-10-09: Implemented; web UI (`npm run build`) and firmware (`idf.py build`) both build clean. Runtime verification is still pending: not yet viewed in dev-mock or tested on the device, so no acceptance criteria are checked.

Firmware: `node_power_logger_battery_source_json()` in firmware/main/node_power_logger.cpp runs today's `compute_day_split` from `ledger_before(today)` and serialises the closing ledger as `{known, solar_pct, grid_pct, energy_wh}`; served at `GET /api/battery/source` (firmware/main/web_server.c). The ledger maths and `split-*` cache format are untouched, so the Power tab Self-powered figures are unaffected.

UI: `useBatterySource()` + stacked bar in `BatteryNode` (firmware/html_app/src/Topology.tsx), refreshed every 60 s; hidden when `known` is false. While discharging, the label also shows the discharge power split by the same mix. Mock handler added in firmware/html_app/src/mocks/handlers.ts; firmware/html_compiled_app rebuilt.

App partition is now 4% free (0x237f00 of 0x250000).

2026-10-09: On-device the mix read 3% then 24% grid during a solar-only charge. Cause and fix tracked in TASK-42.1 (ledger not anchored to SoC, and the result depended on whether yesterday's split cache existed). That fix means acceptance criterion #5 only holds for days recorded before SoC logging; Self-powered figures shift slightly afterwards. `energy_wh` was removed from `/api/battery/source`.
<!-- SECTION:NOTES:END -->
