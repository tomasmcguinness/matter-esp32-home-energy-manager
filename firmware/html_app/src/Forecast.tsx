import { useEffect, useState } from 'react'

type Estimate = { time: string; watts: number }
type ForecastResult = { date: string; estimates: Estimate[]; total_wh: number }

type SolarSlot = { hour_ts: number; power_w: number }
type SolarForecast = { date: string; slots: SolarSlot[] }

type SurplusSlot = { hour_ts: number; surplus_w: number }
type SurplusForecast = {
  date: string
  slots: SurplusSlot[]
  learning?: boolean
  usable_days?: number
  mature_days?: number
}

function today() {
  const d = new Date()
  return d.toISOString().slice(0, 10)
}

const PAD = { top: 16, right: 16, bottom: 36, left: 56 }
const SVG_W = 800
const SVG_H = 220
const PLOT_W = SVG_W - PAD.left - PAD.right
const PLOT_H = SVG_H - PAD.top - PAD.bottom

function ForecastChart({ estimates }: { estimates: Estimate[] }) {
  const nonZero = estimates.filter(e => e.watts > 0)
  if (nonZero.length === 0) {
    return <p style={{ color: '#94a3b8', fontSize: 13 }}>No generation expected today.</p>
  }

  const maxW = Math.max(...estimates.map(e => e.watts))
  const barW = PLOT_W / estimates.length

  return (
    <svg viewBox={`0 0 ${SVG_W} ${SVG_H}`} style={{ width: '100%', fontFamily: 'inherit' }}>
      {/* y-axis label */}
      <text x={PAD.left - 8} y={PAD.top} textAnchor="end" fontSize={10} fill="#94a3b8">
        {Math.round(maxW / 1000 * 10) / 10}kW
      </text>
      <text x={PAD.left - 8} y={PAD.top + PLOT_H} textAnchor="end" fontSize={10} fill="#94a3b8">0</text>

      {/* bars */}
      {estimates.map((e, i) => {
        const barH = maxW > 0 ? (e.watts / maxW) * PLOT_H : 0
        const x = PAD.left + i * barW
        const y = PAD.top + PLOT_H - barH
        const showLabel = i % Math.ceil(estimates.length / 12) === 0
        return (
          <g key={e.time}>
            <rect x={x + 1} y={y} width={barW - 2} height={barH} fill="#f59e0b" rx={2} opacity={0.85} />
            {showLabel && (
              <text x={x + barW / 2} y={PAD.top + PLOT_H + 14} textAnchor="middle" fontSize={10} fill="#94a3b8">
                {e.time.slice(0, 5)}
              </text>
            )}
          </g>
        )
      })}

      {/* baseline */}
      <line
        x1={PAD.left} y1={PAD.top + PLOT_H}
        x2={PAD.left + PLOT_W} y2={PAD.top + PLOT_H}
        stroke="#e2e8f0" strokeWidth={1}
      />
    </svg>
  )
}

function SurplusChart({ slots }: { slots: SurplusSlot[] }) {
  if (slots.length === 0) return <p style={{ color: '#94a3b8', fontSize: 13 }}>No surplus data.</p>

  const maxAbs = Math.max(...slots.map(s => Math.abs(s.surplus_w)), 1)
  const barW = PLOT_W / slots.length
  const midY = PAD.top + PLOT_H / 2

  return (
    <svg viewBox={`0 0 ${SVG_W} ${SVG_H}`} style={{ width: '100%', fontFamily: 'inherit' }}>
      <text x={PAD.left - 8} y={PAD.top} textAnchor="end" fontSize={10} fill="#94a3b8">
        +{Math.round(maxAbs / 1000 * 10) / 10}kW
      </text>
      <text x={PAD.left - 8} y={midY + 4} textAnchor="end" fontSize={10} fill="#94a3b8">0</text>
      <text x={PAD.left - 8} y={PAD.top + PLOT_H} textAnchor="end" fontSize={10} fill="#94a3b8">
        -{Math.round(maxAbs / 1000 * 10) / 10}kW
      </text>

      {slots.map((s, i) => {
        const frac = s.surplus_w / maxAbs
        const barH = Math.abs(frac) * (PLOT_H / 2)
        const x = PAD.left + i * barW
        const y = s.surplus_w >= 0 ? midY - barH : midY
        const hour = new Date(s.hour_ts * 1000).getHours()
        return (
          <g key={s.hour_ts}>
            <rect x={x + 1} y={y} width={barW - 2} height={barH}
              fill={s.surplus_w >= 0 ? '#10b981' : '#ef4444'} rx={2} opacity={0.85} />
            {i % 3 === 0 && (
              <text x={x + barW / 2} y={PAD.top + PLOT_H + 14} textAnchor="middle" fontSize={10} fill="#94a3b8">
                {String(hour).padStart(2, '0')}:00
              </text>
            )}
          </g>
        )
      })}

      <line x1={PAD.left} y1={midY} x2={PAD.left + PLOT_W} y2={midY} stroke="#e2e8f0" strokeWidth={1} />
    </svg>
  )
}

