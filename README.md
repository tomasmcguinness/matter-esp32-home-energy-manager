# Matter Home Energy Manager

A Home Energy Manager (HEM) for the ESP32-P4 that acts as a Matter **Device Energy Management (DEM)** controller. It logs power at the grid connection, learns household consumption and per-appliance usage patterns, predicts the next day's solar surplus, and creates a suggested schedule for appliances to make the most of any solar surplus.

## Overview

The system models a home's electrical topology and uses it to drive forecasting and scheduling. The goal is to maximise self-consumption of solar generation by shifting deferrable loads (dishwasher, washing machine, etc.) into windows where surplus power is predicted.

## Device tiers

The system is built from three tiers of device:

- **ESP32-P4 HEM** — the brain. Acts as the Matter Energy Management System (EMS). It interfaces with the PV inverter over Modbus TCP, aggregates metering data, runs consumption/surplus forecasting, runs the appliance scheduler, and issues Matter DEM commands. Owns all forecasting, scheduling, and storage.
- **ESP32-S3 "shim" devices** — one per appliance. Present to Matter as appliance nodes (Dishwasher, LaundryWasher, etc.) exposing Electrical Power/Energy Measurement, Operational State, and the Device Energy Management cluster with a learned Forecast. Run per-appliance cycle detection and profile learning locally.
- **Shelly EM / Plug devices** — accurate meters. Feed live active-power data to shims and the HEM over MQTT or local RPC. Switching variants (Plug S / 1PM) are used wherever DEM start/stop control is needed; metering-only EMs for monitor-only points.

**Design rule:** learning lives at the tier that has a stable identity for the thing being learned. Per-appliance learning lives on the shim (appliance identity is fixed there); system-level surplus forecasting lives on the HEM (only the HEM sees solar + weather + total load together).

## Configuration UI

The setup/config UI is a **node-graph editor** (React Flow / xyflow, in `firmware/html_app`) where the user models their home's electrical topology — power flow, not data flow. Data flow is inferred from the topology.

Core principle: **the handle IS the role.** A meter wired into the Consumer Unit's `Grid` handle *is* the grid meter — roles are expressed by which handle an edge connects to, not stored as device metadata. Typed handles with cardinality (`grid` exactly 1, `solar` 0..N, `battery` 0..1, circuits 0..1 each) make invalid topologies unrepresentable.

The graph is the source of truth, persisted as JSON (nodes + edges). Runtime state references stable node IDs, so swapping hardware re-commissions a Matter node but keeps the graph node ID, its edges, and its learned data.

## Forecasting & scheduling

- **Solar/weather forecast:** Open-Meteo (`temperature_2m`, `cloud_cover`), behind a thin provider abstraction.
- **Surplus prediction:** an on-device linear regression predicts surplus directly from the solar forecast — `surplus[h] = a[h]·solar_forecast[h] + b[h][dow]`, trained nightly from paired forecast + grid history. Falls back to `solar − consumption` until enough history is collected.
- **Consumption baseline:** "same weekday, averaged over the last N weeks."
- **Scheduler:** slides a learned, *shaped* appliance power profile across the predicted surplus curve, maximising self-consumption subject to earliest/latest-start constraints. Output is a run window (e.g. "run the dishwasher 13:00–15:00"). Deferrable appliances are delay-start only — never mid-cycle paused.

## Data & storage

Per-day, per-stream append-only files on SD card (`consumption-*`, `solar-actual-*`, `solar-forecast-*`, `consumption-forecast-*`). Actuals are logged at 1-minute resolution; predictions are made at 15-minute resolution and upsampled. Data is buffered in RAM and flushed periodically to limit SD wear.

## Repository layout

| Path | Contents |
| --- | --- |
| `firmware/` | ESP-IDF / ESP-Matter firmware for the ESP32-P4 HEM |
| `firmware/main/` | Core C/C++ sources — logging, forecasting, scheduler, web/WS server |
| `firmware/html_app/` | React topology editor (config UI) |
| `firmware/html_compiled_app/` | Built web app served by the device |
| `app/` | macOS Matter commissioner app |
| `simulators/` | Test simulators (e.g. a bridge with solar power) |
| `backlog/` | Backlog.md task and project tracking |

## Building

The firmware is built with ESP-IDF and the ESP-Matter SDK. From `firmware/`:

```sh
idf.py set-target esp32p4
idf.py build flash monitor
```

## License

MIT — see [LICENSE](LICENSE).
