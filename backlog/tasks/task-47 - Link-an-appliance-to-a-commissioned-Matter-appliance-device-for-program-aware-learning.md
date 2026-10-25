---
id: TASK-47
title: >-
  Link an appliance to a commissioned Matter appliance device for program-aware
  learning
status: In Progress
assignee: []
created_date: '2026-10-24 07:52'
updated_date: '2026-10-24 08:03'
labels: []
dependencies: []
references:
  - /home/tomasmcguinness/.claude/plans/for-appliances-i-want-wise-swing.md
priority: medium
---

## Description

<!-- SECTION:DESCRIPTION:BEGIN -->
Today an appliance node is only a power meter (Electrical Power Measurement endpoint), and the HEM learns one profile per appliance that averages all cycles together regardless of program.

Let an appliance node also link a commissioned Matter appliance endpoint (Dishwasher 0x0075 or Laundry Washer 0x0073, e.g. from matter-esp32-appliance-bridge). The meter stays the power source; the linked device adds Operational State, the selected program (Dishwasher/Laundry Washer Mode) and phase. The HEM logs state and program per minute, learns per-program profiles, and the scheduler uses the selected program (today), else the most likely program for the weekday, else the all-cycles profile. Observe only; no commands are sent.
<!-- SECTION:DESCRIPTION:END -->

## Acceptance Criteria
<!-- AC:BEGIN -->
- [x] #1 An appliance can be linked to (and unlinked from) a commissioned Dishwasher or Laundry Washer endpoint from the appliance edit UI; the link persists in the node's settings
- [x] #2 Linked devices are subscribed for Operational State and their Mode cluster even when they have no Electrical Sensor endpoint; unlinked meter-only nodes keep their existing subscription paths
- [x] #3 The Appliances tab shows the linked device's live operational state, program, phase and countdown
- [x] #4 Per-minute state/program history is logged per linked appliance in an append-only per-day file
- [x] #5 Nightly training produces per-program stats (runs, power, length, weekday runs), with cycles bounded by Operational State when available; appliances without a link train as before
- [x] #6 Profiles written by the previous version still load and schedule until retrained
- [x] #7 The schedule reports which program and which selection rule (selected/weekday/all) each window used
- [x] #8 Firmware and web app build cleanly; CLAUDE.md documents the per-program learning and the state log
<!-- AC:END -->

## Implementation Notes

<!-- SECTION:NOTES:BEGIN -->
Implemented, built (idf.py build with esp-idf-v5.5.5 + esp-matter-tom; npm run build) but NOT yet verified on hardware: needs the appliance bridge commissioned, a dishwasher linked, a few real runs, then POST /api/test/appliance-profiles/train.

- appliance_state.{cpp,h}: links from settings.device, live state from Operational State / Dishwasher Mode / Laundry Washer Mode / Bridged Reachable, per-minute state log state-<graphId>-YYYY-MM-DD, program catalogue programs-<graphId> keyed by mode label.
- subscription_manager: linked nodes are wanted; per-link paths (OpState cluster, Mode cluster, Reachable) only on nodes with links. on_topology_changed resubscribes nodes whose links changed.
- appliance_profile: version 3 adds programs[8]; state-bounded cycles on days with a usable state log; v2 profiles still load.
- scheduler: pick_program selected -> weekday -> all (min 3 runs); program + program_source in JSON.
- GET /api/devices/endpoints: comma-separated deviceTypeId, role=device, deviceType per entry.
- UI: LinkedDeviceSelect in Home appliance modal and EditNodeModal; Appliances tab live status + By program table (refetches a profile when its device's attributes change on the websocket); canvas node shows state/program as of page load; Schedule shows the program used.
<!-- SECTION:NOTES:END -->
