import { useEffect, useState } from 'react'

const ELECTRICAL_SENSOR_DEVICE_TYPE_ID = 0x0510

type EndpointOption = { nodeId: number; endpointId: number; label: string; deviceName: string }
type SensorOption = { nodeId: number; endpointId: number; label: string }

function sensorKey(nodeId: number, endpointId: number) {
  return `${nodeId}-${endpointId}`
}

export type NodeSettings = Record<string, unknown>

type Props = {
  nodeId: string
  // The node's persisted settings object. The settings endpoint replaces it
  // wholesale, so the edit is merged over this and every field is sent back.
  settings: NodeSettings
  // Loads are metered, so they also get the endpoint picker; a sub consumer unit
  // only has a name.
  hasDevice: boolean
  onSave: (settings: NodeSettings) => void
  onCancel: () => void
}

// Edits an existing topology node in place: its name and, for a load, which
// electrical-sensor endpoint meters it. The graph node id, position and edges are
// untouched, so learned data keyed by the node id survives a device swap.
export function EditNodeModal({ nodeId, settings, hasDevice, onSave, onCancel }: Props) {
  const currentNodeId = typeof settings.nodeId === 'number' ? settings.nodeId : undefined
  const currentEndpointId = typeof settings.endpointId === 'number' ? settings.endpointId : undefined
  const currentLabel = typeof settings.label === 'string' ? settings.label : ''
  const current: SensorOption | null = hasDevice && currentNodeId !== undefined && currentEndpointId !== undefined
    ? { nodeId: currentNodeId, endpointId: currentEndpointId, label: currentLabel || `EP${currentEndpointId}` }
    : null

  const [sensors, setSensors] = useState<SensorOption[]>(current ? [current] : [])
  const [selectedKey, setSelectedKey] = useState(current ? sensorKey(current.nodeId, current.endpointId) : '')
  const [name, setName] = useState((typeof settings.name === 'string' && settings.name) || currentLabel)
  const [loading, setLoading] = useState(hasDevice)
  const [saving, setSaving] = useState(false)
  const [error, setError] = useState(false)

  useEffect(() => {
    if (!hasDevice) return
    // The endpoint list excludes endpoints already assigned to a node, which
    // includes this node's own, so the current one is kept at the top.
    fetch(`/api/devices/endpoints?deviceTypeId=${ELECTRICAL_SENSOR_DEVICE_TYPE_ID}`)
      .then(r => r.ok ? r.json() as Promise<{ endpoints: EndpointOption[] }> : Promise.reject())
      .then(data => {
        const unassigned = data.endpoints
          .filter(ep => !current || ep.nodeId !== current.nodeId || ep.endpointId !== current.endpointId)
          .map(ep => ({
            nodeId: ep.nodeId,
            endpointId: ep.endpointId,
            label: ep.label || ep.deviceName || `EP${ep.endpointId}`,
          }))
        setSensors([...(current ? [current] : []), ...unassigned])
      })
      .catch(() => { })
      .finally(() => setLoading(false))
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, [])

  const selected = sensors.find(s => sensorKey(s.nodeId, s.endpointId) === selectedKey) ?? null
  const trimmedName = name.trim()
  const canSave = !loading && !saving && (hasDevice ? selected !== null : trimmedName !== '')

  const handleSave = () => {
    if (!canSave) return
    const next: NodeSettings = hasDevice && selected
      ? { ...settings, name: trimmedName || selected.label, label: selected.label, nodeId: selected.nodeId, endpointId: selected.endpointId }
      : { ...settings, label: trimmedName }

    setSaving(true)
    setError(false)
    fetch(`/api/nodes/${nodeId}/settings`, {
      method: 'PUT',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify(next),
    })
      .then(r => r.ok ? onSave(next) : Promise.reject())
      .catch(() => { setSaving(false); setError(true) })
  }

  return (
    <>
      <div className="modal show d-block" tabIndex={-1} role="dialog">
        <div className="modal-dialog" role="document">
          <div className="modal-content">
            <div className="modal-header">
              <h5 className="modal-title">{hasDevice ? 'Edit Load' : 'Edit Sub Consumer Unit'}</h5>
              <button type="button" className="btn-close" aria-label="Close" onClick={onCancel} />
            </div>
            <div className="modal-body">
              <div className="mb-3">
                <label htmlFor="editNodeName" className="form-label">Name</label>
                <input
                  id="editNodeName"
                  type="text"
                  className="form-control"
                  autoFocus
                  value={name}
                  onChange={e => setName(e.target.value)}
                  onKeyDown={e => { if (e.key === 'Enter') handleSave() }}
                />
              </div>
              {hasDevice && (
                <div className="mb-3">
                  <label htmlFor="editNodeSensor" className="form-label">Electrical Meter Device</label>
                  {loading ? (
                    <p className="text-muted small mb-0">Loading devices...</p>
                  ) : (
                    <select
                      id="editNodeSensor"
                      className="form-select"
                      value={selectedKey}
                      onChange={e => setSelectedKey(e.target.value)}
                    >
                      {!current && <option value="">— Select a device —</option>}
                      {sensors.map(s => (
                        <option key={sensorKey(s.nodeId, s.endpointId)} value={sensorKey(s.nodeId, s.endpointId)}>
                          {s.nodeId}: {s.label} - {s.endpointId}
                        </option>
                      ))}
                    </select>
                  )}
                </div>
              )}
              {error && <p className="text-danger small mb-0">Couldn't save the change. Try again.</p>}
            </div>
            <div className="modal-footer">
              <button type="button" className="btn btn-secondary" onClick={onCancel}>Cancel</button>
              <button type="button" className="btn btn-primary" onClick={handleSave} disabled={!canSave}>
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
