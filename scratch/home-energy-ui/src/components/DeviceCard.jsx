import React from 'react'
import { DEVICE_TYPES } from '../data/devices'

export default function DeviceCard({ device, selected, onSelect, cardRef }) {
  const def = DEVICE_TYPES[device.type] ?? { icon: '❓', colorClass: 'appliance' }

  const statusClass = device.online
    ? device.status === 'Charging'  ? 'status-charging'
    : device.status === 'Exporting' ? 'status-exporting'
    : 'status-on'
    : 'status-off'

  return (
    <div
      ref={cardRef}
      className={`device-card ${def.colorClass} ${selected ? 'active' : ''} ${!device.online ? 'offline' : ''}`}
      onClick={() => onSelect(device.id)}
    >
      <div className={`device-icon icon-${def.colorClass}`}>{def.icon}</div>
      <div className="device-name">{device.name}</div>
      <div className="device-power">
        {device.powerKw.toFixed(1)} <span>kW</span>
      </div>
      <div className={`device-status ${statusClass}`}>
        {device.online ? device.status : 'Offline'}
      </div>
    </div>
  )
}
