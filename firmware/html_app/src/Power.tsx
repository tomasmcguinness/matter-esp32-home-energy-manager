import { useEffect, useState } from 'react'
import { useSearchParams } from 'react-router'
import { fmtMoney } from './tariff'
import { isAbortError, queuedFetch } from './fetchQueue'

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
    if (cuHandle === 'tariff') continue // the tariff source prices power; it doesn't meter any
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
    // Queued: one chart per connected node would otherwise all hit the device at
    // once. Aborted on date change so stale days don't hold up the queue.
    const ac = new AbortController()
    queuedFetch(url, { signal: ac.signal })
      .then(r => r.ok ? r.json() : Promise.reject(`HTTP ${r.status}`))
      .then((data: { records: PowerRecord[] }) => {
        setRecords(data.records)
        setLoading(false)
      })
      .catch((e: unknown) => {
        if (isAbortError(e)) return
        setError(e instanceof Error ? e.message : String(e))
        setLoading(false)
      })
    return () => ac.abort()
  }, [url])

  if (error) return <div className="alert alert-danger">{error}</div>
  if (loading) return <p style={{ color: '#94a3b8', fontSize: 13 }}>Loading…</p>
  return <PowerChart records={records} date={date} />
}

// ---- 30-day usage history -------------------------------------------------

type EnergyRole = 'grid' | 'solar' | 'load' | 'battery'
type DailyEnergy = {
  nodes: { id: string; role: EnergyRole }[]
  // cost is in major currency units, present only for days with a recorded tariff.
  // solar_kwh / grid_kwh split each load's energy by source (battery discharge
  // counted by where its stored energy came from); present for days with grid data.
  days: { date: string; kwh: Record<string, number>; cost?: Record<string, number>; solar_kwh?: Record<string, number>; grid_kwh?: Record<string, number> }[]
  currency?: number
}

type Series = { id: string; label: string; color: string; total: number; cost: number; solar: number; grid: number }

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

// Share of a device's energy that came from solar (directly or via the battery),
// with the kWh breakdown as a tooltip. '—' when there is no split data.
function SelfPowered({ solar, grid }: { solar: number; grid: number }) {
  const sum = solar + grid
  if (sum <= 0) return <span style={{ color: '#94a3b8' }}>—</span>
  return <span title={`${fmtKwh(solar)} solar · ${fmtKwh(grid)} grid`}>{((solar / sum) * 100).toFixed(0)}%</span>
}

function fmtDay(date: string): string {
  return new Date(`${date}T00:00:00`).toLocaleDateString(undefined, { day: 'numeric', month: 'short' })
}

// Daily consumption = net grid import + solar output. kWh keeps the raw Matter
// sign (+ = into the device), so grid is + import and a generating inverter is
// negative: solar output is -solar, and export days net out. Each appliance
// stream is a segment; the remainder of the total not covered by a metered
// appliance is the unmonitored baseload.
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
    const total = Math.max(loads, sum(gridIds) - sum(solarIds))
    parts[UNMONITORED_ID] = total - loads
    return { date: d.date, total, parts }
  })

  const totalOf = (id: string) => days.reduce((acc, d) => acc + (d.parts[id] ?? 0), 0)
  // The firmware keys the remainder's cost as "__unmonitored", matching UNMONITORED_ID.
  const costOf = (id: string) => data.days.reduce((acc, d) => acc + (d.cost?.[id] ?? 0), 0)
  // The firmware keys the remainder's split as "__unmonitored" too.
  const solarOf = (id: string) => data.days.reduce((acc, d) => acc + (d.solar_kwh?.[id] ?? 0), 0)
  const gridOf = (id: string) => data.days.reduce((acc, d) => acc + (d.grid_kwh?.[id] ?? 0), 0)
  const seriesFor = (id: string, label: string, color: string): Series =>
    ({ id, label, color, total: totalOf(id), cost: costOf(id), solar: solarOf(id), grid: gridOf(id) })
  const series: Series[] = loadIds
    .map((id, i) => seriesFor(id, labels.get(id) ?? id, PALETTE[i % PALETTE.length]))
    .sort((a, b) => b.total - a.total)
  series.push(seriesFor(UNMONITORED_ID, 'Unmonitored', UNMONITORED_COLOR))

  const costedDays = data.days.filter(d => d.cost !== undefined).length
  return { days, series, hasGrid: gridIds.length > 0, costedDays }
}

