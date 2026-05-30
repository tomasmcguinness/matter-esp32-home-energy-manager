import { useEffect, useState } from 'react'

const ELECTRICAL_SENSOR_DEVICE_TYPE_ID = 0x0510

type EndpointOption = { nodeId: number; endpointId: number; label: string; deviceName: string }

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
    fetch(`/api/devices/endpoints?deviceTypeId=${ELECTRICAL_SENSOR_DEVICE_TYPE_ID}`)
      .then(r => r.ok ? r.json() as Promise<{ endpoints: EndpointOption[] }> : Promise.reject())
      .then(data => {
        const options: SensorOption[] = data.endpoints.map(ep => ({
          nodeId: ep.nodeId,
          endpointId: ep.endpointId,
          label: ep.label || ep.deviceName || `EP${ep.endpointId}`,
        }))
        setSensors(options)
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
                <label htmlFor="gridSensor" className="form-label">Electrical Meter Device</label>
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
                    No Electrical Meter endpoints found. Commission a device first.
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
