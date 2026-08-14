import { useEffect, useState } from 'react'

// The learned per-appliance profile served by GET /api/appliance/profiles. When
// `trained` is false only graph_id + trained are present (no runs analysed yet).
type ApplianceProfile = {
  graph_id: string
  trained: boolean
  standby_w?: number
  avg_program_power_w?: number
  std_program_power_w?: number
  avg_program_len_min?: number
  std_program_len_min?: number
  program_count?: number
  days_with_data?: number
  trained_unix?: number
  window_days?: number
}
type ProfilesResponse = { appliances: ApplianceProfile[] }

type NodeConfig = { id: string; settings?: { name?: string; label?: string; nodeId?: number } }
type NodesResponse = { nodes: NodeConfig[] }

// One entry per commissioned Matter device; hasDem is true when any of its
// endpoints implements the Device Energy Management cluster.
type SimpleDevice = { nodeId: number; hasDem?: boolean }
type SimpleDevicesResponse = { devices: SimpleDevice[] }

type Appliance = ApplianceProfile & { name: string; dem: boolean }

function formatPower(w: number) {
  return w >= 1000 ? `${(w / 1000).toFixed(2)} kW` : `${Math.round(w)} W`
}

function formatLength(min: number) {
  if (min >= 60) {
    const h = Math.floor(min / 60)
    const m = min % 60
    return m > 0 ? `${h}h ${m}m` : `${h}h`
  }
  return `${min} min`
}

// --- Cycle sketch -----------------------------------------------------------
// A stylised standby -> program -> standby curve drawn from the profile scalars
// (there is no per-cycle shape stored). Block height encodes average program
// power, width is a fixed central fraction (length is shown as a label), and the
// lighter band over the block top is the ±stddev of power.
const PAD = { top: 20, right: 16, bottom: 28, left: 52 }
const SVG_W = 480
const SVG_H = 150
const PLOT_W = SVG_W - PAD.left - PAD.right
const PLOT_H = SVG_H - PAD.top - PAD.bottom

function CycleSketch({ a }: { a: Appliance }) {
  const standby = a.standby_w ?? 0
  const power = a.avg_program_power_w ?? 0
  const stdP = a.std_program_power_w ?? 0
  const len = a.avg_program_len_min ?? 0
  const stdL = a.std_program_len_min ?? 0

  const maxScale = Math.max(power + stdP, power, 1) * 1.1
  const yOf = (w: number) =>
    PAD.top + PLOT_H - (Math.min(Math.max(w, 0), maxScale) / maxScale) * PLOT_H

  const bottomY = PAD.top + PLOT_H
  const standbyY = yOf(standby)
  const programY = yOf(power)
  const bandTopY = yOf(power + stdP)
  const bandBotY = yOf(power - stdP)

  const blockX0 = PAD.left + PLOT_W * 0.2
  const blockX1 = PAD.left + PLOT_W * 0.8
  const blockW = blockX1 - blockX0

  return (
    <svg viewBox={`0 0 ${SVG_W} ${SVG_H}`} style={{ width: '100%', maxWidth: SVG_W, fontFamily: 'inherit' }}>
      {/* y-axis */}
      <text x={PAD.left - 8} y={PAD.top + 4} textAnchor="end" fontSize={10} fill="#94a3b8">
        {formatPower(Math.round(maxScale))}
      </text>
      <text x={PAD.left - 8} y={bottomY} textAnchor="end" fontSize={10} fill="#94a3b8">0</text>

      {/* program block */}
      <rect x={blockX0} y={programY} width={blockW} height={bottomY - programY}
        fill="#10b981" rx={2} opacity={0.85} />
      {/* ±stddev band on the power axis */}
      {stdP > 0 && (
        <rect x={blockX0} y={bandTopY} width={blockW} height={Math.max(bandBotY - bandTopY, 0)}
          fill="#10b981" opacity={0.18} />
      )}

      {/* standby shelves + baseline */}
      <line x1={PAD.left} y1={standbyY} x2={blockX0} y2={standbyY} stroke="#0f766e" strokeWidth={2} />
      <line x1={blockX1} y1={standbyY} x2={PAD.left + PLOT_W} y2={standbyY} stroke="#0f766e" strokeWidth={2} />
      <line x1={PAD.left} y1={bottomY} x2={PAD.left + PLOT_W} y2={bottomY} stroke="#e2e8f0" strokeWidth={1} />

      {/* labels */}
      <text x={(blockX0 + blockX1) / 2} y={programY - 6} textAnchor="middle" fontSize={11} fill="#0f766e" fontWeight={600}>
        {formatPower(power)}{stdP > 0 ? ` ±${formatPower(stdP)}` : ''}
      </text>
      <text x={(blockX0 + blockX1) / 2} y={bottomY + 16} textAnchor="middle" fontSize={10} fill="#94a3b8">
        {formatLength(len)}{stdL > 0 ? ` ±${stdL} min` : ''}
      </text>
      <text x={PAD.left} y={standbyY - 4} fontSize={10} fill="#64748b">
        standby {formatPower(standby)}
      </text>
    </svg>
  )
}

