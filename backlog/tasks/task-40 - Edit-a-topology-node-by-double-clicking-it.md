---
id: TASK-40
title: Edit a topology node by double-clicking it
status: In Progress
assignee: []
created_date: '2026-10-04 16:26'
updated_date: '2026-10-04 16:28'
labels:
  - ui
  - topology
dependencies: []
---

## Description

<!-- SECTION:DESCRIPTION:BEGIN -->
Double-clicking a node on the Topology canvas should open an editor for it (name, assigned meter device), instead of only the grid meter responding to double-click.
<!-- SECTION:DESCRIPTION:END -->

## Acceptance Criteria
<!-- AC:BEGIN -->
- [ ] #1 Double-clicking a load (device/appliance) node opens a modal to change its name and its electrical meter endpoint
- [ ] #2 Double-clicking a sub consumer unit opens a modal to rename it
- [ ] #3 The device picker lists unassigned endpoints plus the node's current one
- [ ] #4 Saving persists via PUT /api/nodes/<id>/settings without dropping other settings fields, and the canvas updates without a reload
- [ ] #5 The graph node id, position and edges are unchanged by an edit
<!-- AC:END -->

## Implementation Notes

<!-- SECTION:NOTES:BEGIN -->
Implemented in firmware/html_app/src/EditNodeModal.tsx (new) and Topology.tsx; html_compiled_app rebuilt. Frontend only: reuses PUT /api/nodes/<id>/settings, no firmware change. Nodes now carry their raw settings object in data.settings so the wholesale-replace PUT keeps unknown fields. Also reachable from the right-click menu ("Edit"). Type-checks and builds; not yet exercised in a browser or on the device - acceptance criteria left unchecked until then. Out of scope: solar inverter, battery, PV string, tariff and main consumer unit nodes (double-click still does nothing on those).
<!-- SECTION:NOTES:END -->
