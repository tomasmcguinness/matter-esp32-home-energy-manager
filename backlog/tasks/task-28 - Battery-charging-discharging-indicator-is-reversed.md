---
id: TASK-28
title: Battery charging/discharging indicator is reversed
status: To Do
assignee: []
created_date: '2026-06-17 09:10'
updated_date: '2026-10-03 10:24'
labels:
  - BUG
dependencies: []
---

## Description

<!-- SECTION:DESCRIPTION:BEGIN -->
When the battery is drawing power, it shows a "discharging" indicator, which is incorrect.
<!-- SECTION:DESCRIPTION:END -->

## Implementation Notes

<!-- SECTION:NOTES:BEGIN -->
2026-10-03 context: the battery reports per Matter spec 9.2.6.1 (+ = charging, power drawn from the mains; - = discharging). The canvas BatteryNode label was changed to treat negative as discharging, but the user believes battery handling is still wrong overall, so this needs re-checking against real readings.

Current state of the code:
- The battery minute files are stored as raw Matter values.
- compute_day_split (firmware/main/node_power_logger.cpp) negates the battery stream so its maths sees power supplied by the battery.
- The canvas inverter->battery edge is metered at the battery (target) end.

In the same session the solar inverter was changed to be flipped at logging time (on_sample_timer, stream_t.is_solar), so solar files are stored from the consumer unit's perspective (+ supplied to the CU). Decide whether the battery should follow the same 'flip at ingestion' pattern; if it does, remove the negation in compute_day_split.

2026-10-03 update: decided that every reading is stored with the raw Matter sign (no flip on ingestion; the solar flip was reverted). Sign is only interpreted when history is processed: supplied_mw() in node_power_logger.cpp turns inverter and battery readings into power supplied, and grid and load readings are used as stored (+ = import / consumption). The battery now goes through supplied_mw() in compute_day_split. Remaining work is to check the battery against real readings.
<!-- SECTION:NOTES:END -->
