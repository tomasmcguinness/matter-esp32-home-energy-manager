---
id: TASK-44
title: 'OpenADR 3.1 VEN: send the hourly demand forecast to a VTN'
status: In Progress
assignee: []
created_date: '2026-10-10 07:45'
updated_date: '2026-10-10 09:47'
labels:
  - openadr
dependencies: []
documentation:
  - docs/openadr-ven-flow.md
priority: high
---

## Description

<!-- SECTION:DESCRIPTION:BEGIN -->
The HEMS should act as an OpenADR 3.1 VEN (Matter↔OpenADR 3.x Interworking Spec, scenario IW 2.5.1) and send its demand forecast to a VTN every hour, configured from a new "OpenADR" tab in the web UI.

The contract is the demo VTN's VEN flow document, to be copied into this repo at docs/openadr-ven-flow.md. Its sequence of HTTP calls, MQTT subscriptions and payloads must be followed exactly; conflicts with this task are raised with the user rather than resolved silently.

The demo VTN is plain HTTP and plain MQTT on 1883 (no TLS). Its OAuth client-credentials token endpoint accepts any credentials, but the VEN still calls it and sends the bearer token so the flow matches a real VTN.

Constraints: the forecasting logic itself is not changed by this work. No new components beyond esp-mqtt without asking the user. Work lands in small commits.

This is the parent task; the work is split into subtasks for the VEN core, reporting, the HTTP API and the web UI.
<!-- SECTION:DESCRIPTION:END -->

## Acceptance Criteria
<!-- AC:BEGIN -->
- [ ] #1 Firmware builds cleanly with idf.py
- [ ] #2 Pointed at the VTN, the HEMS registers and the VTN dashboard shows today's forecast
- [ ] #3 A new forecast version arrives at the VTN every hour and on 'Send forecast now'
- [ ] #4 After the VTN is restarted with its state wiped, the HEMS recovers without intervention
- [ ] #5 After Ethernet is unplugged for a minute, the HEMS recovers without intervention
<!-- AC:END -->

## Implementation Plan

<!-- SECTION:PLAN:BEGIN -->
PROPOSED 2026-10-10 — awaiting user approval and docs/openadr-ven-flow.md (not yet in the repo). No code written yet.

Sequencing (one or more small commits each):
1. TASK-44.2 (part): openadr_slots.{c,h} — pure C, no ESP-IDF deps. Given a local date, returns local midnight in UTC and the list of hour-slot start times, by stepping 3600 s from mktime(midnight) to mktime(next midnight) → 23/24/25 slots. Host-side test (plain gcc, TZ=GMT0BST,M3.5.0/1,M10.5.0 as set in main.cpp) covering 2026-03-29, 2026-10-25 and a normal day.
2. TASK-44.1: firmware/main/openadr_ven.{c,h}. One FreeRTOS task owning all VTN I/O, driven by a command queue (restart / send-now / reset / mqtt-nudge / mqtt-connected) with a timeout that doubles as the hourly timer and backoff timer. NVS namespace "openadr" for config + cached IDs. HTTP response and report JSON buffers from heap_caps_malloc(MALLOC_CAP_SPIRAM). esp-mqtt event handler only posts to the queue (idempotent nudge → REST resync). Status + 20-entry activity ring behind a mutex; readers get a snapshot. Add `mqtt` to PRIV_REQUIRES. Exact endpoints, topics and payloads come from docs/openadr-ven-flow.md.
3. TASK-44.2 (rest): build the forecast report from surplus_forecast_json / consumption_forecast_json (matched to slots by hour_ts) and the last-hour actual from grid-hourly; schedule at :00 + a few seconds.
4. TASK-44.3: five handlers in web_server.c. PUT/POST handlers only write NVS and post to the VEN queue, returning immediately. 54 + 5 = 59 handlers, which fits in 64; update the count comment.
5. TASK-44.4: OpenAdr.tsx, nav item + route, MSW handlers. Live status: VEN task pushes {"type":"openadr_status"} over the existing websocket via ws_server_broadcast on state/activity change, with one GET on mount; fall back to 5 s visibility-gated polling if broadcasting from the VEN task turns out not to be safe.

Open questions for the user:
- Stored forecasts hold 24 records per day. On the 25-hour day one slot will have no matching record, and on the 23-hour day one record is unused. Proposal: match by timestamp, and fill an unmatched slot from the record with the same local hour-of-day.
- client_secret is stored in NVS in plaintext (NVS encryption is not enabled).

APPROVED 2026-10-10 with these user decisions after reading the contract (now at docs/openadr-ven-flow.md):
- Forecast window: today's local day (local midnight → local midnight, 23/24/25 PT1H intervals, each with an absolute UTC start). This deliberately differs from the contract's rolling 'upcoming 24 h'.
- State order follows the contract: AUTHENTICATING → REGISTERING_VEN → CONNECTING_MQTT (GET /notifiers, connect with venName as client ID, subscribe ven events+programs topics) → DISCOVERING (program, program events topic, event) → RUNNING.
- Last-hour actual: grid-hourly-* only exists after the midnight rollup, so the actual is the average of the grid's 1-minute records for the completed hour.
- Program target is taken from the ven's own `targets` (the PROGRAM_NAME:* entry), not hardcoded.
<!-- SECTION:PLAN:END -->
