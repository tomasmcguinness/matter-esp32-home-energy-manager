import React, { useState, useRef, useEffect, useCallback } from 'react'
import { initialDevices, DEVICE_TYPES } from './data/devices'
import DeviceCard from './components/DeviceCard'
import ConsumerUnit from './components/ConsumerUnit'
import SummaryBar from './components/SummaryBar'
import FlowCanvas from './components/FlowCanvas'
import DetailPanel from './components/DetailPanel'

export default function App() {
  const [devices, setDevices] = useState(initialDevices)
  const [selectedId, setSelectedId] = useState(null)

  // Refs for SVG wire drawing — one per device id
  const cardRefs = useRef({})

  function getRef(id) {
    if (!cardRefs.current[id]) cardRefs.current[id] = React.createRef()
    return cardRefs.current[id]
  }

  // Categorise devices by role
  const sourceDevices    = devices.filter(d => DEVICE_TYPES[d.type]?.role === 'source')
  const hubDevice        = devices.find(d  => DEVICE_TYPES[d.type]?.role === 'hub')
  const applianceDevices = devices.filter(d => DEVICE_TYPES[d.type]?.role === 'appliance')

  // Build ref maps for FlowCanvas
  const sourceRefs    = Object.fromEntries(sourceDevices.map(d => [d.id, getRef(d.id)]))
  const applianceRefs = Object.fromEntries(applianceDevices.map(d => [d.id, getRef(d.id)]))
  const hubRef        = hubDevice ? getRef(hubDevice.id) : { current: null }

  // Toggle selection
  function handleSelect(id) {
    setSelectedId(prev => prev === id ? null : id)
  }

  const selectedDevice = devices.find(d => d.id === selectedId) ?? null

  // Live telemetry simulation — replace with your Matter data source
  useEffect(() => {
    const interval = setInterval(() => {
      setDevices(prev => prev.map(d => {
        if (!d.online) return d
        // Small jitter on power readings to simulate live data
        const jitter = (v, range) => Math.max(0, v + (Math.random() - 0.5) * range)
        switch (d.type) {
          case 'SOLAR_INVERTER': return { ...d, powerKw: +jitter(3.8, 0.25).toFixed(2) }
          case 'BATTERY':        return { ...d, powerKw: +jitter(0.4, 0.1).toFixed(2) }
          case 'OVEN':           return { ...d, powerKw: +jitter(1.8, 0.15).toFixed(2) }
          default:               return d
        }
      }))
    }, 2500)
    return () => clearInterval(interval)
  }, [])

  /**
   * commissonDevice — call this from your Matter commissioning callback
   * to add a newly paired device to the UI.
   *
   * Example:
   *   commissionDevice({
   *     id: 'matter-node-0x0003',
   *     type: 'HEAT_PUMP',
   *     name: 'Daikin Heat Pump',
   *     powerKw: 0,
   *     status: 'Idle',
   *     online: true,
   *     meta: { Model: 'FTXM50R', 'Serial No': '123456' },
   *   })
   */
  const commissionDevice = useCallback((device) => {
    setDevices(prev => {
      const exists = prev.find(d => d.id === device.id)
      if (exists) {
        // Update existing — handles re-commissioning
        return prev.map(d => d.id === device.id ? { ...d, ...device } : d)
      }
      return [...prev, device]
    })
  }, [])

  // Expose for debugging / integration testing from the browser console
  useEffect(() => { window.__commissionDevice = commissionDevice }, [commissionDevice])

  return (
    <div className="app">
      <header className="header">
        <div>
          <div className="header-title">🏠 Home Energy Monitor</div>
          <div className="header-sub">Matter Home Energy Manager — {devices.filter(d => d.online).length} devices online</div>
        </div>
        <div className="live-badge">
          <div className="live-dot" />
          LIVE
        </div>
      </header>

      <SummaryBar devices={devices} />

      {/* Main diagram */}
      <div className="canvas">
        <FlowCanvas
          sourceRefs={sourceRefs}
          hubRef={hubRef}
          applianceRefs={applianceRefs}
          sourceDevices={sourceDevices}
          applianceDevices={applianceDevices}
        />

        {/* Row 1 — Sources */}
        <div className="row row-1">
          {sourceDevices.map(d => (
            <DeviceCard
              key={d.id}
              device={d}
              selected={selectedId === d.id}
              onSelect={handleSelect}
              cardRef={getRef(d.id)}
            />
          ))}
        </div>

        {/* Row 2 — Consumer Unit */}
        {hubDevice && (
          <div className="row row-2">
            <ConsumerUnit
              device={hubDevice}
              selected={selectedId === hubDevice.id}
              onSelect={handleSelect}
              cardRef={getRef(hubDevice.id)}
            />
          </div>
        )}

        {/* Row 3 — Appliances */}
        <div className="row row-3">
          {applianceDevices.map(d => (
            <DeviceCard
              key={d.id}
              device={d}
              selected={selectedId === d.id}
              onSelect={handleSelect}
              cardRef={getRef(d.id)}
            />
          ))}
        </div>
      </div>

      {/* Detail panel */}
      <DetailPanel device={selectedDevice} />
    </div>
  )
}