function UsageHistory({ labels }: { labels: Map<string, string> }) {
  const [data, setData] = useState<DailyEnergy | null>(null)
  const [error, setError] = useState<string | null>(null)
  const [hovered, setHovered] = useState<string | null>(null)

  useEffect(() => {
    queuedFetch(`/api/data/daily-energy?days=${HISTORY_DAYS}`)
      .then(r => r.ok ? r.json() : Promise.reject(`HTTP ${r.status}`))
      .then((d: DailyEnergy) => setData(d))
      .catch((e: unknown) => setError(e instanceof Error ? e.message : String(e)))
  }, [])

  let body
  if (error) body = <div className="alert alert-danger">{error}</div>
  else if (!data) body = <p style={{ color: '#94a3b8', fontSize: 13 }}>Loading…</p>
  else {
    const { days, series, hasGrid, costedDays } = buildHistory(data, labels)
    const hasCost = costedDays > 0
    const totalCost = series.reduce((acc, s) => acc + s.cost, 0)
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
                <th style={{ fontWeight: 600, textAlign: 'right' }} title="Energy supplied by solar, directly or from solar-charged battery">Self-powered</th>
                <th style={{ fontWeight: 600, textAlign: 'right' }}>Cost</th>
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
                  <td style={{ textAlign: 'right', background: hovered === s.id ? '#f1f5f9' : undefined }}>
                    <SelfPowered solar={s.solar} grid={s.grid} />
                  </td>
                  <td style={{ textAlign: 'right', background: hovered === s.id ? '#f1f5f9' : undefined, color: hasCost ? undefined : '#94a3b8' }}>
                    {hasCost ? fmtMoney(s.cost, data.currency) : '—'}
                  </td>
                </tr>
              ))}
              <tr style={{ fontWeight: 600 }}>
                <td>Total</td>
                <td style={{ textAlign: 'right' }}>{fmtKwh(grandTotal)}</td>
                <td style={{ textAlign: 'right' }}>{grandTotal > 0 ? '100%' : '—'}</td>
                <td style={{ textAlign: 'right' }}>
                  <SelfPowered solar={series.reduce((a, s) => a + s.solar, 0)} grid={series.reduce((a, s) => a + s.grid, 0)} />
                </td>
                <td />
              </tr>
              <tr style={{ fontWeight: 600 }}>
                <td>
                  Total cost
                  {hasCost && costedDays < days.length && (
                    <span style={{ fontWeight: 400, color: '#94a3b8' }}> ({costedDays} of {days.length} days)</span>
                  )}
                </td>
                <td />
                <td />
                <td />
                <td style={{ textAlign: 'right', color: hasCost ? undefined : '#94a3b8' }}>
                  {hasCost ? fmtMoney(totalCost, data.currency) : 'N/A'}
                </td>
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

// ---- Hourly heatmap ---------------------------------------------------------

// GET /api/data/node?id=<graphId>&days=N, oldest day first. wh and peak_w keep
// the raw meter sign; null where nothing was recorded.
type HeatmapData = {
  id: string
  role?: EnergyRole
  days: { date: string; wh: (number | null)[]; peak_w: (number | null)[] }[]
}

type HeatMetric = 'wh' | 'peak'

const HEAT_RANGES = [7, 14, 30, 60]
const HM_LEFT = 72
const HM_RIGHT = 64 // daily total bar
const HM_TOP = 20
const HM_W = 800
const HM_CELL_W = (HM_W - HM_LEFT - HM_RIGHT) / 24
const HM_CELL_H = 16
const HM_GAP = 2

