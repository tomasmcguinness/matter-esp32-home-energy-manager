---
id: TASK-30
title: Forecast tab should default to the current date.
status: Done
assignee: []
created_date: '2026-06-17 10:57'
updated_date: '2026-06-17 15:53'
labels: []
dependencies: []
---

## Implementation Notes

<!-- SECTION:NOTES:BEGIN -->
Note: the task title says "Forecast tab", but the actual issue (per user) was the Schedule tab defaulting to tomorrow. The Forecast tab already loads today.

Schedule.tsx: replaced the tomorrow() date helper (which seeded the date picker with the day ahead) with a today() helper that returns the local current date, and seeded the date picker state with it. Also switched from toISOString() UTC date to the local calendar date so "current date" is correct near the midnight boundary (device writes forecast/schedule files using local time). Only the initial picker value changed — navigation to other days still works.

Frontend typechecks clean (tsc --noEmit).
<!-- SECTION:NOTES:END -->
