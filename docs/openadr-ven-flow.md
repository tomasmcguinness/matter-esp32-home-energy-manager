# VEN flow: the exact sequence to port to ESP-IDF

This is what `src/OpenAdr.Vtn.FakeVen` actually does against the VTN, written out step by
step with real request/response bodies, so the same sequence can be re-implemented in C
(`esp_http_client` + `esp-mqtt` + `cJSON`) without re-deriving it from the C# source.

All REST bodies are JSON except `/auth/token`, which is form-urlencoded. All timestamps are
RFC 3339 UTC (`...Z`). All MQTT payloads are JSON, published at QoS 1, not retained.

## 1. Get a token

```
POST /auth/token
Content-Type: application/x-www-form-urlencoded

grant_type=client_credentials&client_id=hems-demo-1&client_secret=demo-secret
```

```json
200 OK
{
  "access_token": "hems-demo-1|1791623597|5768dcdd647949cd9084e66212d0ff3d",
  "token_type": "Bearer",
  "expires_in": 3600,
  "scope": null
}
```

Store `access_token`. Every call below sends `Authorization: Bearer <access_token>`. The
token is opaque — don't try to parse it, just hold onto it until a call returns 401, then
repeat this step and retry that one call once (see §7).

## 2. Find or create the ven

```
GET /vens?venName=kitchen-hems
Authorization: Bearer <token>
```

If the response is a non-empty JSON array, take `[0].id` as the ven ID and skip the POST
below. Otherwise:

```
POST /vens
Authorization: Bearer <token>
Content-Type: application/json

{
  "objectType": "VEN_VEN_REQUEST",
  "venName": "kitchen-hems"
}
```

```json
201 Created
{
  "clientId": "hems-demo-1",
  "venName": "kitchen-hems",
  "targets": ["VEN_NAME:kitchen-hems", "PROGRAM_NAME:HomeForecast"],
  "attributes": null,
  "id": "ven-a7d498e6ae4a4de6",
  "createdDateTime": "2026-10-10T08:13:17.7082499+00:00",
  "modificationDateTime": "2026-10-10T08:13:17.7082499+00:00",
  "objectType": "VEN"
}
```

`clientId` and both targets are stamped on by the VTN — don't set them yourself. Keep `id`
(the ven ID) for every step below.

## 3. Discover notifiers and connect MQTT

```
GET /notifiers
Authorization: Bearer <token>
```

```json
200 OK
{
  "webhook": true,
  "mqtt": {
    "uris": ["mqtt://<vtn-host>:1883"],
    "serialization": "JSON",
    "authentication": { "method": "ANONYMOUS" }
  }
}
```

Connect a plain (no TLS) MQTT client to `<vtn-host>:1883` — this demo VTN only ever offers
`ANONYMOUS` auth. **Use the ven's `venName` as the MQTT client ID** (e.g. `kitchen-hems`):
the VTN tracks per-VEN MQTT connection status by matching the connecting client ID back to
a venName, for the dashboard.

## 4. Subscribe to the ven's own topics

```
GET /notifiers/mqtt/topics/vens/{venId}/events
GET /notifiers/mqtt/topics/vens/{venId}/programs
```

Each returns:

```json
{
  "topics": {
    "create": "vens/ven-a7d498e6ae4a4de6/events/create",
    "update": "vens/ven-a7d498e6ae4a4de6/events/update",
    "delete": "vens/ven-a7d498e6ae4a4de6/events/delete",
    "all": "vens/ven-a7d498e6ae4a4de6/events/+"
  }
}
```

Subscribe to the `all` topic from each response (QoS 1). **Never hardcode a topic string** —
always subscribe to whatever these endpoints return; the VTN is free to change its topic
naming scheme as long as these endpoints stay in sync with it.

## 5. Discover the program and its event topics

```
GET /programs?targets=PROGRAM_NAME%3AHomeForecast
Authorization: Bearer <token>
```

```json
200 OK
[
  {
    "programName": "HomeForecast",
    "id": "program-c9499a63b2d24295",
    "targets": ["PROGRAM_NAME:HomeForecast"],
    ...
  }
]
```

Take `[0].id` as the program ID, then subscribe to that program's event topic too:

```
GET /notifiers/mqtt/topics/programs/{programId}/events
```

→ subscribe to its `all` topic, same shape as step 4.

## 6. Discover the forecast event

```
GET /events?programId=program-c9499a63b2d24295
Authorization: Bearer <token>
```

