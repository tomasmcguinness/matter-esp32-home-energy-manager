---
id: TASK-51
title: Make the Usage history load quickly by generating it ahead of time
status: In Progress
assignee:
  - '@claude'
created_date: '2026-10-30 14:51'
updated_date: '2026-10-30 15:07'
labels: []
dependencies: []
references:
  - firmware/main/node_power_logger.cpp
  - firmware/main/web_server.c
  - firmware/main/mcp_server.c
  - firmware/html_app/src/Power.tsx
priority: medium
---

## Description

<!-- SECTION:DESCRIPTION:BEGIN -->
The Usage history chart on the Power tab takes about 47 s to load. Measured on the device on 2026-10-30, /api/data/daily-energy took 4.3 s for days=1 (today only), 13.7 s for days=7 and 47 s for days=30, which is roughly 1.5 s for each finished day even though finished days never change. The MCP get_daily_energy tool goes through the same code and is just as slow. While a request runs, it also ties up the web server.

Outcome: the history is generated on a schedule in the background, so opening the Power tab shows it almost immediately and the figures stay correct.
<!-- SECTION:DESCRIPTION:END -->

## Acceptance Criteria
<!-- AC:BEGIN -->
- [ ] #1 /api/data/daily-energy responds in under 1 s for any window from 1 to 60 days once the history has been generated
- [ ] #2 Today's figures in the response are no more than 5 minutes old
- [ ] #3 A finished day appears in the history after the midnight rollup without regenerating the other days
- [ ] #4 Generated history survives a reboot, so the first page load after a restart is fast
- [ ] #5 Editing the topology is reflected in the history without manual action
- [ ] #6 Running recompute_days is reflected in the history without manual action
- [ ] #7 Until the first generation completes, the Power tab shows a building message rather than an error or an empty chart, and other pages stay responsive
- [ ] #8 The response format for existing fields is unchanged, and the MCP get_daily_energy description and header comments are updated
- [ ] #9 Firmware and web UI builds pass, and load times are re-measured on the device
<!-- AC:END -->

## Implementation Plan

<!-- SECTION:PLAN:BEGIN -->
Approved by the user on 2026-10-30.

1. node_power_logger.cpp: keep one pre-rendered JSON row per day in PSRAM, 59 finished days plus today, for a 60-day window.
2. New low-priority worker task: refresh every 5 minutes. Finished rows are only built when missing, today's row is always rebuilt.
3. Persist finished rows to /sdcard/usage-history (versioned text lines, tmp + rename), only when the set changes.
4. Move the midnight rollup from the esp_timer callback to the worker, so it cannot race the refresh and no longer blocks power sampling.
5. Full rebuild when the node list changes; recompute_from invalidates rows from its date. A compute mutex serialises the worker and recompute.
6. node_power_logger_daily_energy_json assembles from rows with no SD access; returns {building:true} until the first generation completes.
7. Power.tsx shows a building message and retries. Update MCP tool descriptions and header comments.
8. Build firmware and web UI. Re-measure on the device once flashed.
<!-- SECTION:PLAN:END -->
