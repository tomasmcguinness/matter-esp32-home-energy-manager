/**
 * Device type definitions.
 *
 * Each Matter device you commission maps to one of these types.
 * The `role` field controls where the device appears in the UI:
 *   - 'source'    → top row (grid, solar, battery)
 *   - 'hub'       → middle row (consumer unit)
 *   - 'appliance' → bottom row (loads)
 */
export const DEVICE_TYPES = {
  GRID:          { role: 'source',    icon: '🔌', label: 'Grid',            colorClass: 'grid' },
  SOLAR_INVERTER:{ role: 'source',    icon: '☀️',  label: 'Solar Inverter',  colorClass: 'solar' },
  BATTERY:       { role: 'source',    icon: '🔋', label: 'Battery',         colorClass: 'battery' },
  CONSUMER_UNIT: { role: 'hub',       icon: '⚡', label: 'Consumer Unit',   colorClass: 'cu' },
  LIGHTING:      { role: 'appliance', icon: '💡', label: 'Lighting',        colorClass: 'appliance' },
  OVEN:          { role: 'appliance', icon: '🍳', label: 'Oven / Hob',      colorClass: 'appliance' },
  WASHING:       { role: 'appliance', icon: '🫧', label: 'Washing Machine', colorClass: 'appliance' },
  EV_CHARGER:    { role: 'appliance', icon: '🚗', label: 'EV Charger',      colorClass: 'ev' },
  HEAT_PUMP:     { role: 'appliance', icon: '🌡️', label: 'Heat Pump',       colorClass: 'appliance' },
  HOT_WATER:     { role: 'appliance', icon: '🚿', label: 'Hot Water',       colorClass: 'appliance' },
}

/**
 * Initial device registry.
 *
 * When a new Matter device is commissioned, add an entry here (or push
 * to this array dynamically from your Matter commissioning callback).
 *
 * Each device has:
 *   id          — unique string identifier (e.g. Matter node ID)
 *   type        — key from DEVICE_TYPES above
 *   name        — display name (overrides the type default)
 *   powerKw     — current power reading in kW (updated by telemetry)
 *   status      — short status string shown on the card
 *   online      — whether the device is reachable
 *   meta        — arbitrary extra data shown in the detail panel
 */
export const initialDevices = [
  {
    id: 'grid-1',
    type: 'GRID',
    name: 'Grid Connection',
    powerKw: 0.0,
    status: 'Exporting',
    online: true,
    meta: {
      Voltage: '240 V',
      Frequency: '50.01 Hz',
      'Today Exported': '8.4 kWh',
      'Today Imported': '0.0 kWh',
      Tariff: 'Octopus Flux',
      'Export Rate': '15 p/kWh',
      'Import Rate': '28 p/kWh',
    },
  },
  {
    id: 'solar-1',
    type: 'SOLAR_INVERTER',
    name: 'Solar Inverter',
    powerKw: 3.8,
    status: 'Generating',
    online: true,
    meta: {
      'DC Input': '4.1 kW',
      Efficiency: '92.7%',
      Today: '18.2 kWh',
      'This Month': '312 kWh',
      Panels: '12 × 380W',
      Orientation: 'South 15°',
      Temp: '42°C',
    },
  },
  {
    id: 'battery-1',
    type: 'BATTERY',
    name: 'Battery',
    powerKw: 0.4,
    status: 'Charging',
    online: true,
    meta: {
      'State of Charge': '78%',
      'Charging At': '0.4 kW',
      Capacity: '10 kWh',
      Usable: '9.2 kWh',
      Cycles: '247',
      Health: '96%',
      'Est. Full': '2h 15m',
      Mode: 'Auto',
    },
  },
  {
    id: 'cu-1',
    type: 'CONSUMER_UNIT',
    name: 'Consumer Unit',
    powerKw: 2.6,
    status: 'Active',
    online: true,
    circuits: [
      { name: 'Lighting',   powerKw: 0.2, tripped: false },
      { name: 'Ring Main',  powerKw: 1.1, tripped: false },
      { name: 'Kitchen',    powerKw: 0.8, tripped: false },
      { name: 'EV Charger', powerKw: 0.5, tripped: false },
      { name: 'Immersion',  powerKw: 0.0, tripped: true  },
    ],
    meta: {
      'Total Load': '2.6 kW',
      'Main Fuse': '100A',
      Phase: 'Single',
      'Earth Type': 'TN-S',
      Installed: '2021',
      'Next Service': '2026',
    },
  },
  {
    id: 'lights-1',
    type: 'LIGHTING',
    name: 'Lighting',
    powerKw: 0.2,
    status: 'On',
    online: true,
    meta: { 'Rooms On': '4 of 8', Bulbs: 'LED × 24', Today: '0.8 kWh' },
  },
  {
    id: 'oven-1',
    type: 'OVEN',
    name: 'Oven / Hob',
    powerKw: 1.8,
    status: 'On',
    online: true,
    meta: { 'Hob Zones': '2 active', 'Set Temp': '200°C', Running: '38 min' },
  },
  {
    id: 'washing-1',
    type: 'WASHING',
    name: 'Washing Machine',
    powerKw: 0.6,
    status: 'Running',
    online: true,
    meta: { Programme: '40°C Cotton', Remaining: '22 min', Today: '0.9 kWh' },
  },
  {
    id: 'ev-1',
    type: 'EV_CHARGER',
    name: 'EV Charger',
    powerKw: 0.0,
    status: 'Standby',
    online: false,
    meta: { 'Max Rate': '7.4 kW', Today: '0.0 kWh', Connector: 'Type 2' },
  },
]
