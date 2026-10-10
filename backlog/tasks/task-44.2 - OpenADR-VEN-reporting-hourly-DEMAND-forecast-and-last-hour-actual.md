---
id: TASK-44.2
title: 'OpenADR VEN reporting: hourly DEMAND forecast and last-hour actual'
status: To Do
assignee: []
created_date: '2026-10-10 07:45'
labels:
  - openadr
  - firmware
dependencies: []
documentation:
  - docs/openadr-ven-flow.md
parent_task_id: TASK-44
priority: high
---

## Description

<!-- SECTION:DESCRIPTION:BEGIN -->
Once the VEN (see the VEN core subtask of TASK-44) is RUNNING and SNTP has synced, it reports to the VTN following docs/openadr-ven-flow.md.

Every hour, a few seconds after :00, it POSTs a report for today's local day. The intervalPeriod start is local midnight expressed in UTC, with one PT1H interval per hour slot: 23, 24 or 25 on DST days (clocks go back on 2026-10-25). resourceName is "AGGREGATED_REPORT", payload DEMAND / FORECAST / KW.

forecast_source "net": demand_kw = -surplus_w / 1000 from the stored surplus forecast (positive is import, negative is export). If today's surplus forecast is missing, fall back to the consumption forecast and say so in status. forecast_source "gross": the stored consumption forecast for today.

It also POSTs the actual for the last completed hour as DEMAND / DIRECT_READ / KW, from the grid-hourly data.

The forecasting logic itself must not be changed.
<!-- SECTION:DESCRIPTION:END -->

## Acceptance Criteria
<!-- AC:BEGIN -->
- [ ] #1 A forecast report for today's local day is posted every hour shortly after :00, and not before time sync
- [ ] #2 The report has one PT1H interval per local hour slot and starts at local midnight in UTC
- [ ] #3 A host-side unit test or a /api/test endpoint shows correct slot counts and UTC start times for 2026-03-29 (23), 2026-10-25 (25) and a normal day (24)
- [ ] #4 Net source reports demand_kw = -surplus_w / 1000
- [ ] #5 Net source falls back to the consumption forecast when today's surplus forecast is missing, and status reports the source actually used
- [ ] #6 Gross source reports the consumption forecast
- [ ] #7 The last completed hour's actual is posted as DEMAND / DIRECT_READ / KW from grid-hourly data
- [ ] #8 Forecast computation code is unchanged
<!-- AC:END -->
