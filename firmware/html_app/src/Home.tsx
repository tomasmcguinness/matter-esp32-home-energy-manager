import { useEffect, useState } from 'react'

type SimpleDevice = { nodeId: number; endpointId: number; label: string; hasElectricalSensor: boolean; hasSolarPower: boolean }

type SlotProps = {
  label: string
  description: string
  onConfigure?: () => void
}

function UnconfiguredSlot({ label, description, onConfigure }: SlotProps) {
  const [hovered, setHovered] = useState(false)
  return (
    <div
      onMouseEnter={() => setHovered(true)}
      onMouseLeave={() => setHovered(false)}
      onClick={onConfigure}
      style={{
        flex: 1,
        border: `2px dashed ${hovered ? '#64748b' : '#cbd5e1'}`,
        borderRadius: 10,
        padding: '20px 16px',
        display: 'flex',
        flexDirection: 'column',
        alignItems: 'center',
        gap: 8,
        cursor: onConfigure ? 'pointer' : 'default',
        background: hovered ? '#f8fafc' : '#fff',
        transition: 'border-color .15s, background .15s',
        minWidth: 0,
      }}
    >
      <div style={{ fontWeight: 600, fontSize: 14, color: '#1e293b' }}>{label}</div>
      <div style={{ fontSize: 12, color: '#94a3b8', textAlign: 'center', lineHeight: 1.4 }}>{description}</div>
      {onConfigure && (
        <div style={{
          marginTop: 8,
          fontSize: 11,
          fontWeight: 600,
          textTransform: 'uppercase',
          letterSpacing: '.06em',
          color: hovered ? '#3b82f6' : '#cbd5e1',
          transition: 'color .15s',
        }}>
          + Configure
        </div>
      )}
    </div>
  )
}

function TrashIcon() {
  return (
    <svg width="13" height="13" viewBox="0 0 24 24" fill="none" stroke="currentColor" strokeWidth="2" strokeLinecap="round" strokeLinejoin="round">
      <polyline points="3 6 5 6 21 6" />
      <path d="M19 6l-1 14a2 2 0 01-2 2H8a2 2 0 01-2-2L5 6" />
      <path d="M10 11v6M14 11v6" />
      <path d="M9 6V4a1 1 0 011-1h4a1 1 0 011 1v2" />
    </svg>
  )
}

function ConfiguredSlot({ label, device, onReconfigure, onDelete }: { label: string; device: SimpleDevice; onReconfigure: () => void; onDelete: () => void }) {
  const [hovered, setHovered] = useState(false)
  const [trashHovered, setTrashHovered] = useState(false)
  return (
    <div
      onMouseEnter={() => setHovered(true)}
      onMouseLeave={() => setHovered(false)}
      onClick={onReconfigure}
      style={{
        flex: 1,
        position: 'relative',
        border: `2px solid ${hovered ? '#3b82f6' : '#86efac'}`,
        borderRadius: 10,
        padding: '20px 16px',
        display: 'flex',
        flexDirection: 'column',
        alignItems: 'center',
        gap: 6,
        cursor: 'pointer',
        background: '#f0fdf4',
        transition: 'border-color .15s',
        minWidth: 0,
      }}
    >
      <button
        onClick={e => { e.stopPropagation(); onDelete() }}
        onMouseEnter={() => setTrashHovered(true)}
        onMouseLeave={() => setTrashHovered(false)}
        title="Remove"
        style={{
          position: 'absolute',
          top: 8,
          right: 8,
          padding: 4,
          border: 'none',
          borderRadius: 4,
          background: 'none',
          cursor: 'pointer',
          color: trashHovered ? '#ef4444' : '#cbd5e1',
          transition: 'color .12s',
          display: 'flex',
          alignItems: 'center',
          justifyContent: 'center',
        }}
      >
        <TrashIcon />
      </button>
      <div style={{ fontSize: 10, fontWeight: 700, textTransform: 'uppercase', letterSpacing: '.08em', color: '#16a34a' }}>{label}</div>
      <div style={{ fontWeight: 600, fontSize: 14, color: '#1e293b', textAlign: 'center' }}>{device.label}</div>
      <div style={{ fontSize: 12, color: '#64748b' }}>
        Node 0x{device.nodeId.toString(16).toUpperCase()} &middot; EP {device.endpointId}
      </div>
      <div style={{ marginTop: 4, fontSize: 11, fontWeight: 600, textTransform: 'uppercase', letterSpacing: '.06em', color: hovered ? '#3b82f6' : '#94a3b8', transition: 'color .15s' }}>
        ✎ Change
      </div>
    </div>
  )
}

