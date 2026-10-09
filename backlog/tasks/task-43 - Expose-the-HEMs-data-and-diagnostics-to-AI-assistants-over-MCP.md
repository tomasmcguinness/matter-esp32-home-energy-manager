---
id: TASK-43
title: Expose the HEM's data and diagnostics to AI assistants over MCP
status: In Progress
assignee: []
created_date: '2026-10-09 15:24'
updated_date: '2026-10-09 15:25'
labels: []
dependencies: []
references:
  - firmware/main/mcp_server.c
  - firmware/main/mcp_server.h
  - firmware/main/node_power_logger.cpp
  - firmware/partitions.csv
---

## Description

<!-- SECTION:DESCRIPTION:BEGIN -->
Diagnosing the battery solar/grid mix (TASK-42.1) meant pulling raw minute files off the HEM by hand and reimplementing the ledger offline. An AI assistant connected to the HEM should be able to answer that kind of question directly.

Outcome: the HEM serves a Model Context Protocol endpoint on its existing web server, offering tools to read the topology, logged power, daily energy, forecasts, schedule, appliance profiles and tariff; diagnostic tools for the battery ledger, stream freshness and the files on the SD card; and two guarded write tools (rebuild cached day figures, refresh the forecast). Appliance control and topology editing are deliberately not offered.

Context: the app partition was 4% free, so both app partitions were enlarged from 2.3 MB to 4 MB at the same time (the board has 32 MB of flash). That moves the paa_cert, storage and config partitions, so the first flash must be over serial and the config partition (devices, topology, surplus model) must be backed up from 0x7e0000 and restored at 0xb40000.

There is no authentication, matching the rest of the HTTP API; the endpoint is for the local network only.
<!-- SECTION:DESCRIPTION:END -->

## Acceptance Criteria
<!-- AC:BEGIN -->
- [ ] #1 An MCP client can connect to the HEM, complete initialisation and list the tools
- [ ] #2 Each read tool returns the same data the web UI shows for the same node and date
- [ ] #3 Power series are available at hourly resolution for today as well as past days
- [ ] #4 The battery-mix trace shows the starting ledger and the mix at the end of each hour for a chosen day
- [ ] #5 Stream health reports the age of each node's last report and gaps in today's data
- [ ] #6 Rebuilding cached days leaves the raw minute data untouched and the rebuilt figures chain correctly from day to day
- [ ] #7 A request from a web page on another origin is refused
- [ ] #8 The existing web UI and companion API behave as before
- [x] #9 Firmware builds clean with the enlarged app partitions
<!-- AC:END -->

## Implementation Notes

<!-- SECTION:NOTES:BEGIN -->
2026-10-09: Implemented; `idf.py build` is clean (app 0x23bd10 of 0x400000, 44% free). Not flashed, so nothing has been exercised on the device or with an MCP client; only AC #9 is checked.

- `firmware/partitions.csv`: ota_0/ota_1 0x250000 -> 0x400000. New offsets: paa_cert 0x820000, storage 0x840000, config 0xb40000 (was 0x7e0000).
- `firmware/main/mcp_server.{c,h}`: POST /mcp, single JSON-RPC request per POST answered with application/json (stateless Streamable HTTP); GET /mcp answers 405. Handles initialize, ping, tools/list, tools/call; notifications get 202. Requests whose Origin does not match Host get 403. Registered from web_server_start (50 of 52 URI handler slots now used).
- Tools: get_topology, get_power_series, get_daily_energy, get_battery_source, get_forecast, get_schedule, get_appliance_profiles, get_tariff, explain_battery_mix, get_stream_health, list_data_files (read-only); recompute_days, refresh_forecast (write).
- New helpers in `firmware/main/node_power_logger.cpp`: battery_mix_trace_json, series_json (hourly computed from minute files so today works), stream_health_json, recompute_from (drops and rebuilds cost/split caches from a date to yesterday, in order).
<!-- SECTION:NOTES:END -->