// Sequential scale for the stream's normal direction, and a green one for the
// reverse (grid export, battery discharge). Both start pale so low reads as quiet.
const HEAT_STOPS = ['#e0f2fe', '#38bdf8', '#10b981', '#f59e0b', '#ef4444']
const REVERSE_STOPS = ['#dcfce7', '#4ade80', '#15803d']
const HEAT_ZERO = '#f1f5f9'

function lerpColor(stops: string[], t: number): string {
  const x = Math.min(1, Math.max(0, t)) * (stops.length - 1)
  const i = Math.min(stops.length - 2, Math.floor(x))
  const f = x - i
  const a = parseInt(stops[i].slice(1), 16)
  const b = parseInt(stops[i + 1].slice(1), 16)
  const ch = (s: number) => Math.round(((a >> s) & 255) + (((b >> s) & 255) - ((a >> s) & 255)) * f)
  return `rgb(${ch(16)}, ${ch(8)}, ${ch(0)})`
}

// Readings are raw sign (+ = into the node). Solar is shown as generation, so
// every stream reads positive in its usual direction.
function heatSign(role?: EnergyRole): number {
  return role === 'solar' ? -1 : 1
}

function reverseLabel(role?: EnergyRole): string {
  return role === 'grid' ? 'export' : role === 'battery' ? 'discharge' : 'reverse'
}

function fmtWh(wh: number): string {
  const abs = Math.abs(wh)
  const sign = wh < 0 ? '−' : ''
  return abs >= 1000 ? `${sign}${(abs / 1000).toFixed(2)} kWh` : `${sign}${Math.round(abs)} Wh`
}

function fmtDayLong(date: string): string {
  return new Date(`${date}T00:00:00`).toLocaleDateString(undefined, { weekday: 'short', day: 'numeric', month: 'short' })
}

