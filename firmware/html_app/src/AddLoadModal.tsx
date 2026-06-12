import { useEffect, useState } from 'react'

const ELECTRICAL_SENSOR_DEVICE_TYPE_ID = 0x0510

type EndpointOption = { nodeId: number; endpointId: number; label: string; deviceName: string }
type SensorOption = { nodeId: number; endpointId: number; label: string }

function sensorKey(nodeId: number, endpointId: number) {
  return `${nodeId}-${endpointId}`
}

export type AddedLoad = { nodeId: number; endpointId: number; label: string; name: string }

type Props = {
  onAdd: (load: AddedLoad) => void
  onCancel: () => void
}

// Picks a commissioned electrical-sensor endpoint to drop on the canvas as a
// metered load. Mirrors GridModal's endpoint picker; on Save it hands the choice
// back to the caller, which creates and persists the node (and its edge).
export function AddLoadModal({ onAdd, onCancel }: Props) {
  const [sensors, setSensors] = useState<SensorOption[]>([])
  const [selectedKey, setSelectedKey] = useState('')
  const [name, setName] = useState('')
  // Track manual edits so picking a device can re-seed the default name without
  // clobbering one the user typed.
  const [nameTouched, setNameTouched] = useState(false)
  const [loading, setLoading] = useState(true)

  useEffect(() => {
    fetch(`/api/devices/endpoints?deviceTypeId=${ELECTRICAL_SENSOR_DEVICE_TYPE_ID}`)
      .then(r => r.ok ? r.json() as Promise<{ endpoints: EndpointOption[] }> : Promise.reject())
      .then(data => {
        setSensors(data.endpoints.map(ep => ({
          nodeId: ep.nodeId,
          endpointId: ep.endpointId,
          label: ep.label || ep.deviceName || `EP${ep.endpointId}`,
        })))
      })
      .catch(() => { })
      .finally(() => setLoading(false))
  }, [])

  const selected = sensors.find(s => sensorKey(s.nodeId, s.endpointId) === selectedKey) ?? null

  const handleSelect = (key: string) => {
    setSelectedKey(key)
    const s = sensors.find(x => sensorKey(x.nodeId, x.endpointId) === key)
    if (s && !nameTouched) setName(s.label)
  }

  const handleSave = () => {
    if (!selected) return
    onAdd({
      nodeId: selected.nodeId,
      endpointId: selected.endpointId,
      label: selected.label,
      name: name.trim() || selected.label,
    })
  }

  return (
    <>
      <div className="modal show d-block" tabIndex={-1} role="dialog">
        <div className="modal-dialog" role="document">
          <div className="modal-content">
            <div className="modal-header">
              <h5 className="modal-title">Add Load</h5>
              <button type="button" className="btn-close" aria-label="Close" onClick={onCancel} />
            </div>
            <div className="modal-body">
              <div className="mb-3">
                <label htmlFor="loadName" className="form-label">Name</label>
                <input
                  id="loadName"
                  type="text"
                  className="form-control"
                  placeholder="e.g. Immersion Heater"
                  value={name}
                  onChange={e => { setName(e.target.value); setNameTouched(true) }}
                />
              </div>
              <div className="mb-3">
                <label htmlFor="loadSensor" className="form-label">Electrical Meter Device</label>
                {loading ? (
                  <p className="text-muted small mb-0">Loading devices...</p>
                ) : (
                  <select
                    id="loadSensor"
                    className="form-select"
                    value={selectedKey}
                    onChange={e => handleSelect(e.target.value)}
                  >
                    <option value="">— Select a device —</option>
                    {sensors.map(s => (
                      <option key={sensorKey(s.nodeId, s.endpointId)} value={sensorKey(s.nodeId, s.endpointId)}>
                        {s.nodeId}: {s.label} - {s.endpointId}
                      </option>
                    ))}
                  </select>
                )}
                {!loading && sensors.length === 0 && (
                  <p className="form-text text-muted">
                    No unassigned Electrical Meter endpoints found. Commission a device first.
                  </p>
                )}
              </div>
            </div>
            <div className="modal-footer">
              <button type="button" className="btn btn-secondary" onClick={onCancel}>Cancel</button>
              <button type="button" className="btn btn-primary" onClick={handleSave} disabled={loading || !selected}>
                Add
              </button>
            </div>
          </div>
        </div>
      </div>
      <div className="modal-backdrop show" />
    </>
  )
}
