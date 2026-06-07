import { useEffect, useState } from 'react'
import { useSearchParams } from 'react-router'

type PowerRecord = { minute: number; power_w: number }

const PAD = { top: 16, right: 16, bottom: 36, left: 56 }
const SVG_W = 800
const SVG_H = 240
const PLOT_W = SVG_W - PAD.left - PAD.right
const PLOT_H = SVG_H - PAD.top - PAD.bottom

function niceExtent(value: number): number {
  if (value === 0) return 0
  const abs = Math.abs(value)
  const step = abs <= 2000 ? 500 : abs <= 5000 ? 1000 : 2000
  return Math.sign(value) * Math.ceil(abs / step) * step
}

function fmtW(w: number): string {
  const abs = Math.abs(w)
  const sign = w < 0 ? '−' : ''
  return abs >= 1000 ? `${sign}${(abs / 1000).toFixed(abs % 1000 ? 1 : 0)}kW` : `${sign}${abs}W`
}

// Records are logged once a minute; anything more than this between consecutive
// samples means at least one minute is missing — i.e. a "no data" gap.
const GAP_THRESHOLD_S = 90

// The time window the chart spans: local midnight of the day, to the end of the
// day — clamped to "now" for today, since the remaining hours are the future,
// not missing data.
function dayBounds(date: string): { start: number; end: number } {
  const start = Math.floor(new Date(`${date}T00:00:00`).getTime() / 1000)
  const now = Math.floor(Date.now() / 1000)
  const fullEnd = start + 86400
  return { start, end: now > start && now < fullEnd ? now : fullEnd }
}

