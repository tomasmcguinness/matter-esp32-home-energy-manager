# Companion App API

The REST contract the **MCC** iOS app — [Matter Controller
Companion](https://github.com/tomasmcguinness/matter-controller-companion-app) — expects of a
self-hosted Matter controller. MCC is generic: it works with any controller that answers these
endpoints, and the HEM is one of them.

The phone is **not** the Matter controller. The system Matter setup sheet scans the device's QR
code and hands the decoded onboarding payload to MCC's extension, which forwards it here. The HEM
does the commissioning.

Implemented in `main/companion_api.c`, registered from `web_server_start()`.

## Paths differ from MCC's README

MCC publishes this contract at `/api/nodes`. In this firmware `/api/nodes` already means the
**topology graph** — the canvas nodes and edges in `node_manager`, which the web UI reads and
writes from 19 call sites. So everything here is namespaced under **`/api/companion/`** instead.
MCC is configured to match.

## Conventions

- The base URL is whatever address the user typed into MCC, e.g. `http://192.168.1.42`. mDNS is
  disabled on this board (see `main.cpp`), so there is no `.local` name to use.
- Requests send `Accept: application/json`; those with a body also send `Content-Type:
  application/json`.
- **There is no authentication**, consistent with the rest of the web server. Every endpoint on
  this device is open to anyone on the LAN.
- Any non-2xx is a failure, and MCC shows the response body text to the user — so error strings
  are written to be read by a person.
- MCC's timeouts are 20 seconds, except `POST /api/companion/nodes`, which is 90.
- `{nodeId}` is the numeric Matter node id, in decimal.

| Method | Path | Purpose |
|---|---|---|
| `GET` | `/api/companion/info` | Check the controller is reachable |
| `GET` | `/api/companion/nodes` | List commissioned nodes |
| `POST` | `/api/companion/nodes` | Commission a new node |
| `PUT` | `/api/companion/nodes/{nodeId}/update` | Rename a node |
| `DELETE` | `/api/companion/nodes/{nodeId}` | Remove (unpair) a node |

## `GET /api/companion/info`

Called the moment the user types in an address, so a wrong one fails there rather than on the
device list. MCC treats any 2xx as success and **ignores the body**; the contents are for whoever
curls it.

```json
{ "v": 1, "name": "Home Energy Manager", "ip": "192.168.1.42", "url": "http://192.168.1.42" }
```

`ip` and `url` are `null` until the Ethernet interface has an address. When mDNS is re-enabled,
`url` becomes the `.local` name and nothing else about this contract changes.

## `GET /api/companion/nodes`

A JSON **array** — not the `{"devices":[…]}` object that `GET /api/devices` returns for the web
UI.

```json
[
  {
    "nodeId": 10000,
    "vendorName": "Solax",
    "productName": "Hybrid Inverter",
    "nodeName": "Solar Inverter",
    "hasSubscription": true,
    "endpoints": [
      { "endpointId": 13481, "endpointName": "Solax Inverter", "deviceTypes": [23, 17, 1296] }
    ]
  }
]
```

`vendorName` and `productName` are null until the node has answered a Basic Information read;
`nodeName` is null until someone names it. MCC shows `nodeName`, falling back to `productName`.

### Fields this controller does not send

MCC's schema also carries `isIcd`, `powerSource`, `batteryPercent`, `batteryVoltage`,
`extAddress` and, per endpoint, `measuredValue`. The HEM tracks none of them, and every field
except `nodeId` is optional on the client, so they are **omitted** rather than filled in with
something plausible.

`measuredValue` deserves a specific warning: MCC divides it by 100 and renders it as a sensor
reading in hundredths, the way the Matter temperature and flow clusters carry it. Wiring an
electrical power reading into it would display 2150 W as "21.50". Don't.

## `POST /api/companion/nodes`

```json
{ "inUse": false, "setupCode": "MT:Y.K9042C00KA0648G00" }
```

`inUse` is ignored — MCC always sends `false`, because the system setup flow only hands over
devices that are not on a fabric yet, which is the only case this controller can pair anyway.

The request is **held open until commissioning finishes**, so a 2xx means the device really did
join the fabric. On success:

```json
{ "nodeId": 10001 }
```

| Status | When |
|---|---|
| `200` | Commissioned; body carries the assigned `nodeId` |
| `400` | Body missing, not JSON, or no usable `setupCode` |
| `502` | Commissioning failed |
| `504` | Commissioning timed out — the status MCC's README names for "gave up waiting" |

### Commissioning is on-network only

`CONFIG_BT_ENABLED=n` and there is no Thread radio or Wi-Fi station in this build
(`sdkconfig.defaults`), so the HEM can only pair a device that is **already reachable on the
LAN**. In MCC's flow iOS provisions a Wi-Fi device onto the network before handing over the
payload, so that path works. A factory-new Thread device needs a border router the HEM does not
provide.

## `PUT /api/companion/nodes/{nodeId}/update`

```json
{ "name": "Dishwasher" }
```

Sets the device name and persists it. MCC calls this after commissioning with whatever the user
typed into the system setup sheet, so the device carries the same name in the app and in the web
UI. `404` if no device has that node id. The response body is ignored; a failure here does not
fail setup, because the device is already commissioned.

## `DELETE /api/companion/nodes/{nodeId}`

Unpairs the node and drops it from the device manager. No request body; the response body is
ignored.

If the device is offline, `RemoveFabric` cannot reach it. The delete is honoured anyway — the node
is forgotten locally — so a dead device can still be removed instead of being stuck forever
re-subscribing.
