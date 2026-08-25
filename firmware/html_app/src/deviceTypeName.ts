const DEVICE_TYPE_NAMES: Record<number, string> = {
  0x000e: 'Aggregator',
  0x0011: 'Power Source',
  0x0012: 'OTA Requestor',
  0x0013: 'Bridged Node',
  0x0016: 'Root Node',
  0x0017: 'Solar Power',
  0x0100: 'On/Off Light',
  0x0101: 'Dimmable Light',
  0x010a: 'On/Off Plug',
  0x0302: 'Temperature Sensor',
  0x0306: 'Flow Sensor',
  0x0510: 'Electrical Sensor',
  0x0512: 'Meter Reference Point',
  0x0514: 'Electrical Meter',
}

export function deviceTypeName(id: number): string {
  return DEVICE_TYPE_NAMES[id] ?? `0x${id.toString(16).toUpperCase()}`
}
