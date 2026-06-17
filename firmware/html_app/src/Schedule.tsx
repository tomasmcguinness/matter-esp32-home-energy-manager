import { useEffect, useState } from 'react'

type SurplusSlot = { hour_ts: number; surplus_w: number }
type SurplusForecast = { date: string; slots: SurplusSlot[] }

type Run = {
  graph_id: string
  name: string
  schedulable: boolean
  start_hour: number      // -1 when not placed
  duration_hours: number
  avg_power_w: number
  energy_wh: number
  self_consumption_wh: number
}
type Schedule = { date: string; surplus_available: boolean; runs: Run[] }

// The date picker defaults to the current (local) date. Use the local calendar
// date rather than toISOString()'s UTC date so it matches the day the device
// stores its forecast/schedule files under near the midnight boundary.
function today() {
  const d = new Date()
  return new Date(d.getTime() - d.getTimezoneOffset() * 60000).toISOString().slice(0, 10)
}

const PAD = { top: 16, right: 16, bottom: 36, left: 56 }
const SVG_W = 800
const SVG_H = 240
const PLOT_W = SVG_W - PAD.left - PAD.right
const PLOT_H = SVG_H - PAD.top - PAD.bottom
const HOURS = 24

// Distinct band colours cycled across the scheduled appliances.
const BAND_COLORS = ['#3b82f6', '#8b5cf6', '#ec4899', '#0ea5e9', '#f97316', '#14b8a6']

function hhmm(hour: number) {
  return `${String(hour).padStart(2, '0')}:00`
}

function ScheduleChart({ slots, runs }: { slots: SurplusSlot[]; runs: Run[] }) {
  // Index the surplus curve by local hour so it lines up with the run bands.
  const byHour = new Array<number>(HOURS).fill(0)
  for (const s of slots) {
    const h = new Date(s.hour_ts * 1000).getHours()
    if (h >= 0 && h < HOURS) byHour[h] = s.surplus_w
  }

  const maxAbs = Math.max(...byHour.map(v => Math.abs(v)), 1)
  const barW = PLOT_W / HOURS
  const midY = PAD.top + PLOT_H / 2

  const placed = runs.filter(r => r.start_hour >= 0)

  return (
    <svg viewBox={`0 0 ${SVG_W} ${SVG_H}`} style={{ width: '100%', fontFamily: 'inherit' }}>
      <text x={PAD.left - 8} y={PAD.top} textAnchor="end" fontSize={10} fill="#94a3b8">
        +{Math.round(maxAbs / 1000 * 10) / 10}kW
      </text>
      <text x={PAD.left - 8} y={midY + 4} textAnchor="end" fontSize={10} fill="#94a3b8">0</text>
      <text x={PAD.left - 8} y={PAD.top + PLOT_H} textAnchor="end" fontSize={10} fill="#94a3b8">
        -{Math.round(maxAbs / 1000 * 10) / 10}kW
      </text>

      {/* surplus bars: green = export/available, red = import */}
      {byHour.map((w, i) => {
        const barH = Math.abs(w / maxAbs) * (PLOT_H / 2)
        const x = PAD.left + i * barW
        const y = w >= 0 ? midY - barH : midY
        return (
          <g key={i}>
            <rect x={x + 1} y={y} width={barW - 2} height={barH}
              fill={w >= 0 ? '#10b981' : '#ef4444'} rx={2} opacity={0.8} />
            {i % 3 === 0 && (
              <text x={x + barW / 2} y={PAD.top + PLOT_H + 14} textAnchor="middle" fontSize={10} fill="#94a3b8">
                {hhmm(i)}
              </text>
            )}
          </g>
        )
      })}

      {/* scheduled run windows overlaid as translucent bands */}
      {placed.map((r, k) => {
        const color = BAND_COLORS[k % BAND_COLORS.length]
        const x = PAD.left + r.start_hour * barW
        const w = r.duration_hours * barW
        return (
          <g key={r.graph_id}>
            <rect x={x} y={PAD.top} width={w} height={PLOT_H} fill={color} opacity={0.14} />
            <rect x={x} y={PAD.top} width={2} height={PLOT_H} fill={color} opacity={0.6} />
            <text x={x + 4} y={PAD.top + 12 + (k % 3) * 14} fontSize={10} fill={color} fontWeight={600}>
              {r.name}
            </text>
          </g>
        )
      })}

      <line x1={PAD.left} y1={midY} x2={PAD.left + PLOT_W} y2={midY} stroke="#e2e8f0" strokeWidth={1} />
    </svg>
  )
}

function energyLabel(wh: number) {
  return wh >= 1000 ? `${(wh / 1000).toFixed(2)} kWh` : `${Math.round(wh)} Wh`
}

