---
id: TASK-44.5
title: 'OpenADR VEN: HTTPS and MQTT over TLS'
status: In Progress
assignee: []
created_date: '2026-10-12 18:37'
updated_date: '2026-10-13 15:44'
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
- [x] #1 Firmware builds cleanly with idf.py and the app still fits its partition
- [x] #2 Web app type-checks and builds; no new eslint errors in OpenAdr.tsx
- [x] #3 An https:// VTN base URL is accepted, and with it the VEN reaches RUNNING and the VTN shows the forecast
- [ ] #4 The VEN connects to the broker URI returned by GET /notifiers, whether mqtt:// or mqtts://
- [ ] #5 An http:// URL for a VTN that redirects to https shows a message naming the redirect, not 'no response'
- [ ] #6 A failed connection (for example an untrusted certificate) shows an error that names the underlying failure
- [ ] #7 An http:// VTN base URL is still accepted
<!-- AC:END -->

## Implementation Notes

<!-- SECTION:NOTES:BEGIN -->
2026-10-12: implemented in openadr_ven.c (b42ae5d) and OpenAdr.tsx. http_do() and the esp-mqtt config attach esp_crt_bundle_attach, which is only used for https:// and mqtts://. The config setter accepts http:// or https://. Redirects are still not followed (the VEN would re-send its client secret) and are reported with a hint to change the base URL. No-response errors now carry the esp-tls or HTTP client error name and any certificate verify flags; the first MQTT_EVENT_ERROR of an outage is written to the activity list. No sdkconfig or component changes. Builds pass: firmware 0x24a7a0 of 0x400000 under IDF 5.5.5; tsc, vite build and eslint on OpenAdr.tsx clean.

NOT run on the device: AC #3-#7 are unverified and need a flash plus the VTN's hostname (https by bare IP fails the handshake). Also unchecked on hardware: VEN task stack headroom during a TLS handshake (VEN_TASK_STACK is 10240). Each REST call opens a new TLS session; no connection reuse.

2026-10-13: on the device with https://vtn.tomasmcguinness.com the VEN registered, connected MQTT and posted a report (HTTP 201 at 06:40 UTC+1), so AC #3 holds. It then stuck in BACKOFF with 'POST /reports: no response from VTN (ESP_ERR_NOT_SUPPORTED)'. Cause: the bearer token expired and the VTN returned 401; esp_http_client intercepts a 401 to answer with Basic/Digest auth and returns ESP_ERR_NOT_SUPPORTED when it cannot, so http_do() returned -1 instead of 401 and vtn_call()'s re-authentication never ran. Fixed by passing a 401 through. This bug predates HTTPS; the demo VTN never expired tokens. Builds; not yet flashed, so recovery after token expiry is unverified. AC #4 seen for whichever scheme /notifiers returned (MQTT connected); the scheme was not recorded.

Correction: the last successful report was at 07:00:08 BST on 2026-10-13, not 06:40.
<!-- SECTION:NOTES:END -->
