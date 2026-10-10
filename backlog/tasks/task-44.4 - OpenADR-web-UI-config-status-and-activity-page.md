---
id: TASK-44.4
title: 'OpenADR web UI: config, status and activity page'
status: In Progress
assignee: []
created_date: '2026-10-10 07:45'
updated_date: '2026-10-10 09:57'
labels:
  - openadr
  - ui
dependencies: []
parent_task_id: TASK-44
priority: medium
---

## Description

<!-- SECTION:DESCRIPTION:BEGIN -->
A new nav item and route /openadr in the web UI (firmware/html_app), with a page component OpenAdr.tsx styled like the existing pages, backed by the /api/openadr/* endpoints (config, status, send-now, reset — see the HTTP API subtask of TASK-44).

Config card: enabled toggle, VTN base URL, client ID, client secret (password field; placeholder "unchanged" when already set), VEN name, forecast source (Net grid / Gross consumption), optional MQTT host override, Save button.

Status card: state badge, venID, program, MQTT connected, last report (time + HTTP status), next report due, buttons "Send forecast now" and "Reset registration".

Activity log list, newest entries first.

Status updates live: reuse the existing websocket (useWebSocket.ts) if straightforward, otherwise poll /api/openadr/status every 5 s while the page is visible.
<!-- SECTION:DESCRIPTION:END -->

## Acceptance Criteria
<!-- AC:BEGIN -->
- [ ] #1 An OpenADR nav item opens /openadr
- [ ] #2 The config card loads, edits and saves every config field, and the secret field shows 'unchanged' when a secret is set
- [ ] #3 The status card shows state, venID, program, MQTT connected, last report time and HTTP status, and next report due
- [ ] #4 'Send forecast now' and 'Reset registration' call their endpoints
- [ ] #5 The activity log lists entries newest first
- [ ] #6 Status refreshes live without a page reload, and stops refreshing while the page is hidden if polling is used
- [ ] #7 MSW mock handlers exist for every new endpoint so the page works with npm run dev
- [x] #8 The web app builds without type or lint errors
<!-- AC:END -->

## Implementation Notes

<!-- SECTION:NOTES:BEGIN -->
OpenAdr.tsx, nav item and /openadr route added; MSW handlers for all five endpoints plus an `openadr_status` websocket push in mocks/handlers.ts. Live updates use the existing websocket (firmware broadcasts {type:"openadr_status"} on every state/activity change) with a GET on mount and after each reconnect, so there is no polling. tsc, eslint and vite build pass. The page has not been looked at in a browser yet. Mocks only load with `npm run dev-mock` (VITE_USE_MOCK=true), not plain `npm run dev`.
<!-- SECTION:NOTES:END -->
