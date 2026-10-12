---
id: TASK-44.5
title: 'OpenADR VEN: HTTPS and MQTT over TLS'
status: In Progress
assignee: []
created_date: '2026-10-12 18:37'
labels:
  - openadr
dependencies: []
parent_task_id: TASK-44
priority: high
---

## Description

<!-- SECTION:DESCRIPTION:BEGIN -->
The VTN now sits behind Caddy and redirects every plain-HTTP request to https://, so the VEN (plain HTTP only) is stuck in BACKOFF. The VEN must be able to reach a VTN over HTTPS.

Decisions (user, 2026-10-12): verify the server against the public-CA certificate bundle already in the firmware (no skip-verification switch, no custom CA); MQTT follows the URI from GET /notifiers, with mqtts:// verified the same way and mqtt:// left plain; http:// base URLs are still accepted.

This lifts the "plain HTTP, no TLS" assumption in TASK-44. An HTTPS VTN must be addressed by a hostname with a publicly trusted certificate; https by bare IP fails the handshake.
<!-- SECTION:DESCRIPTION:END -->

## Acceptance Criteria
<!-- AC:BEGIN -->
- [ ] #1 Firmware builds cleanly with idf.py and the app still fits its partition
- [ ] #2 Web app type-checks and builds; no new eslint errors in OpenAdr.tsx
- [ ] #3 An https:// VTN base URL is accepted, and with it the VEN reaches RUNNING and the VTN shows the forecast
- [ ] #4 The VEN connects to the broker URI returned by GET /notifiers, whether mqtt:// or mqtts://
- [ ] #5 An http:// URL for a VTN that redirects to https shows a message naming the redirect, not 'no response'
- [ ] #6 A failed connection (for example an untrusted certificate) shows an error that names the underlying failure
- [ ] #7 An http:// VTN base URL is still accepted
<!-- AC:END -->
