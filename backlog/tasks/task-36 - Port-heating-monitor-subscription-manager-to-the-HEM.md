---
id: TASK-36
title: Port heating monitor subscription manager to the HEM
status: In Progress
assignee: []
created_date: '2026-09-27 05:58'
labels: []
dependencies: []
---

## Description

<!-- SECTION:DESCRIPTION:BEGIN -->
Replace the HEM's per-node backoff/ICD retry table in matter_controller.cpp with the subscription_manager from matter-esp32-heating-monitor. It caps attempts in flight at 4, times out a stuck slot after 90 s, paces attempts at 1 s, sweeps for unsubscribed nodes every 60 s, tracks subscribed/pending/subscription-id per node, and resubscribes ICDs on Check-In instead of chasing them.

Implemented in firmware/main/managers/subscription_manager.{h,cpp}. matter_controller.cpp now only handles attribute reports (matter_controller_attribute_data_cb). Firmware builds; not tested on hardware.
<!-- SECTION:DESCRIPTION:END -->

## Acceptance Criteria
<!-- AC:BEGIN -->
- [ ] #1 At most 4 subscription attempts in flight; the worker waits for a free slot
- [ ] #2 Slots are freed on established, failed, terminated or send/schedule failure, and reclaimed after 90 s
- [ ] #3 Non-ICD nodes without a subscription are re-queued by the 60 s sweep; ICDs are resubscribed on Check-In
- [ ] #4 The tariff source's subscription includes the Commodity Tariff path
- [ ] #5 No CHIP_ERROR_NO_MEMORY at OperationalSessionSetup.cpp:254 under normal operation
<!-- AC:END -->
