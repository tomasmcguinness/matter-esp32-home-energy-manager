import { http, HttpResponse, ws } from 'msw'
import type { Device } from '../Devices'

export type Settings = {
  name: string
}

let settings: Settings = { name: 'Home Energy Manager' }

type NodeConfig = { id: string; x: number; y: number; settings: Record<string, unknown> }
let nodeConfigs: NodeConfig[] = []

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
  },
  {
    nodeId: 20001,
    vendorName: 'Shelly',
    productName: 'Pro 3EM',
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
    const simpleDevices = devices.flatMap(d => {
      const ep0 = d.endpoints.find(e => e.endpointId === 0)
      const ep0Parts = ep0?.parts ?? []
      const candidates = ep0Parts.length > 0
        ? d.endpoints.filter(e => ep0Parts.includes(e.endpointId))
        : d.endpoints.filter(e => e.endpointId !== 0)
      return candidates.map(ep => {
        const partEps = (ep.parts ?? []).map(id => d.endpoints.find(e => e.endpointId === id)).filter(Boolean)
        const hasPowerMeasurement = [ep, ...partEps].some(e => e?.deviceTypes?.includes(ELECTRICAL_SENSOR_DT))
        return { nodeId: d.nodeId, endpointId: ep.endpointId, label: ep.label || `EP${ep.endpointId}`, hasPowerMeasurement }
      })
    })
    return HttpResponse.json({ devices: simpleDevices })
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
    return HttpResponse.json({ nodes: nodeConfigs })
  }),

  http.put('/api/nodes/:nodeId', async ({ params, request }) => {
    const id = params.nodeId as string
    const body = (await request.json()) as { x: number; y: number }
    const existing = nodeConfigs.find(n => n.id === id)
    if (existing) {
      existing.x = body.x
      existing.y = body.y
    } else {
      nodeConfigs.push({ id, x: body.x, y: body.y, settings: {} })
    }
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