// --- Stat card --------------------------------------------------------------
function Stat({ label, value, unit }: { label: string; value: string; unit?: string }) {
  return (
    <div style={{ flex: 1, background: '#f8fafc', border: '1px solid #e2e8f0', borderRadius: 10, padding: '14px 18px' }}>
      <div style={{ fontSize: 11, fontWeight: 700, textTransform: 'uppercase', letterSpacing: '.08em', color: '#475569', marginBottom: 4 }}>{label}</div>
      <div style={{ fontSize: 22, fontWeight: 700, color: '#1e293b' }}>
        {value}{unit && <span style={{ fontSize: 13, fontWeight: 400, color: '#64748b' }}> {unit}</span>}
      </div>
    </div>
  )
}

function ApplianceCard({ a }: { a: Appliance }) {
  const learned = a.trained && (a.program_count ?? 0) > 0
  return (
    <div style={{ border: '1px solid #e2e8f0', borderRadius: 10, padding: '18px 20px' }}>
      <div style={{ display: 'flex', alignItems: 'center', justifyContent: 'space-between', marginBottom: learned ? 16 : 8 }}>
        <h2 style={{ margin: 0, fontSize: 17, fontWeight: 700, color: '#1e293b' }}>{a.name}</h2>
        <div style={{ display: 'flex', alignItems: 'center', gap: 6 }}>
          {a.dem && (
            <span style={{
              fontSize: 11, fontWeight: 700, textTransform: 'uppercase', letterSpacing: '.06em',
              padding: '3px 10px', borderRadius: 999,
              background: '#eff6ff', color: '#1e40af', border: '1px solid #bfdbfe',
            }} title="Supports the Matter Device Energy Management cluster">
              DEM
            </span>
          )}
          <span style={{
            fontSize: 11, fontWeight: 700, textTransform: 'uppercase', letterSpacing: '.06em',
            padding: '3px 10px', borderRadius: 999,
            background: learned ? '#f0fdf4' : '#fffbeb',
            color: learned ? '#166534' : '#92400e',
            border: `1px solid ${learned ? '#86efac' : '#fde68a'}`,
          }}>
            {learned ? 'Trained' : 'Learning'}
          </span>
        </div>
      </div>

      {!learned ? (
        <p style={{ margin: 0, fontSize: 13, color: '#94a3b8' }}>
          No complete runs observed yet
          {a.trained && a.standby_w !== undefined ? ` — standby ~${formatPower(a.standby_w)}` : ''}.
          Profile sharpens as the appliance runs over the coming days.
        </p>
      ) : (
        <>
          <div style={{ display: 'flex', gap: 12, marginBottom: 16, flexWrap: 'wrap' }}>
            <Stat label="Standby" value={formatPower(a.standby_w ?? 0)} />
            <Stat label="Avg power" value={formatPower(a.avg_program_power_w ?? 0)} />
            <Stat label="Avg length" value={formatLength(a.avg_program_len_min ?? 0)} />
            <Stat label="Runs" value={String(a.program_count ?? 0)} />
          </div>

          <div style={{ background: '#fff', border: '1px solid #e2e8f0', borderRadius: 10, padding: '12px 16px', marginBottom: 12 }}>
            <CycleSketch a={a} />
          </div>

          <div style={{ fontSize: 12, color: '#94a3b8' }}>
            ± {formatPower(a.std_program_power_w ?? 0)} power · ± {a.std_program_len_min ?? 0} min length
            {a.days_with_data !== undefined ? ` · ${a.days_with_data} days with data` : ''}
            {a.window_days !== undefined ? ` · ${a.window_days}-day window` : ''}
            {a.trained_unix ? ` · updated ${new Date(a.trained_unix * 1000).toLocaleString()}` : ''}
          </div>
        </>
      )}
    </div>
  )
}

