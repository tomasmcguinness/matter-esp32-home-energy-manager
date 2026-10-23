---
id: TASK-44.6
title: 'OpenADR VEN: run without MQTT when the VTN offers no broker'
status: In Progress
assignee: []
created_date: '2026-10-17 14:23'
updated_date: '2026-10-17 14:23'
labels:
  - openadr
  - firmware
dependencies: []
references:
  - firmware/main/openadr_ven.c
parent_task_id: TASK-44
---

## Description

<!-- SECTION:DESCRIPTION:BEGIN -->
If GET /notifiers lists no MQTT broker (and no mqtt_host_override is set), the VEN used to treat it as an error and loop through BACKOFF forever, never reaching RUNNING or sending a report. The user only needs to push forecasts, so an empty /notifiers now means "no updates will be available": skip the MQTT connect and subscriptions and go straight to DISCOVERING. No polling replaces the notifications. Deviates from docs/openadr-ven-flow.md steps 3-5 by user decision (2026-10-17).
<!-- SECTION:DESCRIPTION:END -->

## Acceptance Criteria
<!-- AC:BEGIN -->
- [ ] #1 With /notifiers offering no MQTT broker, the VEN reaches RUNNING and sends hourly forecast and actual reports
- [ ] #2 An info activity entry records that the VTN offers no broker
- [ ] #3 No program-topic subscription is attempted without an MQTT client
- [ ] #4 A VTN that offers a broker still goes through the MQTT flow unchanged
<!-- AC:END -->

## Implementation Notes

<!-- SECTION:NOTES:BEGIN -->
2026-10-17: step_connect_mqtt() now logs "VTN offers no MQTT broker, continuing without notifications" and moves to DISCOVERING when /notifiers has no mqtt.uris[0]; step_discover() only subscribes to the program events topic when an MQTT client exists. A non-200 from /notifiers still fails as before. The OpenADR page will show MQTT "Not connected" in this mode (no UI change). Firmware builds; not yet run against a VTN without a broker, so no criteria checked.
<!-- SECTION:NOTES:END -->
