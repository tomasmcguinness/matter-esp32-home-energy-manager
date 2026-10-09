---
id: TASK-42.1
title: Anchor the battery's solar/grid mix to its state of charge
status: In Progress
assignee: []
created_date: '2026-10-09 14:58'
updated_date: '2026-10-09 15:00'
labels: []
dependencies: []
parent_task_id: TASK-42
---

## Description

<!-- SECTION:DESCRIPTION:BEGIN -->
The battery node's solar-vs-grid mix is wrong in two ways, found by replaying the ledger against the HEM's minute data for 5-9 Oct 2026.

1. The answer depends on who asked first. If the previous day's split was never cached (HEM off or reflashed over midnight), the mix is computed from an unknown starting ledger until something else (opening the Power tab) builds the day chain, at which point the figure jumps. Observed: 3% grid, then 24% grid, during a pure solar charge.

2. The ledger integrates power and is never tied to the battery's reported state of charge, so it ends each day with 500-1300 Wh still "held" that the real battery no longer has. The residue of each overnight grid charge therefore carries forward indefinitely; the longer the chain, the larger the phantom grid share.

Outcome: the mix is the same whenever it is asked for, the energy the ledger believes is held follows the battery's reported state of charge, and the inaccessible reserve (below 10% state of charge) is ignored entirely.

Notes for whoever picks this up: days recorded before this change have no state-of-charge history and keep the old behaviour. The per-load Self-powered figures on the Power tab will shift slightly from the first day with state-of-charge data, because battery discharge then uses the corrected mix (this relaxes TASK-42 acceptance criterion #5 for new days).
<!-- SECTION:DESCRIPTION:END -->

## Acceptance Criteria
<!-- AC:BEGIN -->
- [ ] #1 The battery's state of charge is recorded once a minute alongside its power
- [ ] #2 On days with state-of-charge data, the energy the mix is computed over tracks the reported state of charge above the 10% reserve
- [ ] #3 Energy at or below 10% state of charge is ignored: at the reserve the mix is reported as unknown and the bar is hidden, and the next charge starts a fresh mix
- [ ] #4 The reported mix is identical whether or not the Power tab has been opened since boot
- [ ] #5 During a solar-only charge the grid percentage never rises; while discharging it does not change
- [ ] #6 Days without state-of-charge data still produce a split using the previous behaviour
- [x] #7 Firmware and web UI build clean
<!-- AC:END -->

## Implementation Notes

<!-- SECTION:NOTES:BEGIN -->
2026-10-09: Implemented in firmware/main/node_power_logger.cpp; firmware (`idf.py build`, app partition 4% free) and web UI (`npm run build`) build clean. Not flashed or tested on the device, so only AC #7 is checked.

- SoC logging: battery streams also sample Power Source BatPercentRemaining (0x002F/0x000C) and write it each minute to `soc-<graph_id>-<date>` (same record layout as the power files, value in half-percent).
- Anchored ledger: on days with a soc file, `compute_day_split` holds usable SoC above the reserve (`kBatteryReserveHalfPct`, 10%) instead of integrated Wh. Each SoC rise takes the solar/grid ratio of the charge recorded since the previous step; a fall leaves the mix alone; at or below the reserve the ledger resets to unknown. The cached ledger gained a `soc` flag; a Wh ledger from an older day is re-based onto the first SoC reading keeping its mix. Days without a soc file use the old power-integrated path.
- Deterministic start: `ledger_before` now splits and caches a missing previous day itself, chaining back up to 7 days, instead of returning unknown.
- `/api/battery/source` no longer returns `energy_wh`.

Offline check: the rule replayed in Python over the device's 5-9 Oct power data with a synthetic SoC (real SoC was never logged) converges to the same mix (6.0-6.4% grid) from four different starting ledgers, including the 85%-grid phantom one. The reserve is a firmware constant, not read from the inverter.
<!-- SECTION:NOTES:END -->
