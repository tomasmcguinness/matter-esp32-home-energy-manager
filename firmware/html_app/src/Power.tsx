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

// ---- 30-day usage history -------------------------------------------------

type EnergyRole = 'grid' | 'solar' | 'load'
type DailyEnergy = {
  nodes: { id: string; role: EnergyRole }[]
  days: { date: string; kwh: Record<string, number> }[]
}

type Series = { id: string; label: string; color: string; total: number }

const HISTORY_DAYS = 30
const UNMONITORED_ID = '__unmonitored'
const UNMONITORED_COLOR = '#94a3b8'
// Categorical palette for appliances; assigned in graph-id order so a device
// keeps its colour between reloads.
const PALETTE = ['#3b82f6', '#f59e0b', '#10b981', '#ef4444', '#8b5cf6', '#ec4899', '#14b8a6', '#f97316']

const H_PAD = { top: 12, right: 16, bottom: 28, left: 56 }
const H_W = 800
const H_H = 240
const H_PLOT_W = H_W - H_PAD.left - H_PAD.right
const H_PLOT_H = H_H - H_PAD.top - H_PAD.bottom

function kwhStep(max: number): number {
  for (const s of [0.5, 1, 2, 5, 10, 20, 50, 100]) if (max / s <= 5) return s
  return 200
}

function fmtKwh(v: number): string {
  return `${v.toFixed(1)} kWh`
}

function fmtDay(date: string): string {
  return new Date(`${date}T00:00:00`).toLocaleDateString(undefined, { day: 'numeric', month: 'short' })
}

// Daily consumption = net grid + solar output (grid is signed, so export days
// net out). Each appliance stream is a segment; the remainder of the total not
// covered by a metered appliance is the unmonitored baseload.
function buildHistory(data: DailyEnergy, labels: Map<string, string>) {
  const gridIds = data.nodes.filter(n => n.role === 'grid').map(n => n.id)
  const solarIds = data.nodes.filter(n => n.role === 'solar').map(n => n.id)
  const loadIds = data.nodes.filter(n => n.role === 'load').map(n => n.id).sort()

  const days = data.days.map(d => {
    const hasGrid = gridIds.some(id => d.kwh[id] !== undefined)
    if (!hasGrid) return { date: d.date, total: null as number | null, parts: {} as Record<string, number> }
    const sum = (ids: string[]) => ids.reduce((acc, id) => acc + (d.kwh[id] ?? 0), 0)
    const parts: Record<string, number> = {}
    for (const id of loadIds) parts[id] = Math.max(0, d.kwh[id] ?? 0)
    const loads = Object.values(parts).reduce((a, b) => a + b, 0)
    const total = Math.max(loads, sum(gridIds) + sum(solarIds))
    parts[UNMONITORED_ID] = total - loads
    return { date: d.date, total, parts }
  })

  const totalOf = (id: string) => days.reduce((acc, d) => acc + (d.parts[id] ?? 0), 0)
  const series: Series[] = loadIds
    .map((id, i) => ({ id, label: labels.get(id) ?? id, color: PALETTE[i % PALETTE.length], total: totalOf(id) }))
    .sort((a, b) => b.total - a.total)
  series.push({ id: UNMONITORED_ID, label: 'Unmonitored', color: UNMONITORED_COLOR, total: totalOf(UNMONITORED_ID) })

  return { days, series, hasGrid: gridIds.length > 0 }
}

