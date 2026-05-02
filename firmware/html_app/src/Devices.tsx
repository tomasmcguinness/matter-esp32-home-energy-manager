import { useEffect, useState } from 'react'
import { Link } from 'react-router'

export type Endpoint = {
  endpointId: number
  label: string
  included: boolean
  deviceTypes: number[]
}

export type Device = {
  nodeId: number
  vendorName: string
  productName: string
  endpoints: Endpoint[]
}

type DevicesResponse = {
  devices: Device[]
}

function deviceTypeName(id: number): string {
  const names: Record<number, string> = {
    14: 'Aggregator',
    19: 'Root Node',
    256: 'On/Off Light',
    257: 'Dimmable Light',
    266: 'On/Off Plug',
    770: 'Temperature Sensor',
    774: 'Flow Sensor',
    1296: 'Bridged Node',
  }
  return names[id] ?? `0x${id.toString(16).toUpperCase()}`
}

function Devices() {
  const [devices, setDevices] = useState<Device[]>([])
  const [loading, setLoading] = useState(true)
  const [error, setError] = useState<string | null>(null)

  useEffect(() => {
    fetch('/api/devices')
      .then((r) => {
        if (!r.ok) throw new Error(`HTTP ${r.status}`)
        return r.json() as Promise<DevicesResponse>
      })
      .then((data) => {
        setDevices(data.devices)
        setLoading(false)
      })
      .catch((e: unknown) => {
        setError(e instanceof Error ? e.message : 'Failed to load devices')
        setLoading(false)
      })
  }, [])

  if (loading) return <p className="mt-3">Loading…</p>

  return (
    <>
      <div className="mt-3 mb-2">
        <h1 className="mb-0">Devices</h1>
      </div>
      <hr />
      {error && <div className="alert alert-danger">{error}</div>}
      {devices.length === 0 ? (
        <p className="text-muted">No devices in fabric yet.</p>
      ) : (
        <div className="list-group">
          {devices.map((dev) => (
            <div key={dev.nodeId} className="list-group-item">
              <div className="d-flex justify-content-between align-items-start">
                <div>
                  <h6 className="mb-1">
                    {dev.vendorName} {dev.productName}
                  </h6>
                  <small className="text-muted">Node 0x{dev.nodeId.toString(16).toUpperCase()}</small>
                </div>
                <Link
                  to={`/devices/${dev.nodeId}`}
                  className="btn btn-outline-secondary btn-sm"
                >
                  Endpoints
                </Link>
              </div>
              {dev.endpoints.length > 0 && (
                <div className="mt-2">
                  {dev.endpoints.map((ep) => (
                    <span key={ep.endpointId} className="badge bg-secondary me-1">
                      {ep.label || `EP${ep.endpointId}`}
                      {ep.deviceTypes.map((dt) => (
                        <span key={dt} className="ms-1 opacity-75">({deviceTypeName(dt)})</span>
                      ))}
                    </span>
                  ))}
                </div>
              )}
            </div>
          ))}
        </div>
      )}
    </>
  )
}

export default Devices
