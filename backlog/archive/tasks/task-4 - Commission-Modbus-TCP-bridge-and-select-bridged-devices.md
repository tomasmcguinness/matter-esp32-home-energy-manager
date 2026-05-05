---
id: TASK-4
title: Commission Modbus TCP bridge and select bridged devices
status: To Do
assignee: []
created_date: '2026-04-29 12:27'
labels:
  - matter
  - commissioning
  - modbus
dependencies: []
priority: high
---

## Description

<!-- SECTION:DESCRIPTION:BEGIN -->
Enable the energy manager to commission a Matter bridge device (the Modbus TCP adapter) and allow the user to choose which of its bridged endpoints to include for monitoring.

The Modbus TCP adapter is a Matter bridge that exposes multiple bridged devices as endpoints. The energy manager needs to act as a Matter controller, commission the bridge over the local IP network using a setup code, read the bridge's PartsList to discover its bridged endpoints, and then let the user select which ones to use.

This work is split into two subtasks:
- Firmware: Matter controller infrastructure and API
- Web UI: Commissioning flow and bridged device selection UI
<!-- SECTION:DESCRIPTION:END -->