function GridSensorModal({ initialSelected, onSave, onClose }: { initialSelected: SimpleDevice | null; onSave: (device: SimpleDevice) => void; onClose: () => void }) {
  const [devices, setDevices] = useState<SimpleDevice[]>([])
  const [selected, setSelected] = useState<SimpleDevice | null>(initialSelected)
  const [loading, setLoading] = useState(true)
  const [saving, setSaving] = useState(false)
  const [error, setError] = useState<string | null>(null)

  useEffect(() => {
    fetch('/api/devices/simple')
      .then(r => r.ok ? r.json() : Promise.reject(`HTTP ${r.status}`))
      .then((data: { devices: SimpleDevice[] }) => {
        setDevices(data.devices.filter(d => d.hasElectricalSensor))
        setLoading(false)
      })
      .catch((e: unknown) => {
        setError(e instanceof Error ? e.message : String(e))
        setLoading(false)
      })
  }, [])

  function handleSave() {
    if (!selected) return
    setSaving(true)
    fetch('/api/topology/grid', {
      method: 'PUT',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify({ nodeId: selected.nodeId, endpointId: selected.endpointId, label: selected.label }),
    })
      .then(r => r.ok ? r.json() : Promise.reject(`HTTP ${r.status}`))
      .then(() => onSave(selected))
      .catch((e: unknown) => {
        setError(e instanceof Error ? e.message : String(e))
        setSaving(false)
      })
  }

  return (
    <div style={{ position: 'fixed', inset: 0, background: 'rgba(0,0,0,0.4)', zIndex: 200, display: 'flex', alignItems: 'center', justifyContent: 'center' }}>
      <div style={{ background: '#fff', borderRadius: 12, padding: 24, width: 380, maxWidth: '90vw', boxShadow: '0 8px 32px rgba(0,0,0,0.18)' }}>
        <h2 style={{ margin: '0 0 4px', fontSize: 16, fontWeight: 700, color: '#1e293b' }}>Select Grid Sensor</h2>
        <p style={{ margin: '0 0 16px', fontSize: 13, color: '#64748b' }}>Choose the device that measures power at the grid connection.</p>
        {error && <div style={{ marginBottom: 12, padding: '8px 12px', background: '#fef2f2', border: '1px solid #fca5a5', borderRadius: 6, fontSize: 13, color: '#b91c1c' }}>{error}</div>}
        {loading && <p style={{ fontSize: 13, color: '#94a3b8' }}>Loading devices…</p>}
        {!loading && devices.length === 0 && <p style={{ fontSize: 13, color: '#94a3b8' }}>No devices with power measurement found.</p>}
        {!loading && devices.length > 0 && (
          <div style={{ display: 'flex', flexDirection: 'column', gap: 8, marginBottom: 20 }}>
            {devices.map(d => {
              const isSelected = selected?.nodeId === d.nodeId && selected?.endpointId === d.endpointId
              return (
                <div
                  key={`${d.nodeId}-${d.endpointId}`}
                  onClick={() => setSelected(d)}
                  style={{
                    padding: '10px 14px', borderRadius: 8, cursor: 'pointer',
                    border: `2px solid ${isSelected ? '#3b82f6' : '#e2e8f0'}`,
                    background: isSelected ? '#eff6ff' : '#fff',
                    transition: 'border-color .12s, background .12s',
                  }}
                >
                  <div style={{ fontWeight: 600, fontSize: 14, color: '#1e293b' }}>{d.label}</div>
                  <div style={{ fontSize: 12, color: '#94a3b8', marginTop: 2 }}>
                    Node 0x{d.nodeId.toString(16).toUpperCase()} &middot; EP {d.endpointId}
                  </div>
                </div>
              )
            })}
          </div>
        )}
        <div style={{ display: 'flex', gap: 8, justifyContent: 'flex-end' }}>
          <button onClick={onClose} style={{ padding: '8px 16px', borderRadius: 6, border: '1px solid #e2e8f0', background: '#fff', fontSize: 13, cursor: 'pointer' }}>Cancel</button>
          <button
            onClick={handleSave}
            disabled={!selected || saving}
            style={{ padding: '8px 16px', borderRadius: 6, border: 'none', background: selected ? '#3b82f6' : '#cbd5e1', color: '#fff', fontSize: 13, fontWeight: 600, cursor: selected ? 'pointer' : 'not-allowed' }}
          >
            {saving ? 'Saving…' : 'Save'}
          </button>
        </div>
      </div>
    </div>
  )
}