function PowerChart({ records, date }: { records: PowerRecord[]; date: string }) {
  if (records.length === 0) {
    return <p style={{ color: '#94a3b8', fontSize: 13 }}>No recordings for this day.</p>
  }

  const { start: minT, end: maxT } = dayBounds(date)
  const spanT = maxT - minT || 1

  const rawMax = Math.max(0, ...records.map(r => r.power_w))
  const rawMin = Math.min(0, ...records.map(r => r.power_w))
  const yMax = niceExtent(rawMax) || 1000
  const yMin = niceExtent(rawMin)
  const yRange = yMax - yMin

  const xPos = (t: number) => PAD.left + ((t - minT) / spanT) * PLOT_W
  const yPos = (p: number) => PAD.top + PLOT_H - ((p - yMin) / yRange) * PLOT_H
  const zeroY = yPos(0)
  const zeroYStr = zeroY.toFixed(1)

  // Split the records into contiguous segments, breaking wherever consecutive
  // minutes are further apart than the logging cadence. Each segment is drawn as
  // its own line/fill so the chart never bridges a gap with a misleading line.
  const segments: PowerRecord[][] = []
  let cur: PowerRecord[] = []
  for (const r of records) {
    if (cur.length && r.minute - cur[cur.length - 1].minute > GAP_THRESHOLD_S) {
      segments.push(cur)
      cur = []
    }
    cur.push(r)
  }
  if (cur.length) segments.push(cur)

  // No-data spans: before the first sample, between segments, and after the last
  // sample up to the end of the window. These get flagged in red.
  const noData: { from: number; to: number }[] = []
  if (records[0].minute - minT > GAP_THRESHOLD_S)
    noData.push({ from: minT, to: records[0].minute })
  for (let i = 1; i < segments.length; i++)
    noData.push({ from: segments[i - 1][segments[i - 1].length - 1].minute, to: segments[i][0].minute })
  if (maxT - records[records.length - 1].minute > GAP_THRESHOLD_S)
    noData.push({ from: records[records.length - 1].minute, to: maxT })

  // Y axis ticks — one step above and below zero
  const absExtent = Math.max(Math.abs(yMax), Math.abs(yMin))
  const yStep = absExtent <= 2000 ? 500 : absExtent <= 5000 ? 1000 : 2000
  const yTicks: { y: number; label: string }[] = []
  for (let p = yMin; p <= yMax; p += yStep)
    yTicks.push({ y: yPos(p), label: fmtW(p) })

  // X axis: one tick per hour across the whole day window
  const startHour = Math.ceil(minT / 3600)
  const endHour   = Math.floor(maxT / 3600)
  const xTicks: { x: number; label: string }[] = []
  for (let h = startHour; h <= endHour; h++) {
    const t = h * 3600
    if (t < minT || t > maxT) continue
    const d = new Date(t * 1000)
    const label = `${d.getHours().toString().padStart(2, '0')}:00`
    xTicks.push({ x: xPos(t), label })
  }

  return (
    <svg viewBox={`0 0 ${SVG_W} ${SVG_H}`} style={{ width: '100%', height: 'auto', display: 'block' }}>
      {/* Horizontal grid lines */}
      {yTicks.map(t => (
        <line key={t.label} x1={PAD.left} y1={t.y} x2={PAD.left + PLOT_W} y2={t.y}
          stroke={t.label === '0W' ? '#94a3b8' : '#e2e8f0'} strokeWidth={t.label === '0W' ? 1.5 : 1} />
      ))}

      {/* Axes (drawn before the no-data markers so the red strip sits on top) */}
      <line x1={PAD.left} y1={PAD.top} x2={PAD.left} y2={PAD.top + PLOT_H}
        stroke="#cbd5e1" strokeWidth={1} />
      <line x1={PAD.left} y1={PAD.top + PLOT_H} x2={PAD.left + PLOT_W} y2={PAD.top + PLOT_H}
        stroke="#cbd5e1" strokeWidth={1} />

      {/* No-data spans: a faint band plus a solid red strip along the x axis */}
      {noData.map((g, i) => (
        <g key={`gap-${i}`}>
          <rect x={xPos(g.from)} y={PAD.top} width={Math.max(0, xPos(g.to) - xPos(g.from))} height={PLOT_H}
            fill="rgba(239,68,68,0.10)" />
          <line x1={xPos(g.from)} y1={PAD.top + PLOT_H} x2={xPos(g.to)} y2={PAD.top + PLOT_H}
            stroke="#ef4444" strokeWidth={3} />
        </g>
      ))}

      {/* One filled area + line per contiguous segment of real data */}
      {segments.map((seg, i) => {
        const poly = seg.map(r => `${xPos(r.minute).toFixed(1)},${yPos(r.power_w).toFixed(1)}`).join(' ')
        const fx = xPos(seg[0].minute).toFixed(1)
        const lx = xPos(seg[seg.length - 1].minute).toFixed(1)
        return (
          <g key={`seg-${i}`}>
            <polygon points={`${fx},${zeroYStr} ${poly} ${lx},${zeroYStr}`} fill="rgba(59,130,246,0.08)" />
            <polyline points={poly} fill="none" stroke="#3b82f6" strokeWidth={1.5}
              strokeLinejoin="round" strokeLinecap="round" />
          </g>
        )
      })}

      {/* Axis tick labels */}
      {yTicks.map(t => (
        <text key={t.label} x={PAD.left - 6} y={t.y + 4}
          textAnchor="end" fontSize={10} fill="#94a3b8">{t.label}</text>
      ))}
      {xTicks.map(t => (
        <text key={t.label} x={t.x} y={SVG_H - 6}
          textAnchor="middle" fontSize={10} fill="#94a3b8">{t.label}</text>
      ))}
    </svg>
  )
}

function todayString(): string {
  const d = new Date()
  return `${d.getFullYear()}-${String(d.getMonth() + 1).padStart(2, '0')}-${String(d.getDate()).padStart(2, '0')}`
}

// The topology graph is the source of truth for which nodes exist and how they
// are wired. The handle a node connects to on the consumer unit IS its role.
const CU_ID = 'consumer_unit'

type NodeRole = 'grid' | 'solar' | 'appliance' | 'other'

type SavedNodeConfig = { id: string; settings?: Record<string, unknown> }
type SavedEdgeConfig = { id: string; source: string; target: string; sourceHandle?: string; targetHandle?: string }

type ConnectedNode = { graphId: string; label: string; role: NodeRole }

function roleForHandle(handle?: string | null): NodeRole {
  if (handle === 'grid') return 'grid'
  if (handle?.startsWith('solar')) return 'solar'
  if (handle?.startsWith('circuit')) return 'appliance'
  return 'other'
}

function nodeLabel(node: SavedNodeConfig | undefined, fallback: string): string {
  if (!node) return fallback
  const name = node.settings?.name
  const label = node.settings?.label
  if (typeof name === 'string' && name) return name
  if (typeof label === 'string' && label) return label
  return fallback
}

const ROLE_ORDER: Record<NodeRole, number> = { grid: 0, solar: 1, appliance: 2, other: 3 }

