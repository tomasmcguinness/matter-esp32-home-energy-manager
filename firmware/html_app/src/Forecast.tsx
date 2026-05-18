import { useState } from 'react'

type Estimate = { time: string; watts: number }
type ForecastResult = { date: string; estimates: Estimate[]; total_wh: number }

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

function Forecast() {
  const [loading, setLoading] = useState(false)
  const [error, setError] = useState<string | null>(null)
  const [result, setResult] = useState<ForecastResult | null>(null)

  function fetchForecast() {
    setLoading(true)
    setError(null)
    fetch('/api/forecast/solar/fetch', { method: 'POST' })
      .then(r => r.ok ? r.json() as Promise<ForecastResult> : r.text().then(t => Promise.reject(t)))
      .then(data => { setResult(data); setLoading(false) })
      .catch((e: unknown) => {
        setError(e instanceof Error ? e.message : String(e))
        setLoading(false)
      })
  }

  const totalKwh = result ? (result.total_wh / 1000).toFixed(2) : null
  const peakWatts = result ? Math.max(...result.estimates.map(e => e.watts)) : null
  const peakTime = result && peakWatts
    ? result.estimates.find(e => e.watts === peakWatts)?.time
    : null

  return (
    <div style={{ padding: '32px 40px', maxWidth: 900, margin: '0 auto' }}>
      <div style={{ display: 'flex', alignItems: 'center', justifyContent: 'space-between', marginBottom: 24 }}>
        <div>
          <h2 style={{ margin: 0, fontSize: 18, fontWeight: 700, color: '#1e293b' }}>Solar Forecast</h2>
          <p style={{ margin: '4px 0 0', fontSize: 13, color: '#64748b' }}>
            Estimated PV generation for today via Forecast.Solar
          </p>
        </div>
        <button
          onClick={fetchForecast}
          disabled={loading}
          style={{
            padding: '9px 18px',
            borderRadius: 8,
            border: 'none',
            background: loading ? '#cbd5e1' : '#f59e0b',
            color: '#fff',
            fontSize: 13,
            fontWeight: 600,
            cursor: loading ? 'not-allowed' : 'pointer',
            transition: 'background .15s',
          }}
        >
          {loading ? 'Fetching…' : 'Fetch Today\'s Forecast'}
        </button>
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
          Press the button above to fetch today's solar generation forecast.
        </div>
      )}
    </div>
  )
}

export default Forecast