function Forecast() {
  const [loading, setLoading] = useState(false)
  const [error, setError] = useState<string | null>(null)
  const [result, setResult] = useState<ForecastResult | null>(null)

  const [surplusLoading, setSurplusLoading] = useState(false)
  const [surplusError, setSurplusError] = useState<string | null>(null)
  const [surplusResult, setSurplusResult] = useState<SurplusForecast | null>(null)
  const [surplusDate, setSurplusDate] = useState(today())

  function loadSolarForecast() {
    setLoading(true)
    setError(null)
    fetch(`/api/forecast/solar?date=${today()}`)
      .then(r => r.ok ? r.json() as Promise<SolarForecast> : r.text().then(t => Promise.reject(t)))
      .then(data => {
        if (data.slots.length === 0) { setResult(null); setLoading(false); return }
        const estimates = data.slots.map(s => ({
          time: `${String(new Date(s.hour_ts * 1000).getHours()).padStart(2, '0')}:00`,
          watts: Math.round(s.power_w),
        }))
        const total_wh = data.slots.reduce((sum, s) => sum + s.power_w, 0)
        setResult({ date: data.date, estimates, total_wh })
        setLoading(false)
      })
      .catch((e: unknown) => { setError(String(e)); setLoading(false) })
  }

  function loadSurplusForecast() {
    setSurplusLoading(true)
    setSurplusError(null)
    fetch(`/api/forecast/surplus?date=${surplusDate}`)
      .then(r => {
        // A 404 means a required forecast (solar or consumption) isn't ready yet —
        // that's the "still learning" state, not an error.
        if (r.status === 404) { setSurplusResult(null); setSurplusLoading(false); return null }
        return r.ok ? r.json() as Promise<SurplusForecast> : r.text().then(t => Promise.reject(t))
      })
      .then(data => {
        if (data === null) return
        setSurplusResult(data.slots.length > 0 ? data : null); setSurplusLoading(false)
      })
      .catch((e: unknown) => { setSurplusError(String(e)); setSurplusLoading(false) })
  }

  // Render the automatically-loaded daily forecasts as soon as the tab opens.
  useEffect(() => {
    loadSolarForecast()
    loadSurplusForecast()
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, [])

  const totalKwh = result ? (result.total_wh / 1000).toFixed(2) : null
  const peakWatts = result ? Math.max(...result.estimates.map(e => e.watts)) : null
  const peakTime = result && peakWatts
    ? result.estimates.find(e => e.watts === peakWatts)?.time
    : null

  return (
    <div>
      <h1 className="mb-0">Forecast</h1>
      <hr />
      <div style={{ display: 'flex', alignItems: 'center', justifyContent: 'space-between', marginBottom: 24 }}>
        <div>
          <h2 style={{ margin: 0, fontSize: 18, fontWeight: 700, color: '#1e293b' }}>Solar Forecast</h2>
          <p style={{ margin: '4px 0 0', fontSize: 13, color: '#64748b' }}>
            Estimated PV generation for today via Forecast.Solar
          </p>
        </div>
      </div>

      {error && (
        <div style={{ marginBottom: 20, padding: '10px 14px', background: '#fef2f2', border: '1px solid #fca5a5', borderRadius: 8, fontSize: 13, color: '#b91c1c' }}>
          {error}
        </div>
      )}

      {result && (
        <>
          <div style={{ display: 'flex', gap: 16, marginBottom: 24 }}>
            <div style={{ flex: 1, background: '#fffbeb', border: '1px solid #fde68a', borderRadius: 10, padding: '16px 20px' }}>
              <div style={{ fontSize: 11, fontWeight: 700, textTransform: 'uppercase', letterSpacing: '.08em', color: '#92400e', marginBottom: 4 }}>Total Today</div>
              <div style={{ fontSize: 24, fontWeight: 700, color: '#1e293b' }}>{totalKwh} <span style={{ fontSize: 14, fontWeight: 400, color: '#64748b' }}>kWh</span></div>
            </div>
            <div style={{ flex: 1, background: '#fffbeb', border: '1px solid #fde68a', borderRadius: 10, padding: '16px 20px' }}>
              <div style={{ fontSize: 11, fontWeight: 700, textTransform: 'uppercase', letterSpacing: '.08em', color: '#92400e', marginBottom: 4 }}>Peak Output</div>
              <div style={{ fontSize: 24, fontWeight: 700, color: '#1e293b' }}>
                {peakWatts !== null && peakWatts >= 1000
                  ? `${(peakWatts / 1000).toFixed(2)} kW`
                  : `${peakWatts} W`}
                {peakTime && <span style={{ fontSize: 13, fontWeight: 400, color: '#64748b' }}> at {peakTime}</span>}
              </div>
            </div>
            <div style={{ flex: 1, background: '#fffbeb', border: '1px solid #fde68a', borderRadius: 10, padding: '16px 20px' }}>
              <div style={{ fontSize: 11, fontWeight: 700, textTransform: 'uppercase', letterSpacing: '.08em', color: '#92400e', marginBottom: 4 }}>Date</div>
              <div style={{ fontSize: 18, fontWeight: 700, color: '#1e293b' }}>{result.date}</div>
            </div>
          </div>

          <div style={{ background: '#fff', border: '1px solid #e2e8f0', borderRadius: 10, padding: '16px 20px' }}>
            <ForecastChart estimates={result.estimates} />
          </div>
        </>
      )}

      {!result && !loading && !error && (
        <div style={{ padding: '48px 0', textAlign: 'center', color: '#94a3b8', fontSize: 13 }}>
          No stored solar forecast for today yet. Press the button above to fetch it.
        </div>
      )}

      <hr style={{ margin: '32px 0', border: 'none', borderTop: '1px solid #e2e8f0' }} />

      {/* Surplus Forecast */}
      <div style={{ display: 'flex', alignItems: 'center', justifyContent: 'space-between', marginBottom: 24 }}>
        <div>
          <h2 style={{ margin: 0, fontSize: 18, fontWeight: 700, color: '#1e293b' }}>Surplus Forecast</h2>
          <p style={{ margin: '4px 0 0', fontSize: 13, color: '#64748b' }}>
            Predicted hourly surplus, learned from the solar forecast. Green = export, red = import.
          </p>
        </div>
      </div>

      {surplusError && (
        <div style={{ marginBottom: 20, padding: '10px 14px', background: '#fef2f2', border: '1px solid #fca5a5', borderRadius: 8, fontSize: 13, color: '#b91c1c' }}>
          {surplusError}
        </div>
      )}

      {surplusResult && (() => {
        const peakSurplus = surplusResult.slots.reduce((a, b) => b.surplus_w > a.surplus_w ? b : a)
        const peakDeficit = surplusResult.slots.reduce((a, b) => b.surplus_w < a.surplus_w ? b : a)
        return (
          <>
            {surplusResult.learning && (
              <div style={{ marginBottom: 16, padding: '10px 14px', background: '#fffbeb', border: '1px solid #fde68a', borderRadius: 8, fontSize: 13, color: '#92400e' }}>
                Model still learning{
                  surplusResult.usable_days !== undefined && surplusResult.mature_days !== undefined
                    ? ` — ${surplusResult.usable_days} of ${surplusResult.mature_days} days of history`
                    : ''
                }. Predictions improve as more days are logged.
              </div>
            )}
            <div style={{ display: 'flex', gap: 16, marginBottom: 24 }}>
              <div style={{ flex: 1, background: '#f0fdf4', border: '1px solid #86efac', borderRadius: 10, padding: '16px 20px' }}>
                <div style={{ fontSize: 11, fontWeight: 700, textTransform: 'uppercase', letterSpacing: '.08em', color: '#166534', marginBottom: 4 }}>Peak Export</div>
                <div style={{ fontSize: 22, fontWeight: 700, color: '#1e293b' }}>
                  {Math.round(peakSurplus.surplus_w)} <span style={{ fontSize: 13, fontWeight: 400, color: '#64748b' }}>W at {String(new Date(peakSurplus.hour_ts * 1000).getHours()).padStart(2,'0')}:00</span>
                </div>
              </div>
              <div style={{ flex: 1, background: '#fef2f2', border: '1px solid #fca5a5', borderRadius: 10, padding: '16px 20px' }}>
                <div style={{ fontSize: 11, fontWeight: 700, textTransform: 'uppercase', letterSpacing: '.08em', color: '#991b1b', marginBottom: 4 }}>Peak Import</div>
                <div style={{ fontSize: 22, fontWeight: 700, color: '#1e293b' }}>
                  {Math.abs(Math.round(peakDeficit.surplus_w))} <span style={{ fontSize: 13, fontWeight: 400, color: '#64748b' }}>W at {String(new Date(peakDeficit.hour_ts * 1000).getHours()).padStart(2,'0')}:00</span>
                </div>
              </div>
              <div style={{ flex: 1, background: '#f8fafc', border: '1px solid #e2e8f0', borderRadius: 10, padding: '16px 20px' }}>
                <div style={{ fontSize: 11, fontWeight: 700, textTransform: 'uppercase', letterSpacing: '.08em', color: '#475569', marginBottom: 4 }}>Date</div>
                <div style={{ fontSize: 18, fontWeight: 700, color: '#1e293b' }}>{surplusResult.date}</div>
              </div>
            </div>
            <div style={{ background: '#fff', border: '1px solid #e2e8f0', borderRadius: 10, padding: '16px 20px' }}>
              <SurplusChart slots={surplusResult.slots} />
            </div>
          </>
        )
      })()}

      {!surplusResult && !surplusLoading && !surplusError && (
        <div style={{ padding: '48px 0', textAlign: 'center', color: '#94a3b8', fontSize: 13 }}>
          Surplus appears once both solar and consumption forecasts exist for this date.
        </div>
      )}
    </div>
  )
}

export default Forecast
