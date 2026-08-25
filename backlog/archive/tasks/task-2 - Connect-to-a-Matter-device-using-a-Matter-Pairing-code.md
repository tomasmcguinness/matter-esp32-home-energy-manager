---
id: TASK-2
title: Connect to a Matter device using a Matter Pairing code
status: To Do
assignee: []
created_date: '2026-04-25 06:29'
updated_date: '2026-05-05 05:45'
labels: []
milestone: m-0
dependencies: []
---

## Description

<!-- SECTION:DESCRIPTION:BEGIN -->
By entering a Matter pairing code, the Home Energy Manager will commission that device and add it to a list of devices.
<!-- SECTION:DESCRIPTION:END -->

## Implementation Plan

<!-- SECTION:PLAN:BEGIN -->
A new devices section must be added to the web application. 

This will contain an add button. When the user opens the Add Device page, they must input the Matter pairing code.

When commissioning completes, the device's details shoudlbe shown on a Device details page.
<!-- SECTION:PLAN:END -->

## Implementation Notes

<!-- SECTION:NOTES:BEGIN -->
Matter device is assumed to be on-network. No WiFi or Thread network credentials are required.

Device Attestation can be skipped completely.
<!-- SECTION:NOTES:END -->
