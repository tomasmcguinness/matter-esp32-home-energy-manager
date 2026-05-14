<!-- BACKLOG.MD MCP GUIDELINES START -->

<CRITICAL_INSTRUCTION>

## BACKLOG WORKFLOW INSTRUCTIONS

This project uses Backlog.md MCP for all task and project management activities.

**CRITICAL GUIDANCE**

- If your client supports MCP resources, read `backlog://workflow/overview` to understand when and how to use Backlog for this project.
- If your client only supports tools or the above request fails, call `backlog.get_backlog_instructions()` to load the tool-oriented overview. Use the `instruction` selector when you need `task-creation`, `task-execution`, or `task-finalization`.

- **First time working here?** Read the overview resource IMMEDIATELY to learn the workflow
- **Already familiar?** You should have the overview cached ("## Backlog.md Overview (MCP)")
- **When to read it**: BEFORE creating tasks, or when you're unsure whether to track work

These guides cover:
- Decision framework for when to create tasks
- Search-first workflow to avoid duplicates
- Links to detailed guides for task creation, execution, and finalization
- MCP tools reference

You MUST read the overview resource to understand the complete workflow. The information is NOT summarized here.

</CRITICAL_INSTRUCTION>

<!-- BACKLOG.MD MCP GUIDELINES END -->

<!-- PROJECT CONTEXT START -->

## Project: P4 Home Energy Management System (HEMS)

ESP32-P4-based home energy manager. Logs power at the grid connection, learns household consumption patterns and per-appliance usage profiles, predicts next-day surplus power, and schedules appliances to run when that surplus is available.

### System architecture

The ESP32-P4 is the central HEM and acts as the Matter **Device Energy Manager (EMS)**. It interfaces with the PV inverter over **Modbus TCP**, aggregates metering data, runs consumption/surplus forecasting, runs the appliance scheduler, and issues Matter DEM commands.

Three device tiers:

- **ESP32-P4 HEM** — the brain. Matter EMS role. Owns forecasting, scheduling, and storage.
- **ESP32-S3 "shim" devices** — one per appliance. Present to Matter as appliance nodes (Dishwasher, LaundryWasher, etc.) exposing Electrical Power/Energy Measurement, Operational State, and the **Device Energy Management cluster** with a learned Forecast. Run per-appliance cycle detection and profile learning locally. Built on the ESP-Matter SDK.
- **Shelly EM / Plug devices** — dumb-but-accurate meters. Feed live active-power data to shims/HEM over MQTT or local RPC. Use a switching variant (Plug S Gen4 / 1PM) wherever DEM start/stop control is needed; metering-only EMs for monitor-only points.

Design rule: keep learning at the tier that has a stable identity for the thing being learned. Per-appliance learning lives on the shim (appliance identity is fixed there); system-level surplus forecasting lives on the HEM (only the HEM sees solar + weather + total load together).

### Configuration canvas (node-graph editor)

The setup/config UI is a **node-graph editor** (React Flow / xyflow) where the user models their home's **electrical topology** — power flow, not data flow. Data flow is inferred; the HEM subscribes to whatever the topology implies.

Core principle: **the handle IS the role.** Roles are not stored as device metadata — they are expressed by which handle an edge connects to. A meter wired into the Consumer Unit's `Grid` handle *is* the grid meter.

- **Typed handles with cardinality.** `cu.grid` exactly 1, `cu.solar` 0..N (summed), `cu.battery` 0..1, `cu.circuit_N` 0..1 each. Meters are inline pass-through (one input handle, one output handle). Connection validity is enforced by handle type + cardinality (`isValidConnection`), so invalid topologies are unrepresentable.
- **Consumer Unit node** is the hub; its handle list is effectively the home's electrical schema. Handle set is configurable (add/remove circuits). Trunk paths = handles directly on the CU; branches = small Circuit nodes hanging off CU handles (lets circuits carry their own metadata and lets unmonitored circuits be represented explicitly).
- **Uniqueness is structural**, not enforced in application code: one `grid` handle means one grid meter. Don't limit device *types* — limit *handle occupancy*.
- **The graph is the source of truth.** Persist as JSON: nodes (id, type, position, config) + edges (from.port → to.port). Runtime state (learned profiles, schedules, role lookups) references node IDs. Node IDs are stable across device replacement — swapping hardware re-commissions a Matter node but keeps the graph node ID, its edges, and its learned data.
- **Validation is first-class:** structural errors (no grid meter assigned, unconnected mandatory port, cycle detected), warnings (stale meter, CT sign looks inverted), info (unmonitored branch). Render as badges on nodes plus a sidebar issue list.
- Edges may carry light metadata (CT sign convention, scaling coefficient).
- The HEM itself is **not** a node on the canvas — the canvas models the house; the HEM is the consumer of that model.

### Data & storage

- Per-day, per-stream files: `consumption-YYYY-MM-DD`, `solar-actual-*`, `solar-forecast-*`, `consumption-forecast-*`. Append-only, SD-card friendly. Compact binary preferred over CSV. Buffer in RAM and flush periodically to limit SD wear.
- Log actuals at 1-min resolution; **predict at 15-min** and upsample for display/scheduling. 1-min prediction is dominated by unpredictable switching noise.
- The CU's unmonitored remainder (sum of inputs − monitored circuits) is the baseload. Synthesize it as its own series; it is the most predictable input to the model.

### Forecasting

- **Open-Meteo** for both the solar PV forecast and weather features (`temperature_2m`, `cloud_cover`). Free, no API key, generous limits.
- Put forecast providers behind a thin abstraction. Solcast Hobbyist is the fallback for the solar piece if Open-Meteo accuracy proves insufficient after comparison.
- End-of-day cron builds the next-day prediction; add a morning refresh (solar forecasts sharpen closer to the day).
- Predict consumption and fetch the solar forecast **separately**, then compute `surplus = solar − consumption`. Do not predict surplus directly.

### ML approach

- **Baseline first:** "same weekday, averaged over the last N weeks" at 15-min buckets. ML must beat this baseline before it is worth adding.
- Next step after baseline: gradient boosting (per time-of-day bucket) with lag + weather + calendar features. On-device inference, off-device training.
- Realistic accuracy: 15–30% MAPE on hourly consumption. Daily totals predict far better than hourly shape.

### Scheduler

- Appliance profiles have **shape** (a power vector over the cycle), not flat power × time. Learn mean + stddev per appliance; optionally cluster by program (eco/intensive).
- Optimise by sliding the appliance power profile across the predicted surplus curve, maximising self-consumption `Σ min(profile[i], surplus[t+i])` subject to earliest/latest-start constraints.
- Output is a window ("run the dishwasher 13:00–15:00").
- For dishwasher/washer: **delay-start only** — never mid-cycle pause a machine full of water. The shim advertises these as deferrable-but-not-pausable in its DEM forecast.

### Build order

1. Log consumption + solar actuals (1-min, per-day files).
2. Pull the daily solar forecast and store it.
3. Same-weekday-average consumption baseline.
4. Compute predicted surplus (solar forecast − consumption forecast).
5. Appliance window optimiser against shaped profiles.
6. Replace the baseline with the gradient-boosting model only if it does not beat the baseline meaningfully.

<!-- PROJECT CONTEXT END -->
