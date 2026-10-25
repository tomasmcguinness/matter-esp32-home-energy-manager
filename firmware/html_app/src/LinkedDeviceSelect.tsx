import { useEffect, useState, type CSSProperties } from 'react'
import { deviceTypeName } from './deviceTypeName'
import { DISHWASHER_DEVICE_TYPE, LAUNDRY_WASHER_DEVICE_TYPE, type LinkedDevice } from './linkedDevice'

type EndpointOption = { nodeId: number; endpointId: number; label: string; deviceName: string; deviceType?: number }

const key = (d: { nodeId: number; endpointId: number }) => `${d.nodeId}-${d.endpointId}`

// A select of commissioned dishwasher / washing machine endpoints, with "None".
// Endpoints already linked to another appliance are left out by the API; the
// current link is kept at the top.
export function LinkedDeviceSelect({ id, value, onChange, className, style }: {
  id?: string
  value: LinkedDevice | null
  onChange: (device: LinkedDevice | null) => void
  className?: string
  style?: CSSProperties
}) {
  const [options, setOptions] = useState<LinkedDevice[]>(value ? [value] : [])
  const [loading, setLoading] = useState(true)

  useEffect(() => {
    fetch(`/api/devices/endpoints?deviceTypeId=${DISHWASHER_DEVICE_TYPE},${LAUNDRY_WASHER_DEVICE_TYPE}&role=device`)
      .then(r => r.ok ? r.json() as Promise<{ endpoints: EndpointOption[] }> : Promise.reject())
      .then(data => {
        const others = data.endpoints
          .filter(ep => !value || key(ep) !== key(value))
          .map(ep => ({
            nodeId: ep.nodeId,
            endpointId: ep.endpointId,
            deviceType: ep.deviceType ?? 0,
            label: ep.label || ep.deviceName || `EP${ep.endpointId}`,
          }))
        setOptions([...(value ? [value] : []), ...others])
      })
      .catch(() => { })
      .finally(() => setLoading(false))
    // Only the link the modal opened with is pinned to the top.
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, [])

  if (loading) return <p style={{ fontSize: 13, color: '#94a3b8', margin: 0 }}>Loading appliances…</p>

  return (
    <select
      id={id}
      className={className}
      style={style}
      value={value ? key(value) : ''}
      onChange={e => onChange(options.find(o => key(o) === e.target.value) ?? null)}
    >
      <option value="">None</option>
      {options.map(o => (
        <option key={key(o)} value={key(o)}>
          {o.label} ({deviceTypeName(o.deviceType)}) — Node 0x{o.nodeId.toString(16).toUpperCase()} · EP {o.endpointId}
        </option>
      ))}
    </select>
  )
}
