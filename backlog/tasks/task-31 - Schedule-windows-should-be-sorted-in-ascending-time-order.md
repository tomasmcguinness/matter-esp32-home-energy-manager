---
id: TASK-31
title: Schedule windows should be sorted in ascending time order
status: Done
assignee: []
created_date: '2026-06-17 10:58'
updated_date: '2026-06-17 14:58'
labels: []
dependencies: []
---

## Implementation Notes

<!-- SECTION:NOTES:BEGIN -->
Schedule runs were returned in appliance-enumeration order. Added a trailing sort in scheduler_compute() (firmware/main/scheduler.c) that orders the returned runs by start_hour ascending, treating start_hour < 0 (unplaced / non-schedulable) as SCHED_HOURS so window-less rows stay at the bottom in enumeration order. Both the /api/schedule JSON and the Schedule.tsx table/chart consume the array in this order, so no consumer changes were needed.
<!-- SECTION:NOTES:END -->