Walk the returned array and find the event whose `reportDescriptors` contains an entry with
`"payloadType": "FORECAST"`:

```json
{
  "id": "event-368de20708534772",
  "programId": "program-c9499a63b2d24295",
  "eventName": "ForecastRequestEvent",
  "intervalPeriod": { "start": null, "duration": "PT1H", "randomizeStart": null },
  "reportDescriptors": [
    {
      "payloadType": "FORECAST",
      "readingType": "DEMAND",
      "units": "KW",
      "aggregate": true,
      "numIntervals": 24,
      "frequency": 1,
      "repeat": -1
    }
  ]
}
```

Keep this event's `id` — every report posted below references it as `eventId`.

## 7. Post reports, once per hour (aligned to the hour)

Two reports per tick, both `POST /reports` with `Authorization: Bearer <token>`:

**a) The 24-hour forecast**, one interval per upcoming hour, each with its own absolute
`intervalPeriod.start`:

```json
{
  "eventId": "event-368de20708534772",
  "clientName": "kitchen-hems",
  "reportName": "Forecast 2026-10-10T13",
  "payloadDescriptors": [
    { "payloadType": "FORECAST", "readingType": "DEMAND", "units": "KW" }
  ],
  "resources": [
    {
      "resourceName": "AGGREGATED_REPORT",
      "intervals": [
        {
          "id": 0,
          "intervalPeriod": { "start": "2026-10-10T13:00:00Z", "duration": "PT1H" },
          "payloads": [{ "type": "FORECAST", "values": [1.842] }]
        },
        {
          "id": 1,
          "intervalPeriod": { "start": "2026-10-10T14:00:00Z", "duration": "PT1H" },
          "payloads": [{ "type": "FORECAST", "values": [1.503] }]
        }
        /* ... 24 intervals total; 23 or 25 on a DST transition day, which the VTN
           accepts without complaint — it never assumes exactly 24. */
      ]
    }
  ]
}
```

**b) The actual reading for the hour that just completed**, one interval:

```json
{
  "eventId": "event-368de20708534772",
  "clientName": "kitchen-hems",
  "reportName": "Actual 2026-10-10T12",
  "payloadDescriptors": [
    { "payloadType": "DIRECT_READ", "readingType": "DEMAND", "units": "KW" }
  ],
  "resources": [
    {
      "resourceName": "AGGREGATED_REPORT",
      "intervals": [
        {
          "id": 0,
          "intervalPeriod": { "start": "2026-10-10T12:00:00Z", "duration": "PT1H" },
          "payloads": [{ "type": "DIRECT_READ", "values": [1.76] }]
        }
      ]
    }
  ]
}
```

Both return `201 Created` with the full stored report (server-assigned `id`,
`createdDateTime`, etc.) on success. **Every POST creates a new report** — the VTN never
merges or overwrites by `clientName`/`eventId`, which is what keeps the whole day's forecast
history available for the dashboard's "ghost earlier forecasts" view. Don't try to update a
previous report in place; just post a new one each time.

## Recovery paths

These are the only three failure modes this flow needs to handle, and none of them should
ever require a restart:

1. **A REST call returns 401.** The token expired. Go back to step 1, get a new token, and
   retry the call that failed — once. If it 401s again, something's actually wrong
   (treat as a hard error, don't loop forever).
2. **`POST /reports` returns 404.** The event (or its program, or the ven) was deleted or
   recreated server-side. Re-run steps 2, 5, and 6 (re-discover ven/program/event) and try
   the post again on the next tick.
3. **The MQTT connection drops.** Reconnect with the same client ID (the venName), re-issue
   every subscription from steps 4–5 (re-fetch the topic names — don't cache them across a
   reconnect, in case they changed), and re-run the REST discovery in steps 2, 5, and 6 to
   make sure local state (ven/program/event IDs) is still current.

None of these need to be handled with any urgency — a VEN that misses a tick because it was
mid-recovery just catches up on the next one.

## `--fast` mode

`OpenAdr.Vtn.FakeVen <vtnUrl> <mqttHost> --fast` runs the same sequence but posts every 10
seconds instead of waiting for the top of the hour, with a synthetic demand curve that drifts
slightly on every tick — useful for watching the dashboard update live without waiting around.
The *actual* (DIRECT_READ) report still only advances to a new hour when a real hour
boundary passes; only the forecast re-posts (and its drift) are accelerated.
