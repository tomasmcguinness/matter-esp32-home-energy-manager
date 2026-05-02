---
id: TASK-4.2
title: 'Web UI: Commissioning flow and bridged device selection'
status: To Do
assignee: []
created_date: '2026-04-29 12:28'
labels:
  - web-ui
  - matter
  - commissioning
dependencies:
  - TASK-4.1
references:
  - ~/development/matter-esp32-heating-monitor/html_app/src/Devices.tsx
  - ~/development/matter-esp32-heating-monitor/html_app/src/AddDevice.tsx
parent_task_id: TASK-4
priority: high
---

## Description

<!-- SECTION:DESCRIPTION:BEGIN -->
Add web UI pages to commission a Matter bridge device and select which of its bridged endpoints to include for monitoring.

## Context

The web app lives in `firmware/html_app/` and is a React + TypeScript SPA using React Router and Bootstrap. It currently has Home and Settings pages. The reference implementation at `~/development/matter-esp32-heating-monitor/html_app/src/` shows the pattern: `Devices.tsx` (list), `AddDevice.tsx` (commissioning form), `Device.tsx` (detail). MSW mocks live in `src/mocks/handlers.ts`.

The firmware task (TASK-4.1) defines these API endpoints:
- `POST /api/devices` — `{ "setupCode": "..." }` → 202 Accepted
- `GET /api/devices` — returns `[{ nodeId, vendorName, productName, endpoints: [{ endpointId, label, deviceTypes: [number], included: boolean }] }]`
- `PUT /api/devices/:nodeId/endpoints/:endpointId` — `{ "included": boolean }` → updated endpoint

## What to build

1. **Nav link**: Add a "Devices" entry to the navbar in `App.tsx`.

2. **`Devices.tsx`** — Lists all commissioned devices. For each device show node ID (hex), vendor name, product name, and its endpoints with their label, device type badges, and included status. Link to an "Add Device" button.

3. **`AddDevice.tsx`** — Simple form: a single "Setup Code" text input (format hint: `XXXXX-XXXXX-XXXXX`) and a "Commission" submit button. POSTs to `/api/devices`. Since commissioning is async (202 Accepted), show a "Commissioning in progress…" message and poll `GET /api/devices` until the new device appears (or timeout after ~60 s). On completion, navigate to the Devices list.

4. **`DeviceEndpoints.tsx`** — Shown after commissioning or by clicking a device from the Devices list. Displays all endpoints of the device. Each endpoint row has a toggle (checkbox or switch) for `included`. Toggling PUTs to `/api/devices/:nodeId/endpoints/:endpointId`. The UI updates optimistically.

5. **MSW mocks** in `src/mocks/handlers.ts`:
   - `GET /api/devices` — returns a mock bridge device with several bridged endpoints
   - `POST /api/devices` — returns 202
   - `PUT /api/devices/:nodeId/endpoints/:endpointId` — updates and returns the endpoint

6. **Routing**: Add routes in `App.tsx`:
   - `/devices` → `Devices`
   - `/devices/add` → `AddDevice`
   - `/devices/:nodeId` → `DeviceEndpoints`
<!-- SECTION:DESCRIPTION:END -->

## Acceptance Criteria
<!-- AC:BEGIN -->
- [ ] #1 Navbar has a 'Devices' link that navigates to /devices
- [ ] #2 Devices page lists commissioned devices; shows a placeholder message when no devices exist
- [ ] #3 Each device entry shows node ID (hex), vendor name, product name, and its endpoints with label, device type badges, and included toggle
- [ ] #4 Add Device page has a setup code input with format hint and a Commission button
- [ ] #5 Submitting the form POSTs to /api/devices and shows a 'commissioning in progress' state
- [ ] #6 After commissioning completes (new device appears in GET /api/devices), the user is navigated to /devices
- [ ] #7 Clicking a device from the list navigates to /devices/:nodeId showing that device's endpoints
- [ ] #8 Each endpoint row has an included toggle; toggling it PUTs to /api/devices/:nodeId/endpoints/:endpointId and reflects the updated state
- [ ] #9 MSW mocks cover GET /api/devices, POST /api/devices (202), and PUT /api/devices/:nodeId/endpoints/:endpointId
- [ ] #10 The app builds without TypeScript errors
<!-- AC:END -->
