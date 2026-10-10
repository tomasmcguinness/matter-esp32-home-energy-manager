---
id: TASK-45
title: Show weekly usage pattern on the Appliances tab
status: In Progress
assignee: []
created_date: '2026-10-10 13:16'
updated_date: '2026-10-10 13:22'
labels:
  - firmware
  - web-ui
dependencies: []
references:
  - firmware/main/appliance_profile.c
  - firmware/main/appliance_profile.h
  - firmware/html_app/src/Appliances.tsx
  - firmware/html_app/src/mocks/handlers.ts
---

## Description

<!-- SECTION:DESCRIPTION:BEGIN -->
Each appliance card on the Appliances tab should show when the appliance is used across the week: for each weekday (Mon–Sun), the average number of runs per day and the average energy used per day, over the profile's 30-day window.

Agreed with the user:
- Layout: heat strip of seven tiles shaded by how often the appliance runs, to the right of the power graph and below the stat row.
- Run count: runs on that weekday ÷ number of that weekday with data.
- Power figure: energy of those runs ÷ number of that weekday with data, in kWh.

The figures come from the existing nightly appliance profile training (firmware/main/appliance_profile.c) and are served in the existing /api/appliance/profiles JSON; no new routes.
<!-- SECTION:DESCRIPTION:END -->

## Acceptance Criteria
<!-- AC:BEGIN -->
- [ ] #1 Appliance profile training records, per weekday, the days with data, the runs and the run energy, and /api/appliance/profiles returns a Monday-first weekdays array with days, runs_per_day and energy_wh_per_day
- [ ] #2 Each trained appliance card shows a seven-tile Mon–Sun heat strip to the right of the power graph, below the stat row, with runs per day and kWh per day; weekdays with no data show a dash
- [ ] #3 The panel wraps below the graph on a narrow screen and untrained appliance cards are unchanged
- [x] #4 MSW mock for /api/appliance/profiles includes weekdays data
- [ ] #5 Web app passes tsc, eslint and vite build, and the compiled app is rebuilt
- [x] #6 Firmware builds cleanly with idf.py
- [ ] #7 On the device, after Re-analyse now, the weekday runs add up to program_count and the Schedule tab still lists appliances
<!-- AC:END -->

## Implementation Notes

<!-- SECTION:NOTES:BEGIN -->
Implemented and committed on open-adr-support: profile struct v2 with dow_days/dow_runs/dow_energy_mwh (indexed by tm_wday), weekdays array (Monday first) in the profile JSON, WeeklyPattern heat strip in Appliances.tsx, mock data, rebuilt app.js. idf.py build passes on ESP-IDF 5.5.5; tsc and vite build pass; eslint reports the same pre-existing errors as before the change. Not verified: the page has not been viewed in a browser (AC #2, #3) and nothing has been flashed (AC #1, #7). The profile version bump means existing /sdcard/profile-* files are ignored until the nightly train or Re-analyse now; until then cards show Learning and the scheduler skips those appliances.
<!-- SECTION:NOTES:END -->
