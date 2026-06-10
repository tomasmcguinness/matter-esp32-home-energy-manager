---
id: TASK-26
title: 'Based on the forecast surplus, a schedule of appliance use should be generated'
status: In Progress
assignee:
  - tomas@tomasmcguinness.com
created_date: '2026-06-10 19:54'
updated_date: '2026-06-10 20:01'
labels: []
dependencies: []
---

## Description

<!-- SECTION:DESCRIPTION:BEGIN -->
Looking ahead to the day's forecast surplus and knowing the average usage pattern of each device, a suggested appliance schedule should be generated.
<!-- SECTION:DESCRIPTION:END -->

## Acceptance Criteria
<!-- AC:BEGIN -->
- [x] #1 GET /api/schedule (?date=, default tomorrow) returns each appliance's suggested run window computed by sliding its learned profile across the day's hourly surplus forecast
- [x] #2 Multiple appliances share surplus greedily (largest energy first, remaining surplus depleted between placements) so two appliances don't double-book the same watts
- [x] #3 Appliances with no learned run cycle are returned as not-schedulable; days with no surplus forecast return surplus_available=false with no runs
- [x] #4 New Schedule web page shows the surplus curve with suggested run windows overlaid plus a table of window / energy / surplus-covered %
- [x] #5 Firmware builds (idf.py build) and the frontend typechecks/bundles
<!-- AC:END -->

## Implementation Plan

<!-- SECTION:PLAN:BEGIN -->
1. appliance_profile.{c,h}: add appliance_profile_load() (load+validate binary profile) and public appliance_enumerate() (graph_id + display name from node settings), single-sourced via existing enumerate_appliances.
2. New scheduler.{c,h}: scheduler_compute() slides each appliance's flat avg-power block (ceil(len_min/60) hours) across the hourly surplus curve, greedy shared-surplus (largest energy first, subtract placed run from remaining). scheduler_json() for the API. No constraints (v1). On-demand, no persistence.
3. web_server.c: GET /api/schedule (?date=, default tomorrow like /api/forecast/surplus). CMakeLists.txt: add scheduler.c. Bump max_uri_handlers.
4. Frontend: new Schedule.tsx page + nav/route in App.tsx. SVG surplus curve (reuse Forecast.tsx pattern) with run windows overlaid + table.
<!-- SECTION:PLAN:END -->

## Implementation Notes

<!-- SECTION:NOTES:BEGIN -->
Implemented v1 optimiser (no per-appliance time constraints, hourly resolution, flat avg-power block = ceil(len_min/60) hours).

- appliance_profile.{c,h}: added appliance_profile_load() and public appliance_enumerate() (graph_id + display name from node settings.name/label), single-sourced via enumerate_appliances (callback now passes name).
- scheduler.{c,h} (new): scheduler_compute() reads /littlefs/surplus-<date> (24 power_record_t), clamps to positive surplus, greedy places largest-energy appliances first, subtracting each run from remaining surplus; best_s=-1 left unscheduled when no surplus to soak. scheduler_json() for the API.
- web_server.c: GET /api/schedule handler (default tomorrow, mirrors /api/forecast/surplus); registered; max_uri_handlers 44->46. CMakeLists.txt: added scheduler.c.
- Frontend: Schedule.tsx (SVG surplus curve + translucent run-window bands + table), wired into App.tsx (nav + route).

Verified: idf.py build succeeds (bin 0x227240, 7% free); npx tsc -b clean. End-to-end runtime verification on device pending (seed via POST /api/test/run-daily-job, then GET /api/schedule and open the Schedule page).
<!-- SECTION:NOTES:END -->