function HourlyHeatmap({ nodes }: { nodes: ConnectedNode[] }) {
  const [streamId, setStreamId] = useState<string>(
    () => (nodes.find(n => n.role === 'appliance') ?? nodes[0]).graphId)
  const [range, setRange] = useState(14)
  const [metric, setMetric] = useState<HeatMetric>('wh')
  const [data, setData] = useState<HeatmapData | null>(null)
  const [error, setError] = useState<string | null>(null)
  const [hover, setHover] = useState<{ day: number; hour: number } | null>(null)

  useEffect(() => {
    setData(null)
    setError(null)
    setHover(null)
    const ac = new AbortController()
    queuedFetch(`/api/data/node?id=${encodeURIComponent(streamId)}&days=${range}`, { signal: ac.signal })
      .then(r => r.ok ? r.json() : Promise.reject(`HTTP ${r.status}`))
      .then((d: HeatmapData) => setData(d))
      .catch((e: unknown) => {
        if (isAbortError(e)) return
        setError(e instanceof Error ? e.message : String(e))
      })
    return () => ac.abort()
  }, [streamId, range])

  const today = todayString()
  const nowHour = new Date().getHours()

  let body
  if (error) body = <div className="alert alert-danger">{error}</div>
  else if (!data) body = <p style={{ color: '#94a3b8', fontSize: 13 }}>Loading…</p>
  else {
    const sign = heatSign(data.role)
    // Newest day at the top, as the most recent hours are the ones looked at most.
    const rows = [...data.days].reverse().map(d => ({
      date: d.date,
      values: (metric === 'wh' ? d.wh : d.peak_w).map(v => v === null ? null : v * sign),
      wh: d.wh.map(v => v === null ? null : v * sign),
      peak: d.peak_w.map(v => v === null ? null : v * sign),
    }))
    const all = rows.flatMap(r => r.values).filter((v): v is number => v !== null)
    const posMax = Math.max(0, ...all)
    const negMax = Math.max(0, ...all.map(v => -v))
    const hasData = all.length > 0

    const avg = Array.from({ length: 24 }, (_, h) => {
      const vs = rows.map(r => r.values[h]).filter((v): v is number => v !== null)
      return vs.length ? vs.reduce((a, b) => a + b, 0) / vs.length : null
    })
    const dayTotals = rows.map(r => r.wh.reduce<number>((a, v) => a + (v !== null && v > 0 ? v : 0), 0))
    const totalMax = Math.max(1, ...dayTotals)

    const fill = (v: number | null): string => {
      if (v === null) return '#fff'
      if (v > 0 && posMax > 0) return lerpColor(HEAT_STOPS, v / posMax)
      if (v < 0 && negMax > 0) return lerpColor(REVERSE_STOPS, -v / negMax)
      return HEAT_ZERO
    }
    const fmtValue = metric === 'wh' ? fmtWh : fmtW

    const avgY = HM_TOP + rows.length * HM_CELL_H + 6
    const svgH = avgY + HM_CELL_H + 4
    const cellX = (h: number) => HM_LEFT + h * HM_CELL_W
    const rowY = (i: number) => HM_TOP + i * HM_CELL_H

    const hovered = hover && hover.day >= 0 ? rows[hover.day] : null

    body = !hasData ? (
      <p style={{ color: '#94a3b8', fontSize: 13 }}>No recordings for this stream in the last {range} days.</p>
    ) : (
      <>
        <svg viewBox={`0 0 ${HM_W} ${svgH}`} style={{ width: '100%', height: 'auto', display: 'block' }}
          onMouseLeave={() => setHover(null)}>
          <defs>
            <pattern id="heat-future" width={6} height={6} patternUnits="userSpaceOnUse" patternTransform="rotate(45)">
              <line x1={0} y1={0} x2={0} y2={6} stroke="#e2e8f0" strokeWidth={2} />
            </pattern>
          </defs>

          {Array.from({ length: 24 }, (_, h) => (h % 2 === 0) && (
            <text key={h} x={cellX(h) + HM_CELL_W / 2} y={HM_TOP - 6} textAnchor="middle" fontSize={10} fill="#94a3b8">
              {String(h).padStart(2, '0')}
            </text>
          ))}

          {rows.map((r, i) => {
            const isToday = r.date === today
            const weekend = [0, 6].includes(new Date(`${r.date}T00:00:00`).getDay())
            return (
              <g key={r.date}>
                <text x={HM_LEFT - 8} y={rowY(i) + HM_CELL_H / 2 + 3.5} textAnchor="end" fontSize={10}
                  fill={isToday ? '#3b82f6' : weekend ? '#334155' : '#94a3b8'} fontWeight={isToday || weekend ? 600 : 400}>
                  {isToday ? 'Today' : fmtDayLong(r.date)}
                </text>
                {r.values.map((v, h) => {
                  const future = isToday && h > nowHour
                  const active = hover?.day === i && hover.hour === h
                  return (
                    <rect key={h} x={cellX(h) + HM_GAP / 2} y={rowY(i) + HM_GAP / 2}
                      width={HM_CELL_W - HM_GAP} height={HM_CELL_H - HM_GAP} rx={2}
                      fill={future ? 'url(#heat-future)' : fill(v)}
                      stroke={active ? '#334155' : v === null && !future ? '#e2e8f0' : 'none'}
                      strokeWidth={active ? 1.5 : 1}
                      onMouseEnter={() => setHover({ day: i, hour: h })} />
                  )
                })}
                <rect x={HM_W - HM_RIGHT + 10} y={rowY(i) + 4} height={HM_CELL_H - 8} rx={2}
                  width={(dayTotals[i] / totalMax) * (HM_RIGHT - 14)} fill="#cbd5e1">
                  <title>{`${fmtDayLong(r.date)} · ${fmtWh(dayTotals[i])}`}</title>
                </rect>
              </g>
            )
          })}

          <text x={HM_LEFT - 8} y={avgY + HM_CELL_H / 2 + 3.5} textAnchor="end" fontSize={10} fill="#64748b" fontWeight={600}>
            Avg / h
          </text>
          {avg.map((v, h) => (
            <rect key={h} x={cellX(h) + HM_GAP / 2} y={avgY + HM_GAP / 2}
              width={HM_CELL_W - HM_GAP} height={HM_CELL_H - HM_GAP} rx={2}
              fill={fill(v)} stroke={hover?.day === -1 && hover.hour === h ? '#334155' : 'none'} strokeWidth={1.5}
              onMouseEnter={() => setHover({ day: -1, hour: h })} />
          ))}
        </svg>

        <div className="d-flex flex-wrap align-items-center justify-content-between mt-2" style={{ fontSize: 12, color: '#64748b', gap: 12 }}>
          <div style={{ minHeight: 18 }}>
            {hover === null ? (
              <span style={{ color: '#94a3b8' }}>Hover a cell to see the hour's energy and peak power.</span>
            ) : hover.day === -1 ? (
              <>
                <strong>Average</strong> · {String(hover.hour).padStart(2, '0')}:00–{String(hover.hour + 1).padStart(2, '0')}:00 ·{' '}
                {avg[hover.hour] === null ? 'no data' : fmtValue(Math.round(avg[hover.hour]!))}
              </>
            ) : hovered && (
              <>
                <strong>{fmtDayLong(hovered.date)}</strong> · {String(hover.hour).padStart(2, '0')}:00–{String(hover.hour + 1).padStart(2, '0')}:00 ·{' '}
                {hovered.date === today && hover.hour > nowHour ? 'not yet' : hovered.wh[hover.hour] === null ? 'no data' : (
                  <>
                    {fmtWh(hovered.wh[hover.hour]!)}
                    {hovered.peak[hover.hour] !== null && <> · peak {fmtW(Math.round(hovered.peak[hover.hour]!))}</>}
                  </>
                )}
              </>
            )}
          </div>
          <div className="d-flex align-items-center" style={{ gap: 6 }}>
            {negMax > 0 && (
              <>
                <span>{fmtValue(Math.round(-negMax))} {reverseLabel(data.role)}</span>
                <span style={{ width: 48, height: 8, borderRadius: 4, background: `linear-gradient(to left, ${REVERSE_STOPS.join(', ')})` }} />
              </>
            )}
            <span>0</span>
            <span style={{ width: 96, height: 8, borderRadius: 4, background: `linear-gradient(to right, ${HEAT_STOPS.join(', ')})` }} />
            <span>{fmtValue(Math.round(posMax))}</span>
          </div>
        </div>
      </>
    )
  }

  const toggle = (active: boolean) => `btn btn-sm ${active ? 'btn-secondary' : 'btn-outline-secondary'}`

  return (
    <div className="mb-4">
      <h2 style={{ fontSize: 14, fontWeight: 600, color: '#64748b', marginBottom: 12 }}>
        Hourly usage (last {range} days)
      </h2>
      <div style={{ border: '1px solid #e2e8f0', borderRadius: 8, padding: '12px 16px', background: '#fff' }}>
        <div className="d-flex flex-wrap mb-3" style={{ gap: 8 }}>
          <div className="btn-group" role="group" aria-label="Days">
            {HEAT_RANGES.map(n => (
              <button key={n} type="button" className={toggle(range === n)} onClick={() => setRange(n)}>{n}d</button>
            ))}
          </div>
          <div className="btn-group" role="group" aria-label="Metric">
            <button type="button" className={toggle(metric === 'wh')} onClick={() => setMetric('wh')}>Wh / mean</button>
            <button type="button" className={toggle(metric === 'peak')} onClick={() => setMetric('peak')}>W peak</button>
          </div>
        </div>
        <div className="d-flex flex-wrap mb-3" style={{ gap: 6 }}>
          {nodes.map(n => (
            <button key={n.graphId} type="button" className={toggle(streamId === n.graphId)}
              onClick={() => setStreamId(n.graphId)}>
              {n.label}{n.role === 'grid' ? ' (Grid)' : ''}
            </button>
          ))}
        </div>
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
    queuedFetch('/api/nodes')
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
          <HourlyHeatmap nodes={nodes} />
          {nodes.map(n => <NodeCard key={n.graphId} node={n} date={date} />)}
        </>
      )}
    </div>
  )
}

export default Power
