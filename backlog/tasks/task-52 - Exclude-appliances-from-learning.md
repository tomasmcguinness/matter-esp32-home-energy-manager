---
id: TASK-52
title: Exclude appliances from learning
status: In Progress
assignee:
  - '@claude'
created_date: '2026-10-30 15:38'
updated_date: '2026-10-31 07:32'
labels: []
dependencies: []
references:
  - firmware/main/appliance_profile.c
  - firmware/main/mcp_server.c
  - firmware/html_app/src/Home.tsx
  - firmware/html_app/src/Appliances.tsx
priority: medium
---

## Description

<!-- SECTION:DESCRIPTION:BEGIN -->
Some appliances shouldn't be profiled at all, but today the only option is 'Exclude from scheduling', which still trains a profile every night. Add a per-appliance 'Exclude from learning' checkbox on the Home page's appliance edit modal, next to the existing scheduling one. An appliance excluded from learning is automatically excluded from scheduling. Learning here means the nightly per-appliance profile; power logging, Usage history, cost and the grid-level forecasting models are unaffected.
<!-- SECTION:DESCRIPTION:END -->

## Acceptance Criteria
<!-- AC:BEGIN -->
- [ ] #1 The Home page's appliance edit modal has an 'Exclude from learning' checkbox whose value persists across reloads
- [x] #2 An excluded appliance is not trained by the nightly job or by Re-analyse
- [x] #3 An excluded appliance never appears in schedule suggestions, whatever its 'Exclude from scheduling' setting
- [ ] #4 While learning is excluded the scheduling checkbox shows as checked and disabled, and unticking learning restores its earlier value
- [x] #5 get_appliance_profiles reports which appliances are excluded from learning
- [x] #6 An excluded appliance's existing profile is kept, and training resumes after the exclusion is removed
- [x] #7 Firmware and web UI builds pass
- [ ] #8 An appliance excluded from learning is not listed on the Appliances page
<!-- AC:END -->

## Implementation Plan

<!-- SECTION:PLAN:BEGIN -->
Approved by the user on 2026-10-30. Full plan: ~/.claude/plans/i-need-to-be-twinkling-fox.md

1. appliance_profile.{c,h}: new settings.excludeFromLearning flag. appliance_is_excluded() returns true for either flag, so the scheduler needs no change. train_one skips excluded appliances, covering the nightly job and Re-analyse. Profile JSON adds excluded_from_learning.
2. The existing profile file is kept, because Topology uses its standby_w for Standby/Off status.
3. Home.tsx: new checkbox above Exclude from scheduling; the scheduling box shows checked and disabled while learning is excluded, and its stored value is untouched.
4. Appliances.tsx: Excluded badge and message. Mocks and the MCP description updated.
5. Verify with builds, a host test of the real appliance_profile.c, and mock checks.
<!-- SECTION:PLAN:END -->

## Implementation Notes

<!-- SECTION:NOTES:BEGIN -->
Implemented. settings.excludeFromLearning is honoured in train_one (covers nightly job and Re-analyse); appliance_is_excluded() now returns true for either flag, so scheduler.c is unchanged. Profile JSON carries excluded_from_learning. Existing profile files are kept because Topology reads standby_w.

Verified: idf.py build and tsc pass. Host test of the real appliance_profile.c and scheduler.c with stubs (7 checks): flag truth table, excluded appliance not trained, existing profile untouched, dropped from the scheduler without the scheduling flag, JSON flag, training resumes after unticking. Also confirmed node_manager_upsert keeps settings when a PUT has none, so dragging a node keeps the flags.

Not yet verified: ACs 1, 4 and 5 are UI behaviour that has only been type-checked. They need a click-through with npm run dev-mock (the mock Kettle is excluded, to show the state) or on the device. Nothing flashed or committed.

Changed on 2026-10-31 at the user's request: excluded appliances are now left off the Appliances page altogether, replacing the Excluded badge and message. The filter is in Appliances.tsx only. GET /api/appliance/profiles still returns them, because Topology reads standby_w and get_appliance_profiles reports excluded_from_learning. If every appliance is excluded the page says so rather than claiming none are configured. Type-checked only; the UI ACs still need a click-through.

Criteria renumbered by that change: the UI items still to check are now ACs 1, 4 and 8. Plan step 4's Excluded badge is superseded.
<!-- SECTION:NOTES:END -->
