import { useState, type CSSProperties } from 'react'

function yesterday() {
  const d = new Date()
  d.setDate(d.getDate() - 1)
  return d.toISOString().slice(0, 10)
}

function today() {
  return new Date().toISOString().slice(0, 10)
}

function tomorrow() {
  const d = new Date()
  d.setDate(d.getDate() + 1)
  return d.toISOString().slice(0, 10)
}

type CardState = { loading: boolean; result: string | null; error: string | null }
const idle: CardState = { loading: false, result: null, error: null }

const CARD: CSSProperties = {
  background: '#fff',
  border: '1px solid #e2e8f0',
  borderRadius: 10,
  padding: '20px 24px',
  marginBottom: 16,
}

const BTN = (loading: boolean): CSSProperties => ({
  padding: '8px 16px',
  borderRadius: 8,
  border: 'none',
  background: loading ? '#cbd5e1' : '#3b82f6',
  color: '#fff',
  fontSize: 13,
  fontWeight: 600,
  cursor: loading ? 'not-allowed' : 'pointer',
  transition: 'background .15s',
  marginLeft: 8,
})

const DATE_INPUT: CSSProperties = {
  padding: '7px 10px',
  borderRadius: 6,
  border: '1px solid #cbd5e1',
  fontSize: 13,
  color: '#1e293b',
}

function Feedback({ state }: { state: CardState }) {
  if (state.error) return (
    <div style={{ marginTop: 10, padding: '8px 12px', background: '#fef2f2', border: '1px solid #fca5a5', borderRadius: 6, fontSize: 13, color: '#b91c1c' }}>
      {state.error}
    </div>
  )
  if (state.result) return (
    <div style={{ marginTop: 10, padding: '8px 12px', background: '#f0fdf4', border: '1px solid #86efac', borderRadius: 6, fontSize: 13, color: '#166534' }}>
      {state.result}
    </div>
  )
  return null
}