function SolarInverterModal({ initialSelected, onSave, onClose }: { initialSelected: SimpleDevice | null; onSave: (device: SimpleDevice) => void; onClose: () => void }) {
  const [devices, setDevices] = useState<SimpleDevice[]>([])
  const [selected, setSelected] = useState<SimpleDevice | null>(initialSelected)
  const [loading, setLoading] = useState(true)
  const [saving, setSaving] = useState(false)
  const [error, setError] = useState<string | null>(null)

  useEffect(() => {
    fetch('/api/devices/simple')
      .then(r => r.ok ? r.json() : Promise.reject(`HTTP ${r.status}`))
      .then((data: { devices: SimpleDevice[] }) => {
        setDevices(data.devices.filter(d => d.hasSolarPower))
        setLoading(false)
      })
      .catch((e: unknown) => {
        setError(e instanceof Error ? e.message : String(e))
        setLoading(false)
      })
  }, [])

  function handleSave() {
    if (!selected) return
    setSaving(true)
    fetch('/api/topology/solar', {
      method: 'PUT',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify({ nodeId: selected.nodeId, endpointId: selected.endpointId, label: selected.label }),
    })
      .then(r => r.ok ? r.json() : Promise.reject(`HTTP ${r.status}`))
      .then(() => onSave(selected))
      .catch((e: unknown) => {
        setError(e instanceof Error ? e.message : String(e))
        setSaving(false)
      })
  }

  return (
    <div style={{ position: 'fixed', inset: 0, background: 'rgba(0,0,0,0.4)', zIndex: 200, display: 'flex', alignItems: 'center', justifyContent: 'center' }}>
      <div style={{ background: '#fff', borderRadius: 12, padding: 24, width: 380, maxWidth: '90vw', boxShadow: '0 8px 32px rgba(0,0,0,0.18)' }}>
        <h2 style={{ margin: '0 0 4px', fontSize: 16, fontWeight: 700, color: '#1e293b' }}>Select Solar Inverter</h2>
        <p style={{ margin: '0 0 16px', fontSize: 13, color: '#64748b' }}>Choose the device that measures power generated by your solar panels.</p>
        {error && <div style={{ marginBottom: 12, padding: '8px 12px', background: '#fef2f2', border: '1px solid #fca5a5', borderRadius: 6, fontSize: 13, color: '#b91c1c' }}>{error}</div>}
        {loading && <p style={{ fontSize: 13, color: '#94a3b8' }}>Loading devices…</p>}
        {!loading && devices.length === 0 && <p style={{ fontSize: 13, color: '#94a3b8' }}>No solar power devices found.</p>}
        {!loading && devices.length > 0 && (
          <div style={{ display: 'flex', flexDirection: 'column', gap: 8, marginBottom: 20 }}>
            {devices.map(d => {
              const isSelected = selected?.nodeId === d.nodeId && selected?.endpointId === d.endpointId
              return (
                <div
                  key={`${d.nodeId}-${d.endpointId}`}
                  onClick={() => setSelected(d)}
                  style={{
                    padding: '10px 14px', borderRadius: 8, cursor: 'pointer',
                    border: `2px solid ${isSelected ? '#3b82f6' : '#e2e8f0'}`,
                    background: isSelected ? '#eff6ff' : '#fff',
                    transition: 'border-color .12s, background .12s',
                  }}
                >
                  <div style={{ fontWeight: 600, fontSize: 14, color: '#1e293b' }}>{d.label}</div>
                  <div style={{ fontSize: 12, color: '#94a3b8', marginTop: 2 }}>
                    Node 0x{d.nodeId.toString(16).toUpperCase()} &middot; EP {d.endpointId}
                  </div>
                </div>
              )
            })}
          </div>
        )}
        <div style={{ display: 'flex', gap: 8, justifyContent: 'flex-end' }}>
          <button onClick={onClose} style={{ padding: '8px 16px', borderRadius: 6, border: '1px solid #e2e8f0', background: '#fff', fontSize: 13, cursor: 'pointer' }}>Cancel</button>
          <button
            onClick={handleSave}
            disabled={!selected || saving}
            style={{ padding: '8px 16px', borderRadius: 6, border: 'none', background: selected ? '#3b82f6' : '#cbd5e1', color: '#fff', fontSize: 13, fontWeight: 600, cursor: selected ? 'pointer' : 'not-allowed' }}
          >
            {saving ? 'Saving…' : 'Save'}
          </button>
        </div>
      </div>
    </div>
  )
}

