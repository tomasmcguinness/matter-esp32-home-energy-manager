---
id: TASK-33
title: Expose a companion app API for the MCC iOS app
status: In Progress
assignee: []
created_date: '2026-09-20 14:41'
updated_date: '2026-09-20 14:46'
labels: []
dependencies: []
references:
  - 'https://github.com/tomasmcguinness/matter-controller-companion-app'
---

## Description

<!-- SECTION:DESCRIPTION:BEGIN -->
The MCC (Matter Controller Companion) iOS app adds Matter devices to any self-hosted controller that implements its REST contract. The HEM does not implement that contract today, so the app cannot list its devices or commission onto it.

MCC's published contract lives at `/api/nodes`, which already means the topology graph in this firmware. The companion contract is therefore namespaced under `/api/companion/` and the app will be updated to match.

Contract (base URL is the address the user types into MCC; no authentication):

| Method | Path | Purpose |
|---|---|---|
| GET | /api/companion/info | Reachability check; any 2xx passes, body ignored |
| GET | /api/companion/nodes | JSON array of commissioned Matter nodes |
| POST | /api/companion/nodes | Commission `{"inUse":false,"setupCode":"MT:..."}` -> `{"nodeId":N}`, synchronous, app waits 90s |
| PUT | /api/companion/nodes/{nodeId}/update | Rename `{"name":"..."}` |
| DELETE | /api/companion/nodes/{nodeId} | Unpair |

`{nodeId}` is decimal. Any non-2xx is a failure and MCC shows the response body text to the user, so error strings must read well. The node JSON shape is defined by MCC's iOS/Shared/Models.swift; every field except nodeId may be omitted and the client defaults it.
<!-- SECTION:DESCRIPTION:END -->

## Acceptance Criteria
<!-- AC:BEGIN -->
- [ ] #1 GET /api/companion/info returns 2xx JSON identifying the device and its current address
- [x] #2 GET /api/companion/nodes returns a bare JSON array (not an object) of commissioned Matter nodes, carrying at least nodeId, nodeName, vendorName, productName, hasSubscription and an endpoints array with endpointId, endpointName and deviceTypes
- [x] #3 Fields the HEM does not track are omitted rather than filled with misleading values; in particular no electrical power reading is reported as measuredValue
- [ ] #4 POST /api/companion/nodes commissions the device and does not respond until commissioning has finished or failed, answering with the assigned nodeId
- [ ] #5 POST /api/companion/nodes answers 400 for a missing or malformed setupCode, 504 on commissioning timeout, and 502 on other commissioning failures
- [ ] #6 PUT /api/companion/nodes/{nodeId}/update sets the device name and persists it, and answers 404 for an unknown node
- [ ] #7 DELETE /api/companion/nodes/{nodeId} unpairs the node, and still removes it locally when the device is offline and cannot be reached
- [ ] #8 The existing topology routes at /api/nodes are unchanged and the web UI Topology, Home, Power and Appliances tabs still work
- [ ] #9 The embedded web app is still served from the catch-all route
- [x] #10 The API contract is documented in the firmware, including that commissioning is on-network only and that there is no authentication
<!-- AC:END -->

## Implementation Notes

<!-- SECTION:NOTES:BEGIN -->
Implemented in `firmware/main/companion_api.c` / `.h`, registered from `web_server_start()` immediately before the `/*` catch-all. Kept out of `web_server.c` (already 2300 lines) because the contract is externally specified and self-contained; follows the heating monitor's `status_api_register(server)` pattern.

`config.max_uri_handlers` went 46 -> 52. 42 of 46 slots were in use and the five new routes overflowed it; that fails at runtime as mystery 404s, not at build time, so `companion_api_register()` logs and returns the error if a registration is ever refused.

`device_manager_get_companion_nodes_json()` (new, in `managers/device_manager.cpp`) emits MCC's flat array. It deliberately omits `isIcd`, `powerSource`, `batteryPercent`, `batteryVoltage`, `extAddress` and `measuredValue` — all optional client-side and all defaulted there. `measuredValue` especially: MCC divides it by 100 and renders it as a sensor reading in hundredths, so an electrical power value would display 2150 W as "21.50". A name the user has not set is emitted as null, not "", which is what the client's fallback to `productName` expects.

`POST /api/companion/nodes` reuses the existing synchronous `matter_controller_commission_on_network()`, so a 2xx really does mean the device joined. `DELETE` mirrors `device_delete_handler`'s offline fallback (`matter_controller_forget_node`) so a dead device can still be removed.

`/controller/commission` was left alone. Nothing in `html_app/src/` references it, so it costs nothing; removing it is separate cleanup.

Contract documented in `firmware/COMPANION_API.md`, including that commissioning is on-network only (`CONFIG_BT_ENABLED=n`, no Thread or Wi-Fi station) and that there is no authentication.

Verified so far: builds clean for esp32p4 (0x22c8d0, 6% partition free). Two host-side tests in the scratchpad covered the parts that fail silently on device — `uri_match_segments` against all five templates plus the existing `/api/nodes*` topology routes in both directions (no cross-matching), the `sscanf` node-id parsing including the reject case, and the emitted JSON (bare array, null vs "", omitted fields, no `parts`/`hasDem` leakage, `[]` when empty). AC 1, 4, 5, 6, 7, 8 and 9 still need a board.
<!-- SECTION:NOTES:END -->