function Schedule() {
  const [date, setDate] = useState(today())
  const [loading, setLoading] = useState(false)
  const [error, setError] = useState<string | null>(null)
  const [schedule, setSchedule] = useState<Schedule | null>(null)
  const [slots, setSlots] = useState<SurplusSlot[]>([])

  function load() {
    setLoading(true)
    setError(null)
    Promise.all([
      fetch(`/api/schedule?date=${date}`).then(r => r.ok ? r.json() as Promise<Schedule> : r.text().then(t => Promise.reject(t))),
      fetch(`/api/forecast/surplus?date=${date}`).then(r => {
        if (r.status === 404) return { date, slots: [] } as SurplusForecast
        return r.ok ? r.json() as Promise<SurplusForecast> : r.text().then(t => Promise.reject(t))
      }),
    ])
      .then(([sched, surplus]) => {
        setSchedule(sched)
        setSlots(surplus.slots)
        setLoading(false)
      })
      .catch((e: unknown) => { setError(String(e)); setLoading(false) })
  }

  useEffect(() => {
    load()
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, [])

  const placed = schedule?.runs.filter(r => r.start_hour >= 0) ?? []
  const noSurplus = schedule != null && !schedule.surplus_available

  return (
    <div>
      <h1 className="mb-0">Schedule</h1>
      <hr />
      <div style={{ display: 'flex', alignItems: 'center', justifyContent: 'space-between', marginBottom: 24 }}>
        <div>
          <h2 style={{ margin: 0, fontSize: 18, fontWeight: 700, color: '#1e293b' }}>Suggested Appliance Schedule</h2>
          <p style={{ margin: '4px 0 0', fontSize: 13, color: '#64748b' }}>
            When to run each appliance to soak up the predicted surplus. Green bars show available surplus; shaded bands are suggested run windows.
          </p>
        </div>
        <div style={{ display: 'flex', alignItems: 'center', gap: 8 }}>
          <input
            type="date"
            value={date}
            onChange={e => setDate(e.target.value)}
            style={{ padding: '7px 10px', borderRadius: 6, border: '1px solid #cbd5e1', fontSize: 13, color: '#1e293b' }}
          />
          <button
            onClick={load}
            disabled={loading}
            style={{
              padding: '9px 18px', borderRadius: 8, border: 'none',
              background: loading ? '#cbd5e1' : '#3b82f6', color: '#fff',
              fontSize: 13, fontWeight: 600, cursor: loading ? 'not-allowed' : 'pointer',
            }}
          >
            {loading ? 'Loading…' : 'Load'}
          </button>
        </div>
      </div>

      {error && (
        <div style={{ marginBottom: 20, padding: '10px 14px', background: '#fef2f2', border: '1px solid #fca5a5', borderRadius: 8, fontSize: 13, color: '#b91c1c' }}>
          {error}
        </div>
      )}

      {noSurplus && !loading && !error && (
        <div style={{ padding: '48px 0', textAlign: 'center', color: '#94a3b8', fontSize: 13 }}>
          No surplus forecast for this date yet. A schedule appears once the surplus forecast has been built.
        </div>
      )}

      {schedule && schedule.surplus_available && (
        <>
          <div style={{ background: '#fff', border: '1px solid #e2e8f0', borderRadius: 10, padding: '16px 20px', marginBottom: 24 }}>
            <ScheduleChart slots={slots} runs={schedule.runs} />
          </div>

          {placed.length === 0 && (
            <div style={{ marginBottom: 16, padding: '10px 14px', background: '#fffbeb', border: '1px solid #fde68a', borderRadius: 8, fontSize: 13, color: '#92400e' }}>
              No runs suggested — there's no usable surplus on this day, or no appliances have learned a run cycle yet.
            </div>
          )}

          <table style={{ width: '100%', borderCollapse: 'collapse', fontSize: 14 }}>
            <thead>
              <tr style={{ textAlign: 'left', color: '#64748b', fontSize: 12, textTransform: 'uppercase', letterSpacing: '.06em' }}>
                <th style={{ padding: '8px 12px', borderBottom: '1px solid #e2e8f0' }}>Appliance</th>
                <th style={{ padding: '8px 12px', borderBottom: '1px solid #e2e8f0' }}>Window</th>
                <th style={{ padding: '8px 12px', borderBottom: '1px solid #e2e8f0' }}>Energy</th>
                <th style={{ padding: '8px 12px', borderBottom: '1px solid #e2e8f0' }}>Surplus Covered</th>
              </tr>
            </thead>
            <tbody>
              {schedule.runs.map(r => {
                const coverage = r.energy_wh > 0 ? Math.round((r.self_consumption_wh / r.energy_wh) * 100) : 0
                if (!r.schedulable) {
                  return (
                    <tr key={r.graph_id} style={{ color: '#94a3b8' }}>
                      <td style={{ padding: '10px 12px', borderBottom: '1px solid #f1f5f9' }}>{r.name}</td>
                      <td style={{ padding: '10px 12px', borderBottom: '1px solid #f1f5f9' }} colSpan={3}>
                        Not enough run history yet
                      </td>
                    </tr>
                  )
                }
                const scheduled = r.start_hour >= 0
                return (
                  <tr key={r.graph_id} style={{ color: '#1e293b' }}>
                    <td style={{ padding: '10px 12px', borderBottom: '1px solid #f1f5f9', fontWeight: 600 }}>{r.name}</td>
                    <td style={{ padding: '10px 12px', borderBottom: '1px solid #f1f5f9' }}>
                      {scheduled
                        ? `${hhmm(r.start_hour)}–${hhmm(r.start_hour + r.duration_hours)}`
                        : <span style={{ color: '#94a3b8' }}>No surplus to use</span>}
                    </td>
                    <td style={{ padding: '10px 12px', borderBottom: '1px solid #f1f5f9' }}>{energyLabel(r.energy_wh)}</td>
                    <td style={{ padding: '10px 12px', borderBottom: '1px solid #f1f5f9' }}>
                      {scheduled ? `${coverage}%` : '—'}
                    </td>
                  </tr>
                )
              })}
            </tbody>
          </table>
        </>
      )}
    </div>
  )
}

export default Schedule
