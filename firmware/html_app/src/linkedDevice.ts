// Matter appliance device types an appliance node can be linked to. Their
// Operational State and Mode clusters give the HEM the program and phase a meter
// can't see.
export const DISHWASHER_DEVICE_TYPE = 0x0075
export const LAUNDRY_WASHER_DEVICE_TYPE = 0x0073

// settings.device on an appliance node. The meter stays in settings.nodeId /
// settings.endpointId; this is the optional appliance device beside it.
export type LinkedDevice = { nodeId: number; endpointId: number; deviceType: number; label: string }

// Reads settings.device back from a node's persisted settings.
export function linkedDeviceFromSettings(settings: Record<string, unknown> | undefined): LinkedDevice | null {
  const d = settings?.device as Record<string, unknown> | undefined
  if (!d || typeof d.nodeId !== 'number' || typeof d.endpointId !== 'number') return null
  return {
    nodeId: d.nodeId,
    endpointId: d.endpointId,
    deviceType: typeof d.deviceType === 'number' ? d.deviceType : 0,
    label: typeof d.label === 'string' ? d.label : `EP${d.endpointId}`,
  }
}
