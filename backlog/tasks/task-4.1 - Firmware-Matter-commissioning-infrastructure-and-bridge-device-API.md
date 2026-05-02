---
id: TASK-4.1
title: 'Firmware: Matter commissioning infrastructure and bridge device API'
status: To Do
assignee: []
created_date: '2026-04-29 12:27'
labels:
  - firmware
  - matter
  - commissioning
dependencies: []
references:
  - ~/development/matter-esp32-heating-monitor/main/commands/pairing_command.cpp
  - ~/development/matter-esp32-heating-monitor/main/managers/node_manager.cpp
  - ~/development/matter-esp32-heating-monitor/main/app_main.cpp
parent_task_id: TASK-4
priority: high
---

## Description

<!-- SECTION:DESCRIPTION:BEGIN -->
Add Matter controller capability to the energy manager firmware so it can commission a Modbus TCP bridge (Matter bridge device) over the local IP network and expose its endpoint data via the web API.

## Context

The energy manager firmware lives in `firmware/` and is currently a pure C ESP-IDF project with Ethernet, mDNS, LittleFS settings storage, and an HTTP web server (no Matter at all). The reference implementation is the heating monitor project at `~/development/matter-esp32-heating-monitor/`, which has a working Matter controller with commissioning. Key files to study there:

- `main/commands/pairing_command.cpp/.h` — Matter pairing/commissioning logic using `esp_matter::controller::matter_controller_client`
- `main/managers/node_manager.cpp/.h` — in-memory node/endpoint store backed by NVS
- `main/app_main.cpp` — shows how `nodes_post_handler` wires up the HTTP POST → `pairing_code()` call, and how post-commissioning attribute reads (PartsList, DeviceTypeList, BridgedDeviceBasicInformation NodeLabel) populate the node manager

## What to build

1. **Add Matter SDK dependency**: Add `espressif/esp_matter` (with controller feature enabled) to `firmware/main/idf_component.yml` and update `firmware/main/CMakeLists.txt`. The existing source files are C; new commissioning files should be C++. Enable the matter controller in sdkconfig.defaults.

2. **Adapt `pairing_command`** from the heating monitor into `firmware/main/commands/pairing_command.cpp/.h`. The Modbus TCP adapter is on the same IP network, so only `pairing_code()` (on-network discovery via `DiscoveryType::kDiscoveryNetworkOnly`) is needed. No Thread/BLE pairing paths required.

3. **Create a `device_manager`** in `firmware/main/managers/device_manager.cpp/.h` (adapted from the heating monitor's `node_manager`) to track commissioned devices and their endpoints. Use LittleFS + cJSON for persistence (consistent with the rest of the project) rather than NVS blobs. Store: node_id, product/vendor name, and per-endpoint: endpoint_id, label, device type IDs, and an `included` boolean.

4. **Wire up Matter controller in `main.c`** (or convert to `main.cpp`): call `esp_matter::controller::matter_controller_client::get_instance().init()` and `setup_commissioner()` after network is up (in `got_ip_event_handler`).

5. **Post-commissioning attribute reads**: On commissioning success, read the PartsList from endpoint 0 Descriptor cluster to discover bridged endpoint IDs, then for each endpoint read DeviceTypeList and BridgedDeviceBasicInformation::NodeLabel. Populate the device_manager with the results and persist.

6. **Web API endpoints** (add to `firmware/main/web_server.c`):
   - `POST /api/devices` — body: `{ "setupCode": "XXXXX-XXXXX-XXXXX" }`. Triggers commissioning. Returns 202 Accepted immediately (commissioning is async).
   - `GET /api/devices` — returns array of commissioned devices with their endpoints.
   - `PUT /api/devices/:nodeId/endpoints/:endpointId` — body: `{ "included": true/false }`. Toggles whether an endpoint is included. Persists the change.

## Acceptance criteria summary (see checklist below)
<!-- SECTION:DESCRIPTION:END -->

## Acceptance Criteria
<!-- AC:BEGIN -->
- [ ] #1 POST /api/devices accepts a setup code and returns 202 Accepted; commissioning begins asynchronously
- [ ] #2 Commissioning success/failure is logged; on success the device is stored in the device manager
- [ ] #3 After commissioning a bridge, the PartsList is read and bridged endpoint IDs are discovered
- [ ] #4 DeviceTypeList and NodeLabel are read for each bridged endpoint and stored
- [ ] #5 GET /api/devices returns all commissioned devices with their node ID, vendor, product name, and array of endpoints (id, label, device types, included flag)
- [ ] #6 PUT /api/devices/:nodeId/endpoints/:endpointId updates the included flag and persists it to LittleFS
- [ ] #7 Device data survives a reboot (loaded from LittleFS on startup)
- [ ] #8 The existing web server, settings, and mDNS functionality continue to work unchanged
<!-- AC:END -->
