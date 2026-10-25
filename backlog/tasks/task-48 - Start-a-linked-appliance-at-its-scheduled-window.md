---
id: TASK-48
title: Start a linked appliance at its scheduled window
status: To Do
assignee: []
created_date: '2026-10-24 07:52'
labels: []
dependencies: []
priority: low
---

## Description

<!-- SECTION:DESCRIPTION:BEGIN -->
Once an appliance is linked to a Matter appliance device (Dishwasher/Laundry Washer with Operational State), the HEM could send Operational State Start when the scheduled surplus window opens, if the machine is ready (stopped, program selected, remote start allowed). Delay-start only; never pause mid-cycle. HomeConnect requires Remote Start to be enabled on the appliance, otherwise the bridge answers UnableToStartOrResume.
<!-- SECTION:DESCRIPTION:END -->

## Acceptance Criteria
<!-- AC:BEGIN -->
- [ ] #1 The user can opt in per appliance to automatic start at the scheduled window
- [ ] #2 The HEM sends Operational State Start only when the linked device is Stopped and reachable at window start
- [ ] #3 A rejected start (e.g. UnableToStartOrResume) is surfaced in the UI and not retried in a loop
- [ ] #4 The HEM never sends Pause or Stop to a running appliance
<!-- AC:END -->
