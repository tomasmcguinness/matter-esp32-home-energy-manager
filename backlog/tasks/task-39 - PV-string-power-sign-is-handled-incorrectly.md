---
id: TASK-39
title: PV string power sign is handled incorrectly
status: To Do
assignee: []
created_date: '2026-10-03 08:43'
labels:
  - BUG
dependencies: []
references:
  - firmware/html_app/src/Topology.tsx
  - firmware/main/node_power_logger.cpp
  - backlog TASK-28
---

## Description

<!-- SECTION:DESCRIPTION:BEGIN -->
PV string nodes (children of the solar inverter, wired to its dc_in handle) report negative power while generating, e.g. "-123 W" on the Topology canvas, following the Matter convention (negative = power flowing out of the node). The user believes the way the HEM displays and interprets these readings is wrong. This needs to be reviewed and made consistent with how the rest of the system now handles sign.

Related context:
- Matter spec 9.2.6.1: positive = power flowing into the node, negative = power the node supplies.
- The solar inverter's readings are now negated as they are logged, so solar history is stored from the consumer unit's perspective (+ = supplied to the CU). Live values on the canvas stay raw Matter.
- On the canvas, the PV string -> inverter edge is metered at the PV string (source) end. Edge direction comes from edgeKw in firmware/html_app/src/Topology.tsx.
- PV strings are not currently logged as streams by node_power_logger.
- The mock PV string reading (firmware/html_app/src/mocks/handlers.ts) is +1.8 kW, which does not match the real devices.
<!-- SECTION:DESCRIPTION:END -->

## Acceptance Criteria
<!-- AC:BEGIN -->
- [ ] #1 A generating PV string animates PV string -> inverter on the Topology canvas
- [ ] #2 The PV string node shows its generation with a sign and label that the user agrees is correct
- [ ] #3 Mock PV string readings match the sign real devices report
- [ ] #4 The chosen sign convention for PV strings is documented in code comments
<!-- AC:END -->