function Appliances() {
  const [appliances, setAppliances] = useState<Appliance[]>([])
  const [loading, setLoading] = useState(true)
  const [error, setError] = useState<string | null>(null)
  const [training, setTraining] = useState(false)

  function load() {
    setLoading(true)
    setError(null)
    Promise.all([
      fetch('/api/appliance/profiles').then(r =>
        r.ok ? (r.json() as Promise<ProfilesResponse>) : r.text().then(t => Promise.reject(t))),
      fetch('/api/nodes').then(r =>
        r.ok ? (r.json() as Promise<NodesResponse>) : r.text().then(t => Promise.reject(t))),
      fetch('/api/devices/simple').then(r =>
        r.ok ? (r.json() as Promise<SimpleDevicesResponse>) : r.text().then(t => Promise.reject(t))),
    ])
      .then(([profiles, topology, devices]) => {
        const names = new Map<string, string>()
        const nodeMatterId = new Map<string, number>()
        for (const n of topology.nodes) {
          names.set(n.id, n.settings?.name || n.settings?.label || n.id)
          if (n.settings?.nodeId !== undefined) nodeMatterId.set(n.id, n.settings.nodeId)
        }
        const demByMatterId = new Map<number, boolean>()
        for (const d of devices.devices) {
          if (d.hasDem) demByMatterId.set(d.nodeId, true)
        }
        setAppliances(profiles.appliances.map(p => ({
          ...p,
          name: names.get(p.graph_id) || p.graph_id,
          dem: demByMatterId.get(nodeMatterId.get(p.graph_id) ?? -1) ?? false,
        })))
        setLoading(false)
      })
      .catch((e: unknown) => { setError(String(e)); setLoading(false) })
  }

  useEffect(() => {
    load()
  }, [])

  function reanalyse() {
    setTraining(true)
    fetch('/api/test/appliance-profiles/train', { method: 'POST' })
      .then(r => r.ok ? r.json() : r.text().then(t => Promise.reject(t)))
      .then(() => { setTraining(false); load() })
      .catch((e: unknown) => { setError(String(e)); setTraining(false) })
  }

  return (
    <div>
      <div style={{ display: 'flex', alignItems: 'center', justifyContent: 'space-between' }}>
        <h1 className="mb-0">Appliances</h1>
        <button
          onClick={reanalyse}
          disabled={training || loading}
          style={{
            padding: '9px 18px', borderRadius: 8, border: 'none',
            background: training || loading ? '#cbd5e1' : '#10b981',
            color: '#fff', fontSize: 13, fontWeight: 600,
            cursor: training || loading ? 'not-allowed' : 'pointer', transition: 'background .15s',
          }}
        >
          {training ? 'Analysing…' : 'Re-analyse now'}
        </button>
      </div>
      <hr />
      <p style={{ margin: '0 0 20px', fontSize: 13, color: '#64748b' }}>
        Usage profiles learned nightly from each appliance's power history — standby draw,
        and the typical power and length of a run.
      </p>

      {error && (
        <div style={{ marginBottom: 20, padding: '10px 14px', background: '#fef2f2', border: '1px solid #fca5a5', borderRadius: 8, fontSize: 13, color: '#b91c1c' }}>
          {error}
        </div>
      )}

      {loading ? (
        <p className="mt-3">Loading…</p>
      ) : appliances.length === 0 ? (
        <div style={{ padding: '48px 0', textAlign: 'center', color: '#94a3b8', fontSize: 13 }}>
          No appliances configured yet. Wire an appliance to a consumer-unit circuit in Topology.
        </div>
      ) : (
        <div className="d-flex flex-column gap-3">
          {appliances.map(a => <ApplianceCard key={a.graph_id} a={a} />)}
        </div>
      )}
    </div>
  )
}

export default Appliances
