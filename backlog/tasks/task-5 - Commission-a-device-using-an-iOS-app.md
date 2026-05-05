---
id: TASK-5
title: Commission a device using an iOS app
status: To Do
assignee: []
created_date: '2026-05-05 05:44'
updated_date: '2026-05-05 05:48'
labels: []
milestone: m-1
dependencies: []
---

## Description

<!-- SECTION:DESCRIPTION:BEGIN -->
To make commissioning easier, it would be nice to be able to scan a QR code using an iOS app and have the HEM commission the device.

This would be similar to how to the Matter onboarding happens in Home Assistant.
<!-- SECTION:DESCRIPTION:END -->

## Implementation Plan

<!-- SECTION:PLAN:BEGIN -->
A new endpoint is required on the device. It must accept an onboarding payload parameter. 

This will be a Matter manual pairing code or matter QR code.

The device should attempt on on-network commissioning of this device.
<!-- SECTION:PLAN:END -->
