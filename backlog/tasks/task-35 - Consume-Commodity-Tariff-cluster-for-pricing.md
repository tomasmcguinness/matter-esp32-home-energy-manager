---
id: TASK-35
title: Consume Commodity Tariff cluster for pricing
status: In Progress
assignee: []
created_date: '2026-09-26 18:12'
updated_date: '2026-09-26 18:22'
labels: []
dependencies: []
---

## Description

<!-- SECTION:DESCRIPTION:BEGIN -->
Subscribe to the Commodity Tariff cluster (0x0700) on a device assigned via a topology tariff node. Resolve today's and tomorrow's 15-minute prices and persist them per day. Use them to cost the 30-day usage history (per-device cost column plus Total cost) and to show a price chart on the Forecast tab.
<!-- SECTION:DESCRIPTION:END -->

## Acceptance Criteria
<!-- AC:BEGIN -->
- [ ] #1 A tariff device (type 0x0513) can be assigned from Home and appears as a tariff node on the consumer unit
- [ ] #2 HEM subscribes to 0x0700 on the tariff node and writes /sdcard/tariff-YYYY-MM-DD with 96 x 15-min prices for current and next day
- [ ] #3 GET /api/tariff?date= returns currency, decimals, unit and 96 slot prices
- [ ] #4 Usage history shows per-device cost and Total cost, with per-device costs summing to the total
- [ ] #5 Forecast tab shows a step chart of today's and tomorrow's prices
- [ ] #6 The tariff node is not treated as a power stream
<!-- AC:END -->

## Implementation Notes

<!-- SECTION:NOTES:BEGIN -->
Implemented, not yet tested on hardware: new tariff.{h,cpp} (decodes 0x0700, resolves CurrentDay/NextDay to 96x15-min prices, writes /sdcard/tariff-YYYY-MM-DD after a 2 s debounce); the tariff node's subscription gets one extra cluster-wide 0x0700 path; PUT /api/topology/tariff and GET /api/tariff; graph edits re-derive the tariff source and subscribe to a new one; per-day cost with import-only pricing allocated by consumption share, cached in /sdcard/cost-YYYY-MM-DD for finished days; UI: Tariff slot on Home, TariffNode with dashed edge on Topology, Cost column + Total cost on Power, price step chart on Forecast; msw mocks. npm run build and idf.py build pass (5% app partition free). Still to do: end-to-end test against examples/energy-gateway-app.
<!-- SECTION:NOTES:END -->
