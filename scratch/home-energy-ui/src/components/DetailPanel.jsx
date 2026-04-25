import React from 'react'
import { DEVICE_TYPES } from '../data/devices'

export default function DetailPanel({ device }) {
  if (!device) return null
  const def = DEVICE_TYPES[device.type] ?? { icon: '❓', label: device.type }
  const meta = device.meta ?? {}

  return (
    <div className="detail-panel visible">
      <div className="detail-title">
        {def.icon} {device.name}
      </div>
      <div className="detail-grid">
        <div className="detail-stat">
          <div className="detail-stat-label">Power Now</div>
          <div className="detail-stat-val">{device.powerKw.toFixed(1)} kW</div>
        </div>
        <div className="detail-stat">
          <div className="detail-stat-label">Status</div>
          <div className="detail-stat-val">{device.online ? device.status : 'Offline'}</div>
        </div>
        {Object.entries(meta).map(([k, v]) => (
          <div key={k} className="detail-stat">
            <div className="detail-stat-label">{k}</div>
            <div className="detail-stat-val">{v}</div>
          </div>
        ))}
      </div>
    </div>
  )
}