function SectionLabel({ children }: { children: string }) {
  return (
    <div style={{
      fontSize: 10,
      fontWeight: 700,
      textTransform: 'uppercase',
      letterSpacing: '.1em',
      color: '#94a3b8',
      marginBottom: 12,
    }}>
      {children}
    </div>
  )
}

function FlowArrow() {
  return (
    <div style={{ display: 'flex', justifyContent: 'center', alignItems: 'center', height: 36 }}>
      <div style={{ display: 'flex', flexDirection: 'column', alignItems: 'center', gap: 0 }}>
        <div style={{ width: 2, height: 20, background: '#cbd5e1' }} />
        <div style={{
          width: 0,
          height: 0,
          borderLeft: '6px solid transparent',
          borderRight: '6px solid transparent',
          borderTop: '8px solid #cbd5e1',
        }} />
      </div>
    </div>
  )
}

const APPLIANCE_SLOTS = [
  'Appliance 1',
  'Appliance 2',
  'Appliance 3',
  'Appliance 4',
  'Appliance 5',
]

type SavedNode = { id: string; settings?: { label?: string; nodeId?: number; endpointId?: number } }

function Home() {
  const [gridModalOpen, setGridModalOpen] = useState(false)
  const [gridSensor, setGridSensor] = useState<SimpleDevice | null>(null)
  const [solarModalOpen, setSolarModalOpen] = useState(false)
  const [solarInverter, setSolarInverter] = useState<SimpleDevice | null>(null)

  useEffect(() => {
    fetch('/api/nodes')
      .then(r => r.ok ? r.json() : Promise.reject())
      .then((data: { nodes: SavedNode[] }) => {
        const gm = data.nodes.find(n => n.id === 'grid_meter')
        if (gm?.settings?.nodeId !== undefined && gm.settings.endpointId !== undefined && gm.settings.label) {
          setGridSensor({ nodeId: gm.settings.nodeId as number, endpointId: gm.settings.endpointId as number, label: gm.settings.label as string, hasElectricalSensor: true, hasSolarPower: false })
        }
        const si = data.nodes.find(n => n.id === 'solar_inverter')
        if (si?.settings?.nodeId !== undefined && si.settings.endpointId !== undefined && si.settings.label) {
          setSolarInverter({ nodeId: si.settings.nodeId as number, endpointId: si.settings.endpointId as number, label: si.settings.label as string, hasElectricalSensor: false, hasSolarPower: true })
        }
      })
      .catch(() => { })
  }, [])

  function handleGridSave(device: SimpleDevice) {
    setGridSensor(device)
    setGridModalOpen(false)
  }

  function handleSolarSave(device: SimpleDevice) {
    setSolarInverter(device)
    setSolarModalOpen(false)
  }

  function handleGridDelete() {
    fetch('/api/edges/grid_meter-power-out-consumer_unit-grid', { method: 'DELETE' })
      .then(() => fetch('/api/nodes/grid_meter', { method: 'DELETE' }))
      .then(() => setGridSensor(null))
      .catch(() => { })
  }

  function handleSolarDelete() {
    fetch('/api/edges/solar_inverter-power-out-consumer_unit-solar_input', { method: 'DELETE' })
      .then(() => fetch('/api/nodes/solar_inverter', { method: 'DELETE' }))
      .then(() => setSolarInverter(null))
      .catch(() => { })
  }

  return (
    <div style={{ padding: '32px 40px', maxWidth: 900, margin: '0 auto' }}>
      {gridModalOpen && (
        <GridSensorModal
          initialSelected={gridSensor}
          onSave={handleGridSave}
          onClose={() => setGridModalOpen(false)}
        />
      )}
      {solarModalOpen && (
        <SolarInverterModal
          initialSelected={solarInverter}
          onSave={handleSolarSave}
          onClose={() => setSolarModalOpen(false)}
        />
      )}

      {/* Inputs */}
      <section>
        <SectionLabel>Inputs</SectionLabel>
        <div style={{ display: 'flex', gap: 16 }}>
          {gridSensor ? (
            <ConfiguredSlot label="Grid" device={gridSensor} onReconfigure={() => setGridModalOpen(true)} onDelete={handleGridDelete} />
          ) : (
            <UnconfiguredSlot
              label="Grid"
              description="Device measuring power imported from or exported to the grid"
              onConfigure={() => setGridModalOpen(true)}
            />
          )}
          {solarInverter ? (
            <ConfiguredSlot label="Solar Inverter" device={solarInverter} onReconfigure={() => setSolarModalOpen(true)} onDelete={handleSolarDelete} />
          ) : (
            <UnconfiguredSlot
              label="Solar Inverter"
              description="Device measuring power generated by your solar panels"
              onConfigure={() => setSolarModalOpen(true)}
            />
          )}
        </div>
      </section>


      <FlowArrow />

      {/* Consumer Unit */}
      <section style={{ display: 'flex', justifyContent: 'center' }}>
        <div style={{
          border: '2px solid #1e293b',
          borderRadius: 10,
          padding: '16px 48px',
          textAlign: 'center',
          background: '#fff',
          minWidth: 200,
        }}>
          <div style={{ fontWeight: 700, fontSize: 14, color: '#1e293b' }}>Consumer Unit</div>
          <div style={{ fontSize: 12, color: '#64748b', marginTop: 4 }}>Main distribution board</div>
        </div>
      </section>

      <FlowArrow />

      {/* Loads */}
      <section>
        <SectionLabel>Loads</SectionLabel>
        <div style={{ display: 'flex', gap: 12 }}>
          {APPLIANCE_SLOTS.map(name => (
            <UnconfiguredSlot
              key={name}
              label={name}
              description="Assign a device to monitor this load"
            />
          ))}
        </div>
      </section>

    </div>
  )
}

export default Home