function Test() {
  const [sample, setSample] = useState<CardState>(idle)
  const [sampleDate, setSampleDate] = useState(yesterday())

  const [rollup, setRollup] = useState<CardState>(idle)
  const [rollupDate, setRollupDate] = useState(yesterday())

  const [forecast, setForecast] = useState<CardState>(idle)
  const [forecastDate, setForecastDate] = useState(tomorrow())

  const [solar, setSolar] = useState<CardState>(idle)

  const [daily, setDaily] = useState<CardState>(idle)

  function triggerGenerate() {
    setSample({ loading: true, result: null, error: null })
    fetch(`/api/test/generate-sample-data?date=${sampleDate}`, { method: 'POST' })
      .then(r => r.ok ? r.json() : r.text().then(t => Promise.reject(t)))
      .then(() => setSample({ loading: false, result: `1440 minutes of sample data written for ${sampleDate}.`, error: null }))
      .catch((e: unknown) => setSample({ loading: false, result: null, error: String(e) }))
  }

  function triggerRollup() {
    setRollup({ loading: true, result: null, error: null })
    fetch(`/api/test/rollup-hourly?date=${rollupDate}`, { method: 'POST' })
      .then(r => r.ok ? r.json() : r.text().then(t => Promise.reject(t)))
      .then(() => setRollup({ loading: false, result: `Hourly rollup complete for ${rollupDate}.`, error: null }))
      .catch((e: unknown) => setRollup({ loading: false, result: null, error: String(e) }))
  }

  function triggerForecast() {
    setForecast({ loading: true, result: null, error: null })
    fetch(`/api/test/consumption-forecast/compute?date=${forecastDate}`, { method: 'POST' })
      .then(r => r.ok ? r.json() : r.text().then(t => Promise.reject(t)))
      .then(() => setForecast({ loading: false, result: `Consumption forecast computed for ${forecastDate}.`, error: null }))
      .catch((e: unknown) => setForecast({ loading: false, result: null, error: String(e) }))
  }

  function triggerDailyJob() {
    setDaily({ loading: true, result: null, error: null })
    fetch('/api/test/run-daily-job', { method: 'POST' })
      .then(r => r.ok ? r.json() : r.text().then(t => Promise.reject(t)))
      .then(() => setDaily({ loading: false, result: `Nightly forecast job complete for ${today()} (solar, consumption, surplus).`, error: null }))
      .catch((e: unknown) => setDaily({ loading: false, result: null, error: String(e) }))
  }

  function triggerSolar() {
    setSolar({ loading: true, result: null, error: null })
    fetch('/api/forecast/solar/fetch', { method: 'POST' })
      .then(r => r.ok ? r.json() as Promise<{ total_wh: number; date: string }> : r.text().then(t => Promise.reject(t)))
      .then(data => setSolar({ loading: false, result: `Fetched solar forecast for ${data.date} — ${(data.total_wh / 1000).toFixed(2)} kWh total.`, error: null }))
      .catch((e: unknown) => setSolar({ loading: false, result: null, error: String(e) }))
  }

  return (
    <div>
      <h1>Test Operations</h1>
      <p style={{ margin: '0 0 24px', fontSize: 13, color: '#64748b' }}>
        Manually invoke timed pipeline operations for a specific date.
      </p>

      {/* Run Nightly Forecast Job */}
      <div style={CARD}>
        <div style={{ fontWeight: 600, fontSize: 15, color: '#1e293b', marginBottom: 4 }}>Run Nightly Forecast Job</div>
        <div style={{ fontSize: 13, color: '#64748b', marginBottom: 12 }}>
          Trigger the full nightly pipeline for the current date: fetch the solar forecast, compute the
          consumption forecast, then derive and save the surplus forecast. Overwrites any existing files for that day.
        </div>
        <button onClick={triggerDailyJob} disabled={daily.loading} style={{ ...BTN(daily.loading), marginLeft: 0 }}>
          {daily.loading ? 'Running…' : 'Run Nightly Job'}
        </button>
        <Feedback state={daily} />
      </div>

      {/* Generate Sample Data */}
      <div style={CARD}>
        <div style={{ fontWeight: 600, fontSize: 15, color: '#1e293b', marginBottom: 4 }}>Generate Sample Data</div>
        <div style={{ fontSize: 13, color: '#64748b', marginBottom: 12 }}>
          Write 1440 synthetic 1-minute power readings for a date, ready for rollup and forecast testing.
        </div>
        <div style={{ display: 'flex', alignItems: 'center' }}>
          <input
            type="date"
            value={sampleDate}
            onChange={e => setSampleDate(e.target.value)}
            style={DATE_INPUT}
          />
          <button onClick={triggerGenerate} disabled={sample.loading} style={BTN(sample.loading)}>
            {sample.loading ? 'Generating…' : 'Generate'}
          </button>
        </div>
        <Feedback state={sample} />
      </div>

      {/* Hourly Rollup */}
      <div style={CARD}>
        <div style={{ fontWeight: 600, fontSize: 15, color: '#1e293b', marginBottom: 4 }}>Hourly Rollup</div>
        <div style={{ fontSize: 13, color: '#64748b', marginBottom: 12 }}>
          Average a day's 1-min grid records into 24 hourly buckets.
        </div>
        <div style={{ display: 'flex', alignItems: 'center' }}>
          <input
            type="date"
            value={rollupDate}
            onChange={e => setRollupDate(e.target.value)}
            style={DATE_INPUT}
          />
          <button onClick={triggerRollup} disabled={rollup.loading} style={BTN(rollup.loading)}>
            {rollup.loading ? 'Running…' : 'Run'}
          </button>
        </div>
        <Feedback state={rollup} />
      </div>

      {/* Consumption Forecast */}
      <div style={CARD}>
        <div style={{ fontWeight: 600, fontSize: 15, color: '#1e293b', marginBottom: 4 }}>Compute Consumption Forecast</div>
        <div style={{ fontSize: 13, color: '#64748b', marginBottom: 12 }}>
          Run the same-weekday baseline to predict hourly consumption for a date.
        </div>
        <div style={{ display: 'flex', alignItems: 'center' }}>
          <input
            type="date"
            value={forecastDate}
            onChange={e => setForecastDate(e.target.value)}
            style={DATE_INPUT}
          />
          <button onClick={triggerForecast} disabled={forecast.loading} style={BTN(forecast.loading)}>
            {forecast.loading ? 'Running…' : 'Run'}
          </button>
        </div>
        <Feedback state={forecast} />
      </div>

      {/* Solar Forecast */}
      <div style={CARD}>
        <div style={{ fontWeight: 600, fontSize: 15, color: '#1e293b', marginBottom: 4 }}>Fetch Solar Forecast</div>
        <div style={{ fontSize: 13, color: '#64748b', marginBottom: 12 }}>
          Pull tomorrow's PV generation estimate from Forecast.Solar.
        </div>
        <button onClick={triggerSolar} disabled={solar.loading} style={{ ...BTN(solar.loading), marginLeft: 0 }}>
          {solar.loading ? 'Fetching…' : 'Fetch'}
        </button>
        <Feedback state={solar} />
      </div>
    </div>
  )
}

export default Test
