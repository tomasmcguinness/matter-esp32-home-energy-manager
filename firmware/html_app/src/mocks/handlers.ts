import { http, HttpResponse, ws } from 'msw'
import type { Device } from '../Devices'

export type Settings = {
  name: string
}

let settings: Settings = { name: 'Home Energy Manager' }

type NodeConfig = { id: string; x: number; y: number; settings: Record<string, unknown> }
type EdgeConfig = { id: string; source: string; target: string; sourceHandle?: string; targetHandle?: string }

// Seed a small topology so the Power page has connected nodes to render in dev:
// a consumer unit fed by a grid meter, with an oven hanging off circuit 1.
let nodeConfigs: NodeConfig[] = [
  { id: 'consumer_unit', x: 0, y: 0, settings: { label: 'Consumer Unit', type: 'consumerUnit', deletable: false } },
  { id: 'grid_meter', x: -220, y: 0, settings: { label: 'Grid Meter', type: 'device', nodeId: 30001, endpointId: 1 } },
  { id: 'node_11', x: 260, y: -40, settings: { name: 'Oven', label: 'Oven', type: 'appliance', nodeId: 20001, endpointId: 1 } },
]
let edgeConfigs: EdgeConfig[] = [
  { id: 'grid_meter-power-out-consumer_unit-grid', source: 'grid_meter', target: 'consumer_unit', sourceHandle: 'power-out', targetHandle: 'grid' },
  { id: 'consumer_unit-circuit_1-node_11-power-in', source: 'consumer_unit', target: 'node_11', sourceHandle: 'circuit_1', targetHandle: 'power-in' },
]

let devices: Device[] = [
  {
    nodeId: 10000,
    vendorName: 'Modbus',
    productName: 'TCP Adapter',
    endpoints: [
      { endpointId: 0, label: 'Root Node',        included: false, deviceTypes: [0x0016], parts: [1, 2] },
      { endpointId: 1, label: 'Solax Inverter',   included: false, deviceTypes: [0x0017], parts: [] },
      { endpointId: 2, label: 'FeedIn CT Clamp',  included: false, deviceTypes: [0x0510], parts: [] },
    ],
    hasSubscription: true,
  },
  {
    nodeId: 20001,
    name: 'Garage Meter',
    vendorName: 'Shelly',
    productName: 'Pro 3EM',
    hasSubscription: true,
    endpoints: [
      { endpointId: 0, label: 'Root Node',   included: false, deviceTypes: [0x0016], parts: [1] },
      { endpointId: 1, label: 'Grid Meter',  included: true,  deviceTypes: [0x0512], parts: [2] },
      { endpointId: 2, label: 'Grid Sensor', included: false, deviceTypes: [0x0510], parts: [] },
    ],
  },
  {
    nodeId: 30001,
    vendorName: 'Cold Bear',
    productName: 'Smart Meter',
    hasSubscription: true,
    endpoints: [
      { endpointId: 0, label: 'Root Node',   included: false, deviceTypes: [0x0016], parts: [1,2] },
      { endpointId: 1, label: 'Meter Reference Point',  included: true,  deviceTypes: [0x0512], parts: [2] },
      { endpointId: 2, label: 'Electrical Sensor', included: false, deviceTypes: [0x0514], parts: [] },
    ],
  },
]

const powerWs = ws.link('ws://*/ws')

