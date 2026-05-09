import { useEffect, useState } from 'react'
import type { Device } from './Devices'

//const ELECTRICAL_SENSOR_DEVICE_TYPE_ID = 0x0510
const METER_REFERENCE_POINT_DEVICE_TYPE_ID = 0x0512

type NodeConfig = { id: string; x: number; y: number; settings: Record<string, unknown> }

type SensorOption = { nodeId: number; endpointId: number; label: string }

function sensorKey(nodeId: number, endpointId: number) {
  return `${nodeId}-${endpointId}`
}

type Props = {
  onSave: () => void
  onCancel: () => void
}

export function GridModal({ onSave, onCancel }: Props) {
  const [sensors, setSensors] = useState<SensorOption[]>([])
  const [selectedKey, setSelectedKey] = useState('')
  const [loading, setLoading] = useState(true)
  const [saving, setSaving] = useState(false)

  useEffect(() => {
    Promise.all([
      fetch('/api/devices').then(r => r.ok ? r.json() as Promise<{ devices: Device[] }> : Promise.reject()),
      fetch('/api/nodes').then(r => r.ok ? r.json() as Promise<{ nodes: NodeConfig[] }> : Promise.reject()),
    ])
      .then(([devicesData, nodesData]) => {
        const options: SensorOption[] = []
        for (const dev of devicesData.devices) {
          for (const ep of dev.endpoints) {
            if (ep.deviceTypes.includes(METER_REFERENCE_POINT_DEVICE_TYPE_ID)) {
              const label = ep.label.trim() || `${dev.vendorName} ${dev.productName} EP${ep.endpointId}`
              options.push({ nodeId: dev.nodeId, endpointId: ep.endpointId, label })
            }
          }
        }
        setSensors(options)

        const gridNode = nodesData.nodes.find(n => n.id === 'meter')
        const nid = gridNode?.settings?.gridSensorNodeId
        const eid = gridNode?.settings?.gridSensorEndpointId
        if (typeof nid === 'number' && typeof eid === 'number' && nid !== 0) {
          setSelectedKey(sensorKey(nid, eid))
        }
      })
      .catch(() => {})
      .finally(() => setLoading(false))
  }, [])

  const handleSave = () => {
    setSaving(true)
    let nodeId = 0, endpointId = 0
    if (selectedKey) {
      const [n, e] = selectedKey.split('-').map(Number)
      nodeId = n
      endpointId = e
    }
    fetch('/api/nodes/grid/settings', {
      method: 'PUT',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify({ gridSensorNodeId: nodeId, gridSensorEndpointId: endpointId }),
    })
      .then(() => onSave())
      .catch(() => setSaving(false))
  }

  return (
    <>
      <div className="modal show d-block" tabIndex={-1} role="dialog">
        <div className="modal-dialog" role="document">
          <div className="modal-content">
            <div className="modal-header">
              <h5 className="modal-title">Grid Connection</h5>
              <button type="button" className="btn-close" aria-label="Close" onClick={onCancel} />
            </div>
            <div className="modal-body">
              <div className="mb-3">
                <label htmlFor="gridSensor" className="form-label">Electrial Meter Device</label>
                {loading ? (
                  <p className="text-muted small mb-0">Loading devices...</p>
                ) : (
                  <select
                    id="gridSensor"
                    className="form-select"
                    value={selectedKey}
                    onChange={e => setSelectedKey(e.target.value)}
                  >
                    <option value="">— None —</option>
                    {sensors.map(s => (
                      <option key={sensorKey(s.nodeId, s.endpointId)} value={sensorKey(s.nodeId, s.endpointId)}>
                        {s.label}
                      </option>
                    ))}
                  </select>
                )}
                {!loading && sensors.length === 0 && (
                  <p className="form-text text-muted">
                    No Electrical Sensor endpoints found. Commission a device first.
                  </p>
                )}
              </div>
            </div>
            <div className="modal-footer">
              <button type="button" className="btn btn-secondary" onClick={onCancel}>Cancel</button>
              <button type="button" className="btn btn-primary" onClick={handleSave} disabled={saving || loading}>
                {saving ? 'Saving…' : 'Save'}
              </button>
            </div>
          </div>
        </div>
      </div>
      <div className="modal-backdrop show" />
    </>
  )
}
