import { http, HttpResponse } from 'msw'
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
      { endpointId: 1, label: 'Solax Inverter',   included: false, deviceTypes: [0x0017] },
      { endpointId: 2, label: 'FeedIn CT Clamp',  included: false, deviceTypes: [0x0510] },
    ],
  },
  {
    nodeId: 20001,
    vendorName: 'Shelly',
    productName: 'Pro 3EM',
    endpoints: [
      { endpointId: 1, label: 'Grid Meter',        included: true,  deviceTypes: [1296] },
      { endpointId: 2, label: 'Solar Feed',         included: false, deviceTypes: [1296] },
    ],
  },
]

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
]
