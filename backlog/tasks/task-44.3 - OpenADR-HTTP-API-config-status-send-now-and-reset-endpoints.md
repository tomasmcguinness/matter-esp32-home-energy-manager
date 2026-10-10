---
id: TASK-44.3
title: 'OpenADR HTTP API: config, status, send-now and reset endpoints'
status: In Progress
assignee: []
created_date: '2026-10-10 07:45'
updated_date: '2026-10-10 09:57'
labels:
  - openadr
  - firmware
dependencies: []
parent_task_id: TASK-44
priority: medium
---

## Description

<!-- SECTION:DESCRIPTION:BEGIN -->
The web server exposes the OpenADR VEN (see the VEN core subtask of TASK-44) to the web UI:

- GET /api/openadr/config — never returns client_secret; returns secret_set true/false instead.
- PUT /api/openadr/config — saves to NVS and restarts the VEN state machine. An empty secret keeps the existing one.
- GET /api/openadr/status — state, last error, venID, program name, event found, MQTT connected, last report time and HTTP status, next report due, forecast source actually used, activity log.
- POST /api/openadr/send-now — send the forecast report immediately.
- POST /api/openadr/reset — clear the cached IDs and re-register.

The HTTP server's max_uri_handlers is 64 with 54 in use; exceeding it makes the catch-all static handler fail to register and 404s the whole web UI.
<!-- SECTION:DESCRIPTION:END -->

## Acceptance Criteria
<!-- AC:BEGIN -->
- [ ] #1 GET /api/openadr/config returns the stored config with secret_set and never the secret
- [ ] #2 PUT /api/openadr/config persists the config and restarts the VEN
- [ ] #3 PUT with an empty secret keeps the stored secret
- [ ] #4 GET /api/openadr/status returns every field listed in the description
- [ ] #5 POST /api/openadr/send-now triggers an immediate forecast report without blocking the HTTP server on the VTN call
- [ ] #6 POST /api/openadr/reset clears cached IDs and the VEN re-registers
- [ ] #7 max_uri_handlers fits all handlers, the handler-count comment is accurate, and the web UI still loads
<!-- AC:END -->

## Implementation Notes

<!-- SECTION:NOTES:BEGIN -->
Five handlers added to web_server.c; 59 of 64 URI handler slots in use (51 in web_server.c, 5 companion, 2 MCP, 1 websocket), so max_uri_handlers stays at 64 and the comment is updated. send-now and reset return 202 and only post to the VEN task's queue. Not yet exercised on the device; confirm `/` still loads after flashing.
<!-- SECTION:NOTES:END -->
