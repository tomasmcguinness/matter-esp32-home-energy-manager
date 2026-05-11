import { useEffect, useState } from 'react'
import { deviceTypeName } from './deviceTypeName'

export type Endpoint = {
  endpointId: number
  label: string
  included: boolean
  deviceTypes: number[]
  parts: number[]
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

const LINE = '#000000'
const TRUNK_X = 9   // x-offset of trunk within the connector column
const CONN_W = 26   // total width of the connector column

function epColor(id: number) {
  return id === 0
    ? { header: '#b5651d', border: '#b5651d' }
    : { header: '#1a6ec8', border: '#1a6ec8' }
}

function epLabel(ep: Endpoint): string {
  if (ep.label) return ep.label
  if (ep.deviceTypes.length > 0) return deviceTypeName(ep.deviceTypes[0])
  return `EP${ep.endpointId}`
}

function EndpointBox({ ep }: { ep: Endpoint }) {
  const { header, border } = epColor(ep.endpointId)
  return (
    <div style={{ border: `1px solid ${border}`, borderRadius: 4, overflow: 'hidden' }}>
      <div style={{
        background: header, color: '#fff',
        padding: '3px 10px', fontWeight: 600,
        fontSize: '0.82rem', whiteSpace: 'nowrap',
      }}>
        EP{ep.endpointId}: {epLabel(ep)}
      </div>
      {ep.deviceTypes.length > 0 && (
        <div style={{ display: 'flex', alignItems: 'center', flexWrap: 'wrap', gap: 4, padding: '3px 10px' }}>
          <span style={{ fontSize: '0.73rem', color: '#666', whiteSpace: 'nowrap' }}>DeviceTypeList</span>
          {ep.deviceTypes.map(dt => (
            <span key={dt} style={{
              background: '#2e7d32', color: '#fff',
              borderRadius: 10, padding: '1px 7px',
              fontSize: '0.71rem', whiteSpace: 'nowrap',
            }}>
              {deviceTypeName(dt)}
            </span>
          ))}
        </div>
      )}
    </div>
  )
}

function EndpointRow({
  ep, epMap, isLast, depth,
}: {
  ep: Endpoint
  epMap: Map<number, Endpoint>
  isLast: boolean
  depth: number
}) {
  const allChildren = (ep.parts ?? []).map(id => epMap.get(id)).filter((e): e is Endpoint => e !== undefined)
  const claimedBySibling = new Set(allChildren.flatMap(c => c.parts ?? []))
  const children = allChildren.filter(c => !claimedBySibling.has(c.endpointId))

  return (
    <div>
      {/* This endpoint's own row */}
      <div style={{ display: 'flex', alignItems: 'stretch', marginBottom: children.length > 0 ? 4 : (isLast ? 0 : 8) }}>
        {depth > 0 && (
          <div style={{ width: CONN_W, flexShrink: 0, position: 'relative', marginRight: 6 }}>
            <div style={{ position: 'absolute', left: TRUNK_X, top: 0, height: 'calc(50%)', width: 2, background: LINE }} />
            <div style={{ position: 'absolute', left: TRUNK_X, top: 'calc(50% - 1px)', right: 0, height: 2, background: LINE }} />
            {!isLast && (
              <div style={{ position: 'absolute', left: TRUNK_X, top: 'calc(50%)', bottom: 0, width: 2, background: LINE }} />
            )}
          </div>
        )}
        <EndpointBox ep={ep} />
      </div>

      {/* Children indented under this endpoint */}
      {children.length > 0 && (
        <div style={{
          marginLeft: depth > 0 ? CONN_W + 6 + TRUNK_X + 2 : TRUNK_X + 2,
          paddingLeft: 0,
          marginBottom: isLast ? 0 : 8,
        }}>
          {children.map((child, idx) => (
            <EndpointRow
              key={child.endpointId}
              ep={child}
              epMap={epMap}
              isLast={idx === children.length - 1}
              depth={depth + 1}
            />
          ))}
        </div>
      )}
    </div>
  )
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

  function handleReinterrogate(nodeId: number) {
    fetch(`/api/devices/${nodeId}/interrogate`, { method: 'POST' })
      .then((r) => { if (!r.ok) throw new Error(`HTTP ${r.status}`) })
  }

  function handleDelete(nodeId: number) {
    if (!window.confirm('Remove this device from the fabric and delete it from storage?')) return
    fetch(`/api/devices/${nodeId}`, { method: 'DELETE' })
      .then((r) => { if (!r.ok) throw new Error(`HTTP ${r.status}`) })
      .then(() => setDevices((prev) => prev.filter((d) => d.nodeId !== nodeId)))
      .catch((e: unknown) => setError(e instanceof Error ? e.message : 'Delete failed'))
  }

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
        <div className="d-flex flex-column gap-3">
          {devices.map((dev) => {
            const name = [dev.vendorName, dev.productName].filter(Boolean).join(' ')
            const nodeHex = `Node 0x${dev.nodeId.toString(16).toUpperCase()}`
            return (
              <div key={dev.nodeId} style={{ border: '1px solid #dee2e6', borderRadius: 6, overflow: 'hidden' }}>
                <div style={{ background: '#f8f9fa', borderBottom: '1px solid #dee2e6', padding: '8px 12px' }}>
                  <strong>{name || nodeHex}</strong>
                  {name && <span className="text-muted ms-2" style={{ fontSize: '0.8rem' }}>{nodeHex}</span>}
                </div>
                {dev.endpoints.length > 0 && (() => {
                  const epMap = new Map(dev.endpoints.map(e => [e.endpointId, e]))
                  const childIds = new Set(dev.endpoints.flatMap(e => e.parts ?? []))
                  const roots = dev.endpoints.filter(e => !childIds.has(e.endpointId))
                  return (
                    <div style={{ padding: '10px 12px' }}>
                      {roots.map((ep, idx) => (
                        <EndpointRow
                          key={ep.endpointId}
                          ep={ep}
                          epMap={epMap}
                          isLast={idx === roots.length - 1}
                          depth={0}
                        />
                      ))}
                    </div>
                  )
                })()}
                <div style={{ padding: '8px 12px', borderTop: '1px solid #dee2e6' }} className="d-flex gap-2">
                  <button className="btn btn-primary btn-sm" onClick={() => handleReinterrogate(dev.nodeId)}>
                    Re-interrogate
                  </button>
                  <button className="btn btn-danger btn-sm" onClick={() => handleDelete(dev.nodeId)}>
                    Delete
                  </button>
                </div>
              </div>
            )
          })}
        </div>
      )}
    </>
  )
}

export default Devices