function UsageHistory({ labels }: { labels: Map<string, string> }) {
  const [data, setData] = useState<DailyEnergy | null>(null)
  const [error, setError] = useState<string | null>(null)
  const [hovered, setHovered] = useState<string | null>(null)

  useEffect(() => {
    fetch(`/api/data/daily-energy?days=${HISTORY_DAYS}`)
      .then(r => r.ok ? r.json() : Promise.reject(`HTTP ${r.status}`))
      .then((d: DailyEnergy) => setData(d))
      .catch((e: unknown) => setError(e instanceof Error ? e.message : String(e)))
  }, [])

  let body
  if (error) body = <div className="alert alert-danger">{error}</div>
  else if (!data) body = <p style={{ color: '#94a3b8', fontSize: 13 }}>Loading…</p>
  else {
    const { days, series, hasGrid } = buildHistory(data, labels)
    if (!hasGrid) {
      body = <p style={{ color: '#94a3b8', fontSize: 13 }}>Assign a grid meter on the Topology page to see usage history.</p>
    } else {
      const grandTotal = days.reduce((acc, d) => acc + (d.total ?? 0), 0)
      const step = kwhStep(Math.max(0, ...days.map(d => d.total ?? 0)) || 1)
      const yMax = Math.max(step, Math.ceil(Math.max(0, ...days.map(d => d.total ?? 0)) / step) * step)
      const yPos = (v: number) => H_PAD.top + H_PLOT_H - (v / yMax) * H_PLOT_H
      const slot = H_PLOT_W / days.length
      const barW = slot * 0.7

      const yTicks: number[] = []
      for (let v = 0; v <= yMax + 1e-9; v += step) yTicks.push(v)

      const opacity = (id: string) => hovered === null || hovered === id ? 1 : 0.25

      body = (
        <>
          <svg viewBox={`0 0 ${H_W} ${H_H}`} style={{ width: '100%', height: 'auto', display: 'block' }}
            onMouseLeave={() => setHovered(null)}>
            {yTicks.map(v => (
              <g key={v}>
                <line x1={H_PAD.left} y1={yPos(v)} x2={H_PAD.left + H_PLOT_W} y2={yPos(v)}
                  stroke={v === 0 ? '#cbd5e1' : '#e2e8f0'} strokeWidth={1} />
                <text x={H_PAD.left - 6} y={yPos(v) + 4} textAnchor="end" fontSize={10} fill="#94a3b8">
                  {`${v % 1 ? v.toFixed(1) : v} kWh`}
                </text>
              </g>
            ))}

            {days.map((d, i) => {
              const x = H_PAD.left + i * slot + (slot - barW) / 2
              if (d.total === null) {
                // No grid data for this day: flag it along the x axis.
                return <line key={d.date} x1={x} y1={H_PAD.top + H_PLOT_H} x2={x + barW} y2={H_PAD.top + H_PLOT_H}
                  stroke="#ef4444" strokeWidth={3}><title>{`${fmtDay(d.date)} · no data`}</title></line>
              }
              // Stack bottom-up in series order (largest appliance first, unmonitored last).
              let acc = 0
              return (
                <g key={d.date}>
                  {series.map(s => {
                    const v = d.parts[s.id] ?? 0
                    if (v <= 0) return null
                    const y0 = yPos(acc)
                    acc += v
                    const y1 = yPos(acc)
                    return (
                      <rect key={s.id} x={x} y={y1} width={barW} height={Math.max(0, y0 - y1)}
                        fill={s.color} opacity={opacity(s.id)} style={{ transition: 'opacity .12s' }}
                        onMouseEnter={() => setHovered(s.id)}>
                        <title>{`${s.label} · ${fmtDay(d.date)} · ${fmtKwh(v)}`}</title>
                      </rect>
                    )
                  })}
                </g>
              )
            })}

            {days.map((d, i) => (i % 5 === (days.length - 1) % 5) && (
              <text key={d.date} x={H_PAD.left + i * slot + slot / 2} y={H_H - 8}
                textAnchor="middle" fontSize={10} fill="#94a3b8">{fmtDay(d.date)}</text>
            ))}
          </svg>

          <table className="table table-sm mb-0 mt-3" style={{ fontSize: 13 }}>
            <thead>
              <tr style={{ color: '#64748b' }}>
                <th style={{ fontWeight: 600 }}>Device</th>
                <th style={{ fontWeight: 600, textAlign: 'right' }}>Consumption</th>
                <th style={{ fontWeight: 600, textAlign: 'right' }}>Share</th>
              </tr>
            </thead>
            <tbody>
              {series.map(s => (
                <tr key={s.id}
                  onMouseEnter={() => setHovered(s.id)}
                  onMouseLeave={() => setHovered(null)}
                  style={{ cursor: 'default' }}>
                  <td style={{ background: hovered === s.id ? '#f1f5f9' : undefined }}>
                    <span style={{
                      display: 'inline-block', width: 12, height: 12, borderRadius: 2,
                      background: s.color, marginRight: 8, verticalAlign: '-1px',
                    }} />
                    {s.label}
                  </td>
                  <td style={{ textAlign: 'right', background: hovered === s.id ? '#f1f5f9' : undefined }}>{fmtKwh(s.total)}</td>
                  <td style={{ textAlign: 'right', background: hovered === s.id ? '#f1f5f9' : undefined }}>
                    {grandTotal > 0 ? `${((s.total / grandTotal) * 100).toFixed(1)}%` : '—'}
                  </td>
                </tr>
              ))}
              <tr style={{ fontWeight: 600 }}>
                <td>Total</td>
                <td style={{ textAlign: 'right' }}>{fmtKwh(grandTotal)}</td>
                <td style={{ textAlign: 'right' }}>{grandTotal > 0 ? '100%' : '—'}</td>
              </tr>
              <tr style={{ fontWeight: 600 }}>
                <td>Total cost</td>
                <td style={{ textAlign: 'right', color: '#94a3b8' }}>N/A</td>
                <td />
              </tr>
            </tbody>
          </table>
        </>
      )
    }
  }

  return (
    <div className="mb-4">
      <h2 style={{ fontSize: 14, fontWeight: 600, color: '#64748b', marginBottom: 12 }}>
        Usage history (last {HISTORY_DAYS} days)
      </h2>
      <div style={{ border: '1px solid #e2e8f0', borderRadius: 8, padding: '12px 16px', background: '#fff' }}>
        {body}
      </div>
    </div>
  )
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
  const [labels, setLabels] = useState<Map<string, string>>(new Map())
  const [loading, setLoading] = useState(true)
  const [error, setError] = useState<string | null>(null)

  useEffect(() => {
    setLoading(true)
    setError(null)
    fetch('/api/nodes')
      .then(r => r.ok ? r.json() : Promise.reject(`HTTP ${r.status}`))
      .then((data: { nodes?: SavedNodeConfig[]; edges?: SavedEdgeConfig[] }) => {
        setNodes(connectedNodes(data.nodes ?? [], data.edges ?? []))
        // Every graph node, not just direct CU neighbours: sub-CU loads are streams too.
        setLabels(new Map((data.nodes ?? []).map(n => [n.id, nodeLabel(n, n.id)])))
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
        <>
          <UsageHistory labels={labels} />
          {nodes.map(n => <NodeCard key={n.graphId} node={n} date={date} />)}
        </>
      )}
    </div>
  )
}

export default Power
