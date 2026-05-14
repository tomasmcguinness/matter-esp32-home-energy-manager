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

function PowerChart({ records }: { records: PowerRecord[] }) {
  if (records.length === 0) {
    return <p style={{ color: '#94a3b8', fontSize: 13 }}>No recordings for this day.</p>
  }

  const minT = records[0].minute
  const maxT = records[records.length - 1].minute
  const spanT = maxT - minT || 1

  const rawMax = Math.max(0, ...records.map(r => r.power_w))
  const rawMin = Math.min(0, ...records.map(r => r.power_w))
  const yMax = niceExtent(rawMax) || 1000
  const yMin = niceExtent(rawMin)
  const yRange = yMax - yMin

  const xPos = (t: number) => PAD.left + ((t - minT) / spanT) * PLOT_W
  const yPos = (p: number) => PAD.top + PLOT_H - ((p - yMin) / yRange) * PLOT_H
  const zeroY = yPos(0)

  const polyPoints = records.map(r => `${xPos(r.minute).toFixed(1)},${yPos(r.power_w).toFixed(1)}`).join(' ')

  // Fill closes back through the zero axis
  const firstX = xPos(records[0].minute).toFixed(1)
  const lastX  = xPos(records[records.length - 1].minute).toFixed(1)
  const zeroYStr = zeroY.toFixed(1)
  const fillPoints = `${firstX},${zeroYStr} ${polyPoints} ${lastX},${zeroYStr}`

  // Y axis ticks — one step above and below zero
  const absExtent = Math.max(Math.abs(yMax), Math.abs(yMin))
  const yStep = absExtent <= 2000 ? 500 : absExtent <= 5000 ? 1000 : 2000
  const yTicks: { y: number; label: string }[] = []
  for (let p = yMin; p <= yMax; p += yStep)
    yTicks.push({ y: yPos(p), label: fmtW(p) })

  // X axis: one tick per hour
  const startHour = Math.ceil(minT / 3600)
  const endHour   = Math.floor(maxT / 3600)
  const xTicks: { x: number; label: string }[] = []
  for (let h = startHour; h <= endHour; h++) {
    const t = h * 3600
    if (t < minT || t > maxT) continue
    const date = new Date(t * 1000)
    const label = `${date.getHours().toString().padStart(2, '0')}:00`
    xTicks.push({ x: xPos(t), label })
  }

  return (
    <svg viewBox={`0 0 ${SVG_W} ${SVG_H}`} style={{ width: '100%', height: 'auto', display: 'block' }}>
      {/* Horizontal grid lines */}
      {yTicks.map(t => (
        <line key={t.label} x1={PAD.left} y1={t.y} x2={PAD.left + PLOT_W} y2={t.y}
          stroke={t.label === '0W' ? '#94a3b8' : '#e2e8f0'} strokeWidth={t.label === '0W' ? 1.5 : 1} />
      ))}

      {/* Filled area (between line and zero axis) */}
      <polygon points={fillPoints} fill="rgba(59,130,246,0.08)" />

      {/* Power line */}
      <polyline points={polyPoints} fill="none" stroke="#3b82f6" strokeWidth={1.5}
        strokeLinejoin="round" strokeLinecap="round" />

      {/* Y axis */}
      <line x1={PAD.left} y1={PAD.top} x2={PAD.left} y2={PAD.top + PLOT_H}
        stroke="#cbd5e1" strokeWidth={1} />
      {yTicks.map(t => (
        <text key={t.label} x={PAD.left - 6} y={t.y + 4}
          textAnchor="end" fontSize={10} fill="#94a3b8">{t.label}</text>
      ))}

      {/* X axis */}
      <line x1={PAD.left} y1={PAD.top + PLOT_H} x2={PAD.left + PLOT_W} y2={PAD.top + PLOT_H}
        stroke="#cbd5e1" strokeWidth={1} />
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

function Data() {
  const [searchParams, setSearchParams] = useSearchParams()
  const date = searchParams.get('date') ?? todayString()

  function setDate(value: string) {
    setSearchParams(prev => { prev.set('date', value); return prev }, { replace: true })
  }
  const [records, setRecords] = useState<PowerRecord[]>([])
  const [loading, setLoading] = useState(true)
  const [error, setError] = useState<string | null>(null)

  useEffect(() => {
    setLoading(true)
    setError(null)
    fetch(`/api/data/grid?date=${date}`)
      .then(r => r.ok ? r.json() : Promise.reject(`HTTP ${r.status}`))
      .then((data: { records: PowerRecord[] }) => {
        setRecords(data.records)
        setLoading(false)
      })
      .catch((e: unknown) => {
        setError(e instanceof Error ? e.message : String(e))
        setLoading(false)
      })
  }, [date])

  return (
    <div className="container">
      <div className="mt-3 mb-2 d-flex align-items-center justify-content-between">
        <h1 className="mb-0">Data</h1>
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

      <h2 style={{ fontSize: 14, fontWeight: 600, color: '#64748b', marginBottom: 12 }}>Grid Power</h2>
      {loading ? (
        <p style={{ color: '#94a3b8', fontSize: 13 }}>Loading…</p>
      ) : (
        <div style={{ border: '1px solid #e2e8f0', borderRadius: 8, padding: '12px 16px', background: '#fff' }}>
          <PowerChart records={records} />
        </div>
      )}
    </div>
  )
}

export default Data