export const handlers = [
  http.get('/api/settings', () => {
    return HttpResponse.json(settings)
  }),

  http.put('/api/settings', async ({ request }) => {
    const body = (await request.json()) as Settings
    settings = { ...settings, ...body }
    return HttpResponse.json(settings)
  }),

  http.get('/api/devices', () => {
    return HttpResponse.json({ devices })
  }),

  http.get('/api/devices/simple', () => {
    const ELECTRICAL_SENSOR_DT = 0x0510
    const SOLAR_POWER_DT = 0x0017
    const simpleDevices = devices.flatMap(d => {
      const ep0 = d.endpoints.find(e => e.endpointId === 0)
      const ep0Parts = ep0?.parts ?? []
      const candidates = ep0Parts.length > 0
        ? d.endpoints.filter(e => ep0Parts.includes(e.endpointId))
        : d.endpoints.filter(e => e.endpointId !== 0)
      return candidates.map(ep => {
        const partEps = (ep.parts ?? []).map(id => d.endpoints.find(e => e.endpointId === id)).filter(Boolean)
        const hasElectricalSensor = [ep, ...partEps].some(e => e?.deviceTypes?.includes(ELECTRICAL_SENSOR_DT))
        const hasSolarPower = [ep, ...partEps].some(e => e?.deviceTypes?.includes(SOLAR_POWER_DT))
        return { nodeId: d.nodeId, endpointId: ep.endpointId, label: ep.label || `EP${ep.endpointId}`, hasElectricalSensor, hasSolarPower }
      })
    })
    return HttpResponse.json({ devices: simpleDevices })
  }),

  http.get('/api/devices/endpoints', ({ request }) => {
    const url = new URL(request.url)
    const raw = url.searchParams.get('deviceTypeId') ?? '0'
    const filterType = raw.startsWith('0x') ? parseInt(raw, 16) : parseInt(raw, 10)
    // An endpoint may be assigned to at most one topology node regardless of role,
    // so exclude any node that has already claimed a nodeId/endpointId.
    const inUse = nodeConfigs
      .filter(n => typeof n.settings?.nodeId === 'number' && typeof n.settings?.endpointId === 'number')
      .map(n => ({ nodeId: n.settings.nodeId as number, endpointId: n.settings.endpointId as number }))
    const endpoints = devices.flatMap(d =>
      d.endpoints
        .filter(ep => ep.endpointId !== 0 && ep.deviceTypes?.includes(filterType))
        .filter(ep => !inUse.some(u => u.nodeId === d.nodeId && u.endpointId === ep.endpointId))
        .map(ep => ({ nodeId: d.nodeId, endpointId: ep.endpointId, label: ep.label || `EP${ep.endpointId}`, deviceName: d.productName }))
    )
    return HttpResponse.json({ endpoints })
  }),

  http.put('/api/devices/:nodeId/name', async ({ params, request }) => {
    const nodeId = Number(params.nodeId)
    const body = (await request.json()) as { name: string }
    devices = devices.map((d) => (d.nodeId === nodeId ? { ...d, name: body.name } : d))
    return HttpResponse.json({})
  }),

  http.put('/api/devices/:nodeId/endpoints/:endpointId', async ({ params, request }) => {
    const nodeId = Number(params.nodeId)
    const endpointId = Number(params.endpointId)
    const body = (await request.json()) as { included: boolean }
    devices = devices.map((d) =>
      d.nodeId === nodeId
        ? {
            ...d,
            endpoints: d.endpoints.map((ep) =>
              ep.endpointId === endpointId ? { ...ep, included: body.included } : ep
            ),
          }
        : d
    )
    return HttpResponse.json({})
  }),

  http.get('/api/nodes', () => {
    // Mirror the firmware: nodes wired to a Matter endpoint carry the current
    // cached ElectricalPowerMeasurement values (cluster 144) so power renders on load.
    const nodes = nodeConfigs.map(n => {
      const nodeId = n.settings?.nodeId
      const endpointId = n.settings?.endpointId
      if (typeof nodeId !== 'number' || typeof endpointId !== 'number') return n
      return {
        ...n,
        values: [
          { clusterId: 144, attributeId: 0x04, value: 230000 },  // 230.0 V
          { clusterId: 144, attributeId: 0x05, value: 4350 },    // 4.35 A
          { clusterId: 144, attributeId: 0x08, value: 1000000 }, // 1000.0 W
        ],
      }
    })
    return HttpResponse.json({ nodes, edges: edgeConfigs })
  }),

  http.put('/api/nodes/:nodeId', async ({ params, request }) => {
    const id = params.nodeId as string
    const body = (await request.json()) as { x: number; y: number; settings?: Record<string, unknown> }
    const existing = nodeConfigs.find(n => n.id === id)
    if (existing) {
      existing.x = body.x
      existing.y = body.y
      if (body.settings) existing.settings = body.settings
    } else {
      nodeConfigs.push({ id, x: body.x, y: body.y, settings: body.settings ?? {} })
    }
    return HttpResponse.json({})
  }),

  http.get('/api/data/grid', ({ request }) => {
    const url = new URL(request.url)
    const date = url.searchParams.get('date') ?? new Date().toISOString().slice(0, 10)
    const [year, month, day] = date.split('-').map(Number)
    const startOfDay = Math.floor(new Date(year, month - 1, day).getTime() / 1000)
    const endOfDay = startOfDay + 86400
    const nowSec = Math.floor(Date.now() / 1000)
    const endTime = Math.min(nowSec, endOfDay)
    const records = []
    for (let t = startOfDay; t < endTime; t += 60) {
      const hour = (t - startOfDay) / 3600
      const base = 400
      const morning = hour >= 7 && hour < 9 ? 1800 * Math.sin(Math.PI * (hour - 7) / 2) : 0
      const evening = hour >= 17 && hour < 21 ? 2500 * Math.sin(Math.PI * (hour - 17) / 4) : 0
      const solar = hour >= 9 && hour < 17 ? 3000 * Math.sin(Math.PI * (hour - 9) / 8) : 0
      const noise = (Math.random() - 0.5) * 150
      // positive = import, negative = export
      records.push({ minute: t, power_w: Math.round(base + morning + evening - solar + noise) })
    }
    return HttpResponse.json({ records })
  }),

  http.get('/api/data/node', ({ request }) => {
    const url = new URL(request.url)
    const id = url.searchParams.get('id') ?? ''
    const date = url.searchParams.get('date') ?? new Date().toISOString().slice(0, 10)
    const [year, month, day] = date.split('-').map(Number)
    const startOfDay = Math.floor(new Date(year, month - 1, day).getTime() / 1000)
    const endOfDay = startOfDay + 86400
    const nowSec = Math.floor(Date.now() / 1000)
    const endTime = Math.min(nowSec, endOfDay)

    // Only the seeded oven has synthetic data; other nodes return empty (the
    // chart then shows "No recordings for this day").
    if (id !== 'node_11') return HttpResponse.json({ records: [] })

    // An oven runs in a few short bursts: ~2kW with the element duty-cycling.
    const bursts = [[11.0, 11.8], [18.0, 19.25]] // [startHour, endHour]
    const records = []
    for (let t = startOfDay; t < endTime; t += 60) {
      const hour = (t - startOfDay) / 3600
      const inBurst = bursts.some(([a, b]) => hour >= a && hour < b)
      const duty = inBurst && (Math.floor((t - startOfDay) / 60) % 5) < 3 // ~60% on
      const noise = duty ? (Math.random() - 0.5) * 120 : 0
      records.push({ minute: t, power_w: Math.round((duty ? 2000 : 0) + noise) })
    }
    return HttpResponse.json({ records })
  }),

  http.put('/api/topology/grid', async ({ request }) => {
    const body = (await request.json()) as { nodeId: number; endpointId: number; label: string }
    const cu = nodeConfigs.find(n => n.id === 'consumer_unit')
    const cuX = cu?.x ?? 0
    const cuY = cu?.y ?? 0
    const nodeX = cuX - 220
    const nodeY = cuY
    const nodeId = 'grid_meter'
    const edgeId = 'grid_meter-power-out-consumer_unit-grid'
    const settings = { label: body.label, type: 'device', nodeId: body.nodeId, endpointId: body.endpointId }
    const existing = nodeConfigs.find(n => n.id === nodeId)
    if (existing) {
      existing.x = nodeX; existing.y = nodeY; existing.settings = settings
    } else {
      nodeConfigs.push({ id: nodeId, x: nodeX, y: nodeY, settings })
    }
    return HttpResponse.json({
      node: { id: nodeId, x: nodeX, y: nodeY, settings },
      edge: { id: edgeId, source: nodeId, sourceHandle: 'power-out', target: 'consumer_unit', targetHandle: 'grid' },
    })
  }),

  http.put('/api/topology/solar', async ({ request }) => {
    const body = (await request.json()) as { nodeId: number; endpointId: number; label: string }
    const cu = nodeConfigs.find(n => n.id === 'consumer_unit')
    const cuX = cu?.x ?? 0
    const cuY = cu?.y ?? 0
    const nodeX = cuX + 220
    const nodeY = cuY
    const nodeId = 'solar_inverter'
    const edgeId = 'solar_inverter-power-out-consumer_unit-solar_input'
    const settings = { label: body.label, type: 'device', nodeId: body.nodeId, endpointId: body.endpointId }
    const existing = nodeConfigs.find(n => n.id === nodeId)
    if (existing) {
      existing.x = nodeX; existing.y = nodeY; existing.settings = settings
    } else {
      nodeConfigs.push({ id: nodeId, x: nodeX, y: nodeY, settings })
    }
    return HttpResponse.json({
      node: { id: nodeId, x: nodeX, y: nodeY, settings },
      edge: { id: edgeId, source: nodeId, sourceHandle: 'power-out', target: 'consumer_unit', targetHandle: 'solar_input' },
    })
  }),

  http.delete('/api/nodes/:nodeId', ({ params }) => {
    const id = params.nodeId as string
    nodeConfigs = nodeConfigs.filter(n => n.id !== id)
    edgeConfigs = edgeConfigs.filter(e => e.source !== id && e.target !== id)
    return HttpResponse.json({})
  }),

  http.post('/api/edges', async ({ request }) => {
    const body = (await request.json()) as EdgeConfig
    const existing = edgeConfigs.find(e => e.id === body.id)
    if (!existing) edgeConfigs.push(body)
    return HttpResponse.json({})
  }),

  http.delete('/api/edges/:edgeId', ({ params }) => {
    const id = params.edgeId as string
    edgeConfigs = edgeConfigs.filter(e => e.id !== id)
    return HttpResponse.json({})
  }),

  http.delete('/api/edges/:edgeId', () => {
    return HttpResponse.json({})
  }),

  http.put('/api/nodes/:nodeId/settings', async ({ params, request }) => {
    const id = params.nodeId as string
    const body = (await request.json()) as Record<string, unknown>
    const existing = nodeConfigs.find(n => n.id === id)
    if (existing) {
      existing.settings = { ...existing.settings, ...body }
    } else {
      nodeConfigs.push({ id, x: 0, y: 0, settings: body })
    }
    return HttpResponse.json({})
  }),

  http.get('/api/forecast/surplus', ({ request }) => {
    const url = new URL(request.url)
    const date = url.searchParams.get('date') ?? new Date(Date.now() + 86400000).toISOString().slice(0, 10)
    const [y, mo, d] = date.split('-').map(Number)
    const slots = Array.from({ length: 24 }, (_, h) => {
      const solar       = h >= 9  && h < 17 ? 3000 * Math.sin(Math.PI * (h - 9)  / 8) : 0
      const consumption = 400
        + (h >= 7  && h < 9  ? 1800 * Math.sin(Math.PI * (h - 7)  / 2) : 0)
        + (h >= 17 && h < 21 ? 2500 * Math.sin(Math.PI * (h - 17) / 4) : 0)
      const hour_ts = Math.floor(new Date(y, mo - 1, d, h).getTime() / 1000)
      return { hour_ts, surplus_w: Math.round(solar - consumption) }
    })
    return HttpResponse.json({ date, slots })
  }),

  http.get('/api/forecast/consumption', ({ request }) => {
    const url = new URL(request.url)
    const date = url.searchParams.get('date') ?? new Date(Date.now() + 86400000).toISOString().slice(0, 10)
    const [y, mo, d] = date.split('-').map(Number)
    const slots = Array.from({ length: 24 }, (_, h) => {
      const base    = 400
      const morning = h >= 7  && h < 9  ? 1800 * Math.sin(Math.PI * (h - 7)  / 2) : 0
      const evening = h >= 17 && h < 21 ? 2500 * Math.sin(Math.PI * (h - 17) / 4) : 0
      const solar   = h >= 9  && h < 17 ? 3000 * Math.sin(Math.PI * (h - 9)  / 8) : 0
      const hour_ts = Math.floor(new Date(y, mo - 1, d, h).getTime() / 1000)
      return { hour_ts, power_w: Math.round(base + morning + evening - solar) }
    })
    return HttpResponse.json({ date, slots })
  }),

  http.post('/api/test/generate-sample-data', () => {
    return HttpResponse.json({})
  }),

  http.post('/api/test/rollup-hourly', () => {
    return HttpResponse.json({})
  }),

  http.post('/api/test/consumption-forecast/compute', () => {
    return HttpResponse.json({})
  }),

  http.post('/api/test/run-daily-job', () => {
    return HttpResponse.json({})
  }),

  http.get('/api/appliance/profiles', () => {
    // node_11 (Oven) has a learned profile; a second appliance is still learning.
    return HttpResponse.json({
      appliances: [
        {
          graph_id: 'node_11',
          trained: true,
          standby_w: 3,
          avg_program_power_w: 1820,
          std_program_power_w: 210,
          avg_program_len_min: 112,
          std_program_len_min: 9,
          program_count: 14,
          days_with_data: 26,
          trained_unix: Math.floor(Date.now() / 1000) - 3600,
          window_days: 30,
        },
        {
          graph_id: 'node_42',
          trained: false,
        },
      ],
    })
  }),

  http.post('/api/test/appliance-profiles/train', () => {
    return HttpResponse.json({ trained: true })
  }),

  http.get('/api/forecast/solar', ({ request }) => {
    const url = new URL(request.url)
    const date = url.searchParams.get('date') ?? new Date().toISOString().slice(0, 10)
    const [y, mo, d] = date.split('-').map(Number)
    const slots = Array.from({ length: 24 }, (_, h) => {
      const x = (h - 13) / 4
      const power_w = h >= 6 && h <= 20 ? Math.round(3500 * Math.exp(-x * x)) : 0
      const hour_ts = Math.floor(new Date(y, mo - 1, d, h).getTime() / 1000)
      return { hour_ts, power_w }
    })
    return HttpResponse.json({ date, slots })
  }),

  http.post('/api/forecast/solar/fetch', () => {
    const today = new Date().toISOString().slice(0, 10)
    const estimates = []
    for (let h = 6; h <= 20; h++) {
      // Bell curve centred at solar noon (13:00), peak ~3500 W
      const x = (h - 13) / 4
      const watts = Math.max(0, Math.round(3500 * Math.exp(-x * x) + (Math.random() - 0.5) * 80))
      estimates.push({ time: `${String(h).padStart(2, '0')}:00`, watts })
    }
    const total_wh = estimates.reduce((s, e) => s + e.watts, 0)
    return HttpResponse.json({ date: today, estimates, total_wh })
  }),

  powerWs.addEventListener('connection', ({ client }) => {
    console.log('Connected!');

    const interval = setInterval(() => {
      const kw = parseFloat((Math.random() * 1 + 2).toFixed(2))
      console.log("Sending " + kw);

      client.send(JSON.stringify({
        type: 'power_update',
        data: { nodeId: 10000, endpointId: 2, kw },
      }))
    }, 5000)

    // Simulate a device being commissioned 8 seconds after connection
    const commissionTimer = setTimeout(() => {
      client.send(JSON.stringify({
        type: 'device_commissioned',
        data: { nodeId: 30001, vendorName: 'Shelly', productName: 'Pro 1PM' },
      }))
    }, 8000)

    client.addEventListener('close', () => {
      console.log('Closing connection...')
      clearInterval(interval)
      clearTimeout(commissionTimer)
    })
  }),
]
