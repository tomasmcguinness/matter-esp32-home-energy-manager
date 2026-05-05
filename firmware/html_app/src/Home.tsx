import { useEffect, useState } from 'react'
import { Link } from 'react-router'
import type { Device } from './Devices'

const DEVICE_TYPE_ICONS: Record<number, string> = {
  14:   '🔗',
  19:   '🏠',
  256:  '💡',
  257:  '💡',
  266:  '🔌',
  770:  '🌡️',
  774:  '💧',
  1296: '🔗',
}

const DEVICE_TYPE_NAMES: Record<number, string> = {
  14:   'Aggregator',
  19:   'Root Node',
  256:  'Light',
  257:  'Dimmable Light',
  266:  'Smart Plug',
  770:  'Temperature Sensor',
  774:  'Flow Sensor',
  1296: 'Bridged Node',
}

function primaryDeviceType(dev: Device): number | null {
  for (const ep of dev.endpoints) {
    for (const dt of ep.deviceTypes) {
      if (dt !== 19 && dt !== 14 && dt !== 1296) return dt
    }
  }
  return dev.endpoints[0]?.deviceTypes[0] ?? null
}

function iconFor(dev: Device): string {
  const dt = primaryDeviceType(dev)
  return dt !== null ? (DEVICE_TYPE_ICONS[dt] ?? '⚙️') : '⚙️'
}

function typeNameFor(dev: Device): string {
  const dt = primaryDeviceType(dev)
  return dt !== null ? (DEVICE_TYPE_NAMES[dt] ?? `0x${dt.toString(16).toUpperCase()}`) : 'Unknown'
}

const DOT_GRID_BG: React.CSSProperties = {
  position: 'fixed',
  inset: 0,
  zIndex: -1,
  backgroundImage: 'radial-gradient(circle, #cbd5e1 1px, transparent 1px)',
  backgroundSize: '24px 24px',
}

const GRID: React.CSSProperties = {
  display: 'grid',
  gridTemplateColumns: 'repeat(auto-fill, minmax(160px, 1fr))',
  gap: '1rem',
}

const CARD: React.CSSProperties = {
  borderRadius: '1rem',
  padding: '1.25rem 1rem',
  textAlign: 'center',
  textDecoration: 'none',
  color: 'inherit',
  background: '#fff',
  boxShadow: '0 1px 4px rgba(0,0,0,0.08)',
  transition: 'box-shadow 0.15s, transform 0.15s',
  display: 'block',
}

function Home() {
  const [devices, setDevices] = useState<Device[]>([])
  const [loading, setLoading] = useState(true)
  const [error, setError] = useState<string | null>(null)

  useEffect(() => {
    fetch('/api/devices')
      .then((r) => {
        if (!r.ok) throw new Error(`HTTP ${r.status}`)
        return r.json() as Promise<{ devices: Device[] }>
      })
      .then((data) => { setDevices(data.devices); setLoading(false) })
      .catch((e: unknown) => {
        setError(e instanceof Error ? e.message : 'Failed to load devices')
        setLoading(false)
      })
  }, [])

  if (loading) return <p className="mt-4">Loading…</p>
  if (error) return <div className="alert alert-danger mt-4">{error}</div>

  return (
    <>
      <div style={DOT_GRID_BG} />
      <div style={{ padding: '1.5rem 0' }}>
      {devices.length === 0 ? (
        <p className="text-muted">No devices in fabric yet. Commission a device from Settings.</p>
      ) : (
        <div style={GRID}>
          {devices.map((dev) => (
            <Link
              key={dev.nodeId}
              to={`/devices/${dev.nodeId}`}
              style={CARD}
              onMouseEnter={(e) => {
                const el = e.currentTarget
                el.style.boxShadow = '0 4px 12px rgba(0,0,0,0.15)'
                el.style.transform = 'translateY(-2px)'
              }}
              onMouseLeave={(e) => {
                const el = e.currentTarget
                el.style.boxShadow = '0 1px 4px rgba(0,0,0,0.08)'
                el.style.transform = 'translateY(0)'
              }}
            >
              <div style={{ fontSize: '2.5rem', lineHeight: 1 }}>{iconFor(dev)}</div>
              <div className="fw-semibold mt-2" style={{ fontSize: '0.9rem' }}>
                {dev.productName || dev.vendorName || 'Unknown'}
              </div>
              <div className="text-muted" style={{ fontSize: '0.75rem' }}>{typeNameFor(dev)}</div>
              <div className="text-muted mt-1" style={{ fontSize: '0.65rem', opacity: 0.5 }}>
                {dev.nodeId.toString(16).toUpperCase().padStart(4, '0')}
              </div>
            </Link>
          ))}
        </div>
      )}
      </div>
    </>
  )
}

export default Home
