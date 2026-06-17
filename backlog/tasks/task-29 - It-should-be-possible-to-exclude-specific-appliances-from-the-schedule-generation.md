---
id: TASK-29
title: >-
  It should be possible to exclude specific appliances from the schedule
  generation
status: Done
assignee: []
created_date: '2026-06-17 09:12'
updated_date: '2026-06-17 15:43'
labels: []
dependencies: []
---

## Description

<!-- SECTION:DESCRIPTION:BEGIN -->
Appliances, like the hob and oven, can't be run at any time like the dishwasher. It should be possible to exclude them from the scheduling process.
<!-- SECTION:DESCRIPTION:END -->

## Implementation Notes

<!-- SECTION:NOTES:BEGIN -->
Added a per-appliance "Exclude from scheduling" toggle for non-deferrable appliances (hob, oven).

Backend:
- appliance_profile.{c,h}: new appliance_is_excluded(graph_id) helper. Parses the topology graph (node_manager_get_all_json) and returns true iff the node's settings.excludeFromScheduling is JSON true; defaults false on any missing node/field or parse error.
- scheduler.c: scheduler_compute() skips excluded appliances in the enumeration loop (continue without count++), so they never enter the results array — absent from /api/schedule JSON, the Schedule table, and the chart (the "hide entirely" UX).

Frontend (Home.tsx):
- SimpleDevice gains optional excludeFromScheduling; loaded from node settings.
- EditApplianceModal: checkbox after the name field, seeded from existing settings, included in the settings object on PUT /api/nodes/<id> (backend replaces whole settings object) and passed back via onSave.
- mocks/handlers.ts: seeded the Oven node with excludeFromScheduling: true for dev.

Storage reuses the existing node settings blob — no new HTTP endpoint. Frontend typechecks clean (tsc --noEmit). Firmware not compiled here (no ESP-IDF build run).
<!-- SECTION:NOTES:END -->
