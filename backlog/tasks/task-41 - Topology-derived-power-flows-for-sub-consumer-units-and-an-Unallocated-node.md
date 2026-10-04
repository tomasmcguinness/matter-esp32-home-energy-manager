---
id: TASK-41
title: 'Topology: derived power flows for sub consumer units and an Unallocated node'
status: In Progress
assignee: []
created_date: '2026-10-04 16:46'
labels:
  - ui
  - topology
dependencies: []
---

## Description

<!-- SECTION:DESCRIPTION:BEGIN -->
Canvas-side derived flows in Topology.tsx (withSubCuFeeds, withUnallocated). Browser only: the Unallocated node is persisted (position) but its feed edge is synthesised on the canvas and never saved, so firmware and the Power tab do not see it as a consumer-unit connection. Built and arithmetic checked with sample numbers; not yet exercised in a browser or on the device. Follow-up: move the calculation into firmware when the baseload series needs it, sharing the definition with node_power_logger's __unmonitored remainder; consider smoothing the live value.
<!-- SECTION:DESCRIPTION:END -->

## Acceptance Criteria
<!-- AC:BEGIN -->
- [ ] #1 Sub-CU feed edge equals the sum of its load edges and points into the sub-CU
- [ ] #2 Grid meter -> main CU edge shows meter reading minus sub-CU feeds
- [ ] #3 Unallocated node shows CU inputs minus metered circuits, clamped at zero
- [ ] #4 Unallocated node shows a metering warning when the remainder stays below -50 W for 30 s
- [ ] #5 Unallocated node cannot be deleted or rewired and keeps its dragged position across reloads
<!-- AC:END -->
