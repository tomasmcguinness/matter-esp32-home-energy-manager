---
id: TASK-44.1
title: 'OpenADR VEN core: config, registration, discovery and MQTT state machine'
status: In Progress
assignee: []
created_date: '2026-10-10 07:45'
updated_date: '2026-10-10 09:57'
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
A new firmware module, openadr_ven, that brings the HEMS up as a VEN against the VTN described in docs/openadr-ven-flow.md (follow that document's exact sequence of HTTP calls, MQTT subscriptions and payloads).

It runs in its own FreeRTOS task and must never block the Matter thread or the HTTP server. Large JSON buffers live in PSRAM. It uses esp_http_client (plain http://), esp-mqtt (plain mqtt://, a client separate from any local metering MQTT client) and cJSON.

Config in NVS namespace "openadr": enabled, vtn_base_url, client_id, client_secret, ven_name (default derived from the Ethernet MAC, e.g. hems-a1b2c3), forecast_source ("net" or "gross"), mqtt_host_override (optional; normally taken from GET /notifiers). Cached in the same namespace: venID, programID, the forecast eventID.

States, exposed via status: DISABLED → WAIT_TIME_SYNC → AUTHENTICATING → REGISTERING_VEN → DISCOVERING (programs/events by target; find the event whose reportDescriptor is DEMAND/FORECAST) → CONNECTING_MQTT → RUNNING, plus BACKOFF on error.

MQTT notifications are only a nudge to resync; REST is the source of truth.
<!-- SECTION:DESCRIPTION:END -->

## Acceptance Criteria
<!-- AC:BEGIN -->
- [ ] #1 With OpenADR disabled the module stays in DISABLED and makes no network calls
- [ ] #2 Nothing is sent to the VTN before SNTP time sync
- [ ] #3 The VEN authenticates, registers, discovers the DEMAND/FORECAST event and connects to MQTT, reaching RUNNING, using the calls and payloads in docs/openadr-ven-flow.md
- [ ] #4 venID, programID and the forecast eventID survive a reboot and are reused
- [ ] #5 On an error the VEN enters BACKOFF with exponential delay capped at 5 minutes and resumes from the step that failed
- [ ] #6 A 401 causes re-authentication
- [ ] #7 A 404 for a cached ID clears the cached IDs and triggers re-discovery
- [ ] #8 On MQTT reconnect the VEN re-subscribes and then resyncs events over REST
- [ ] #9 Duplicate MQTT notifications (QoS 1) cause no duplicate side effects
- [ ] #10 The last 20 activity entries (state changes, reports with HTTP status, MQTT notifications, errors) are retained for the UI
- [ ] #11 The Matter thread and HTTP server are never blocked by VEN network activity
<!-- AC:END -->

## Implementation Notes

<!-- SECTION:NOTES:BEGIN -->
Implemented in firmware/main/openadr_ven.{c,h}; started from main.cpp after the web server. Builds cleanly. No acceptance criterion has been exercised on hardware or against a VTN yet, so none are checked. State order follows the contract (CONNECTING_MQTT before DISCOVERING), by user decision.
<!-- SECTION:NOTES:END -->
