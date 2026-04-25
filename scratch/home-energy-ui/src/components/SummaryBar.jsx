import React from 'react'

function SumCard({ label, value, colorClass, sub }) {
  return (
    <div className="sum-card">
      <div className="sum-label">{label}</div>
      <div className={`sum-val ${colorClass}`}>{value}</div>
      {sub && <div className="sum-sub">{sub}</div>}
    </div>
  )
}

export default function SummaryBar({ devices }) {
  const solar   = devices.find(d => d.type === 'SOLAR_INVERTER')
  const battery = devices.find(d => d.type === 'BATTERY')
  const grid    = devices.find(d => d.type === 'GRID')

  const solarKw   = solar?.powerKw ?? 0
  const battKw    = battery?.powerKw ?? 0
  const battMeta  = battery?.meta ?? {}
  const gridKw    = grid?.powerKw ?? 0

  const homeKw = devices
    .filter(d => DEVICE_TYPES_ROLE[d.type] === 'appliance')
    .reduce((sum, d) => sum + (d.powerKw ?? 0), 0)

  const exportKw = Math.max(0, solarKw - homeKw - battKw).toFixed(1)

  return (
    <div className="summary-bar">
      <SumCard label="Solar generating" value={`${solarKw.toFixed(1)} kW`}  colorClass="yellow" sub={`Battery: ${battMeta['State of Charge'] ?? '—'}`} />
      <SumCard label="Home consuming"   value={`${homeKw.toFixed(1)} kW`}   colorClass="blue"   sub="Total appliance load" />
      <SumCard label="Grid export"      value={`${exportKw} kW`}            colorClass="green"  sub={gridKw > 0 ? `Importing ${gridKw.toFixed(1)} kW` : 'No import'} />
      <SumCard label="Battery"          value={battMeta['State of Charge'] ?? '—'} colorClass="purple" sub={`${battery?.status ?? '—'} · ${battKw.toFixed(1)} kW`} />
    </div>
  )
}

// Small local map — avoids a circular import from App
const DEVICE_TYPES_ROLE = {
  GRID: 'source', SOLAR_INVERTER: 'source', BATTERY: 'source',
  CONSUMER_UNIT: 'hub',
  LIGHTING: 'appliance', OVEN: 'appliance', WASHING: 'appliance',
  EV_CHARGER: 'appliance', HEAT_PUMP: 'appliance', HOT_WATER: 'appliance',
}
