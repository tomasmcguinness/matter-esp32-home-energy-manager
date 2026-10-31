---
id: TASK-50
title: Show tabs based on what's configured (simple first-run UI)
status: In Progress
assignee: []
created_date: '2026-10-29 16:04'
updated_date: '2026-10-29 16:11'
labels:
  - web-ui
  - firmware
dependencies: []
---

## Description

<!-- SECTION:DESCRIPTION:BEGIN -->
Make the web UI configuration-driven: an empty install shows only Home, Topology and Settings, and further tabs appear as the user configures what they need (grid clamp + smart plugs/clamps first). Built on main. Also stop the forecast job's work when no solar is wired, and run appliance profiling regardless of the solar fetch.
<!-- SECTION:DESCRIPTION:END -->

## Acceptance Criteria
<!-- AC:BEGIN -->
- [x] #1 Home, Topology, Settings always shown
- [x] #2 Devices shown once at least one Matter device is commissioned
- [x] #3 Power shown once any meter is wired to the consumer unit
- [x] #4 Appliances shown once a load is wired to a CU circuit
- [ ] #5 Forecast and Schedule shown once a solar inverter and a grid meter are wired
- [x] #6 Test shown only when 'Show advanced features' is ticked in Settings (per browser)
- [x] #7 Hidden routes render a notice instead of a blank page; tabs update without a reload after configuration changes
- [x] #8 Power hides cost columns when no tariff is configured
- [ ] #9 Daily job skips the solar fetch / surplus model / catch-up without a solar inverter, and always runs appliance profiling
- [x] #10 Web UI and firmware build
<!-- AC:END -->

## Implementation Notes

<!-- SECTION:NOTES:BEGIN -->
Built on branch `config-driven-tabs` off main (main has no OpenADR page, so that rule was dropped).

- `src/topology.ts`: CU role helpers moved out of Power.tsx (connectedNodes, roleForHandle, nodeLabel, hasTariff).
- `src/setup.ts` + `src/SetupProvider.tsx`: loads /api/nodes + /api/devices on every navigation and on refresh(); derives hasDevices / hasMeters / hasLoads / hasSolarAndGrid / hasTariff; `advanced` lives in localStorage (`hem.advanced`).
- `App.tsx`: one PAGES table drives the nav and routes; hidden routes render a "isn't available yet" card that links to what unlocks them.
- Refresh points: Home save/delete handlers, Devices delete, Topology (debounced 1s on the set of edge ids, since edges are re-set on every power update) and the `device_commissioned` websocket message.
- Settings: "Display" card with "Show advanced features" (Test page).
- Power: Cost column + Total cost row hidden without a tariff.
- Firmware: `node_power_logger_has_role()`; daily job always runs appliance_profile_train_all first, and skips the Open-Meteo fetch / surplus model / forecasts and the boot catch-up when no solar stream is wired.
- Mocks: `?mock=empty` and `?mock=unwired` seeds; mock grid save now adds its edge like the firmware.

Verified in mock mode with Playwright: empty → Home, Topology, Settings; unwired → + Devices; assigning a grid sensor → + Power without a reload; seeded → + Appliances; advanced → + Test; /forecast shows the notice; Power without a tariff has no Cost column. The solar unlock (AC #5) wasn't exercised in a browser: the mock's inverter endpoints are all `included: false`, so the Solar modal lists nothing. Firmware builds (IDF 5.5.5, 44% free). AC #9 needs checking on the device logs.
<!-- SECTION:NOTES:END -->
