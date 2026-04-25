import React from 'react'

export default function ConsumerUnit({ device, selected, onSelect, cardRef }) {
  const circuits = device.circuits ?? []

  return (
    <div
      ref={cardRef}
      className={`device-card consumer-unit ${selected ? 'active' : ''}`}
      onClick={() => onSelect(device.id)}
    >
      <div className="cu-header">⚡ {device.name}</div>
      <div className="cu-circuits">
        {circuits.map((c) => (
          <div key={c.name} className="cu-circuit">
            <span className="cu-circuit-name">{c.name}</span>
            <span className="cu-circuit-val">{c.powerKw.toFixed(1)} kW</span>
            <div className={`cu-breaker ${c.tripped ? 'trip' : ''}`} title={c.tripped ? 'Tripped' : 'On'} />
          </div>
        ))}
      </div>
    </div>
  )
}
