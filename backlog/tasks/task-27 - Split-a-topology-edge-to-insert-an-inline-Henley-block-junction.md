---
id: TASK-27
title: Split a topology edge to insert an inline Henley-block junction
status: In Progress
assignee:
  - tomas@tomasmcguinness.com
created_date: '2026-06-10 20:12'
updated_date: '2026-06-10 20:27'
labels: []
dependencies: []
---

## Description

<!-- SECTION:DESCRIPTION:BEGIN -->
In the topology editor a user can only draw edges directly between handles. This adds the ability to split an existing edge by inserting an inline node onto it — specifically a Henley block, modelled as an unmetered junction (one power-in handle, configurable N power-out handles) that splits an incoming feed to fan out to several loads/consumer units. Triggered by right-clicking an edge.

Implementation (done, builds clean; runtime verification pending): frontend-only, no firmware changes — node/edge CRUD endpoints already cover persistence. All in firmware/html_app/src/Topology.tsx: HenleyNode component (power-in + out_1..N handles, +/- persisting via PUT /api/nodes/{id}/settings) registered as nodeTypes.henley; edgeMenu state + onEdgeContextMenu + backdrop-dismissed menu; splitEdgeWithHenley() creates the node, deletes the original edge, POSTs A->Henley (targetHandle power-in) and Henley->B (sourceHandle out_1); onInit restore passes settings.outputs into node data. Verified npx tsc -b + npm run build.
<!-- SECTION:DESCRIPTION:END -->

## Acceptance Criteria
<!-- AC:BEGIN -->
- [ ] #1 Right-clicking an edge opens a menu with 'Insert Henley block' that splits A->B into A->Henley->B
- [ ] #2 Henley node renders one power-in and a configurable number of power-out handles, with +/- to change the count
- [ ] #3 The new junction node, its output count, and the two re-wired edges persist across reload (via /api/nodes and /api/edges)
- [ ] #4 Frontend typechecks and bundles (tsc -b, vite build)
- [ ] #5 Right-clicking empty canvas offers 'Add load', which opens a metered-device picker and drops the chosen node at the click point
- [ ] #6 The new load node and a hand-drawn edge from a Henley output to it persist across reload
<!-- AC:END -->
