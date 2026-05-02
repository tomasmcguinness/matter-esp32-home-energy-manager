import { useEffect, useState } from 'react'
import { useParams, Link } from 'react-router'
import type { Device, Endpoint } from './Devices'

type DevicesResponse = { devices: Device[] }

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

function DeviceEndpoints() {
  const { nodeId } = useParams<{ nodeId: string }>()
  const [device, setDevice] = useState<Device | null>(null)
  const [loading, setLoading] = useState(true)
  const [error, setError] = useState<string | null>(null)

  useEffect(() => {
    fetch('/api/devices')
      .then((r) => {
        if (!r.ok) throw new Error(`HTTP ${r.status}`)
        return r.json() as Promise<DevicesResponse>
      })
      .then((data) => {
        const found = data.devices.find((d) => String(d.nodeId) === nodeId)
        setDevice(found ?? null)
        setLoading(false)
      })
      .catch((e: unknown) => {
        setError(e instanceof Error ? e.message : 'Failed to load devices')
        setLoading(false)
      })
  }, [nodeId])

  function toggleIncluded(ep: Endpoint) {
    if (!device) return
    const newIncluded = !ep.included
    setDevice((prev) => {
      if (!prev) return prev
      return {
        ...prev,
        endpoints: prev.endpoints.map((e) =>
          e.endpointId === ep.endpointId ? { ...e, included: newIncluded } : e
        ),
      }
    })
    fetch(`/api/devices/${device.nodeId}/endpoints/${ep.endpointId}`, {
      method: 'PUT',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify({ included: newIncluded }),
    }).catch(() => {
      setDevice((prev) => {
        if (!prev) return prev
        return {
          ...prev,
          endpoints: prev.endpoints.map((e) =>
            e.endpointId === ep.endpointId ? { ...e, included: ep.included } : e
          ),
        }
      })
    })
  }

  if (loading) return <p className="mt-3">Loading…</p>

  if (!device) {
    return (
      <>
        <h1 className="mt-3">Device not found</h1>
        <Link to="/devices" className="btn btn-outline-secondary mt-2">Back to Devices</Link>
      </>
    )
  }

  return (
    <>
      <div className="d-flex align-items-center gap-3 mt-3 mb-2">
        <Link to="/devices" className="btn btn-outline-secondary btn-sm">← Back</Link>
        <h1 className="mb-0">{device.vendorName} {device.productName}</h1>
      </div>
      <p className="text-muted">Node 0x{device.nodeId.toString(16).toUpperCase()}</p>
      <hr />
      {error && <div className="alert alert-danger">{error}</div>}
      {device.endpoints.length === 0 ? (
        <p className="text-muted">No endpoints discovered for this device.</p>
      ) : (
        <table className="table table-hover">
          <thead>
            <tr>
              <th>EP</th>
              <th>Label</th>
              <th>Device Types</th>
              <th>Include</th>
            </tr>
          </thead>
          <tbody>
            {device.endpoints.map((ep) => (
              <tr key={ep.endpointId}>
                <td className="font-monospace">{ep.endpointId}</td>
                <td>{ep.label || <span className="text-muted">—</span>}</td>
                <td>
                  {ep.deviceTypes.map((dt) => (
                    <span key={dt} className="badge bg-secondary me-1">{deviceTypeName(dt)}</span>
                  ))}
                </td>
                <td>
                  <div className="form-check form-switch mb-0">
                    <input
                      className="form-check-input"
                      type="checkbox"
                      role="switch"
                      checked={ep.included}
                      onChange={() => toggleIncluded(ep)}
                      id={`ep-${ep.endpointId}`}
                    />
                  </div>
                </td>
              </tr>
            ))}
          </tbody>
        </table>
      )}
    </>
  )
}

export default DeviceEndpoints