// Derive the connected nodes (and their roles) from the consumer unit's edges.
function connectedNodes(nodes: SavedNodeConfig[], edges: SavedEdgeConfig[]): ConnectedNode[] {
  const byId = new Map(nodes.map(n => [n.id, n]))
  const seen = new Set<string>()
  const result: ConnectedNode[] = []

  for (const e of edges) {
    let otherId: string | null = null
    let cuHandle: string | null | undefined = null
    if (e.source === CU_ID) { otherId = e.target; cuHandle = e.sourceHandle }
    else if (e.target === CU_ID) { otherId = e.source; cuHandle = e.targetHandle }
    if (!otherId || seen.has(otherId)) continue
    seen.add(otherId)
    result.push({
      graphId: otherId,
      label: nodeLabel(byId.get(otherId), otherId),
      role: roleForHandle(cuHandle),
    })
  }

  return result.sort((a, b) =>
    ROLE_ORDER[a.role] - ROLE_ORDER[b.role] || a.label.localeCompare(b.label))
}

// Fetch a day's power records from the given endpoint and chart them. The grid
// reads its own stream; every other connected node reads its per-node stream.
function ProfileSection({ url, date }: { url: string; date: string }) {
  const [records, setRecords] = useState<PowerRecord[]>([])
  const [loading, setLoading] = useState(true)
  const [error, setError] = useState<string | null>(null)

  useEffect(() => {
    setLoading(true)
    setError(null)
    fetch(url)
      .then(r => r.ok ? r.json() : Promise.reject(`HTTP ${r.status}`))
      .then((data: { records: PowerRecord[] }) => {
        setRecords(data.records)
        setLoading(false)
      })
      .catch((e: unknown) => {
        setError(e instanceof Error ? e.message : String(e))
        setLoading(false)
      })
  }, [url])

  if (error) return <div className="alert alert-danger">{error}</div>
  if (loading) return <p style={{ color: '#94a3b8', fontSize: 13 }}>Loading…</p>
  return <PowerChart records={records} date={date} />
}

function NodeCard({ node, date }: { node: ConnectedNode; date: string }) {
  const url = node.role === 'grid'
    ? `/api/data/grid?date=${date}`
    : `/api/data/node?id=${encodeURIComponent(node.graphId)}&date=${date}`
  return (
    <div className="mb-4">
      <h2 style={{ fontSize: 14, fontWeight: 600, color: '#64748b', marginBottom: 12 }}>
        {node.label}{node.role === 'grid' ? ' (Grid)' : ''}
      </h2>
      <div style={{ border: '1px solid #e2e8f0', borderRadius: 8, padding: '12px 16px', background: '#fff' }}>
        <ProfileSection url={url} date={date} />
      </div>
    </div>
  )
}

function Power() {
  const [searchParams, setSearchParams] = useSearchParams()
  const date = searchParams.get('date') ?? todayString()

  function setDate(value: string) {
    setSearchParams(prev => { prev.set('date', value); return prev }, { replace: true })
  }

  const [nodes, setNodes] = useState<ConnectedNode[]>([])
  const [loading, setLoading] = useState(true)
  const [error, setError] = useState<string | null>(null)

  useEffect(() => {
    setLoading(true)
    setError(null)
    fetch('/api/nodes')
      .then(r => r.ok ? r.json() : Promise.reject(`HTTP ${r.status}`))
      .then((data: { nodes?: SavedNodeConfig[]; edges?: SavedEdgeConfig[] }) => {
        setNodes(connectedNodes(data.nodes ?? [], data.edges ?? []))
        setLoading(false)
      })
      .catch((e: unknown) => {
        setError(e instanceof Error ? e.message : String(e))
        setLoading(false)
      })
  }, [])

  return (
    <div className="container">
      <div className="mt-3 mb-2 d-flex align-items-center justify-content-between">
        <h1 className="mb-0">Power</h1>
        <input
          type="date"
          className="form-control form-control-sm"
          style={{ width: 160 }}
          value={date}
          max={todayString()}
          onChange={e => setDate(e.target.value)}
        />
      </div>
      <hr />

      {error && <div className="alert alert-danger">{error}</div>}

      {loading ? (
        <p style={{ color: '#94a3b8', fontSize: 13 }}>Loading…</p>
      ) : nodes.length === 0 ? (
        <p style={{ color: '#94a3b8', fontSize: 13 }}>
          No connected devices. Wire a meter or appliance to the consumer unit on the Topology page.
        </p>
      ) : (
        nodes.map(n => <NodeCard key={n.graphId} node={n} date={date} />)
      )}
    </div>
  )
}

export default Power
