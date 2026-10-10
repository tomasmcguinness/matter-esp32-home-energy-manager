import { useCallback, useEffect, useState, type FormEvent, type ReactNode } from 'react'
import { useWebSocket, type WsMessage } from './useWebSocket'

export type OpenAdrConfig = {
  enabled: boolean
  vtn_base_url: string
  client_id: string
  secret_set: boolean
  ven_name: string
  forecast_source: 'net' | 'gross'
  mqtt_host_override: string
}

export type OpenAdrActivity = { ts: number; kind: string; text: string }

export type OpenAdrStatus = {
  state: string
  last_error: string
  ven_id: string
  program_id: string
  program_name: string
  event_id: string
  event_found: boolean
  mqtt_connected: boolean
  last_report_ts: number
  last_report_http: number
  next_report_ts: number
  retry_ts: number
  forecast_source_used: string
  activity: OpenAdrActivity[]   // newest first
}

// How often to fetch the status while the websocket is not connected.
const STATUS_POLL_MS = 5000

type SaveState = 'idle' | 'saving' | 'saved' | 'error'

const STATE_BADGE: Record<string, string> = {
  RUNNING: 'text-bg-success',
  DISABLED: 'text-bg-secondary',
  BACKOFF: 'text-bg-danger',
}

const KIND_COLOR: Record<string, string> = {
  error: '#dc3545',
  report: '#198754',
  mqtt: '#0d6efd',
  state: '#475569',
  info: '#64748b',
}

const SOURCE_LABEL: Record<string, string> = {
  net: 'Net grid',
  gross: 'Gross consumption',
  gross_fallback: 'Gross consumption (no surplus forecast for today)',
}

function formatTime(ts: number) {
  return ts > 0 ? new Date(ts * 1000).toLocaleString() : '—'
}

function Card({ title, children }: { title: string; children: ReactNode }) {
  return (
    <div style={{ border: '1px solid #e2e8f0', borderRadius: 10, padding: '18px 20px', marginBottom: 20 }}>
      <h2 style={{ margin: '0 0 16px', fontSize: 17, fontWeight: 700, color: '#1e293b' }}>{title}</h2>
      {children}
    </div>
  )
}

function Row({ label, children }: { label: string; children: ReactNode }) {
  return (
    <div style={{ display: 'flex', gap: 16, padding: '6px 0', borderBottom: '1px solid #f1f5f9' }}>
      <div style={{ width: 150, flexShrink: 0, fontSize: '0.85rem', color: '#64748b' }}>{label}</div>
      <div style={{ minWidth: 0, overflowWrap: 'anywhere' }}>{children}</div>
    </div>
  )
}

function ConfigCard() {
  const [config, setConfig] = useState<OpenAdrConfig | null>(null)
  const [secret, setSecret] = useState('')
  const [saveState, setSaveState] = useState<SaveState>('idle')
  const [loadFailed, setLoadFailed] = useState(false)

  useEffect(() => {
    fetch('/api/openadr/config')
      .then(r => {
        if (!r.ok) throw new Error(`HTTP ${r.status}`)
        return r.json() as Promise<OpenAdrConfig>
      })
      .then(setConfig)
      .catch(() => setLoadFailed(true))
  }, [])

  if (loadFailed) return <Card title="Configuration"><span style={{ color: '#dc3545' }}>Could not load the configuration.</span></Card>
  if (!config) return <Card title="Configuration"><span style={{ color: '#6c757d' }}>Loading…</span></Card>

  function update<K extends keyof OpenAdrConfig>(key: K, value: OpenAdrConfig[K]) {
    setConfig(c => (c ? { ...c, [key]: value } : c))
    setSaveState('idle')
  }

  function handleSubmit(e: FormEvent) {
    e.preventDefault()
    if (!config) return
    setSaveState('saving')
    // An empty client_secret keeps the stored one; the device ignores secret_set.
    fetch('/api/openadr/config', {
      method: 'PUT',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify({ ...config, client_secret: secret }),
    })
      .then(r => {
        if (!r.ok) throw new Error(`HTTP ${r.status}`)
        return r.json() as Promise<OpenAdrConfig>
      })
      .then(saved => {
        setConfig(saved)
        setSecret('')
        setSaveState('saved')
      })
      .catch(() => setSaveState('error'))
  }

  return (
    <Card title="Configuration">
      <form onSubmit={handleSubmit}>
        <div className="form-check form-switch mb-3">
          <input className="form-check-input" type="checkbox" role="switch" id="openadr-enabled"
            checked={config.enabled} onChange={e => update('enabled', e.target.checked)} />
          <label className="form-check-label" htmlFor="openadr-enabled">Enabled</label>
        </div>

        <div className="mb-3">
          <label className="form-label" htmlFor="openadr-url">VTN base URL</label>
          <input className="form-control" id="openadr-url" type="url" placeholder="http://192.168.1.10:5000"
            value={config.vtn_base_url} onChange={e => update('vtn_base_url', e.target.value)} />
          <div className="form-text">Plain http:// only.</div>
        </div>

        <div className="row">
          <div className="col-md-6 mb-3">
            <label className="form-label" htmlFor="openadr-client-id">Client ID</label>
            <input className="form-control" id="openadr-client-id" autoComplete="off"
              value={config.client_id} onChange={e => update('client_id', e.target.value)} />
          </div>
          <div className="col-md-6 mb-3">
            <label className="form-label" htmlFor="openadr-secret">Client secret</label>
            <input className="form-control" id="openadr-secret" type="password" autoComplete="new-password"
              placeholder={config.secret_set ? 'unchanged' : ''}
              value={secret} onChange={e => { setSecret(e.target.value); setSaveState('idle') }} />
          </div>
        </div>

        <div className="row">
          <div className="col-md-6 mb-3">
            <label className="form-label" htmlFor="openadr-ven-name">VEN name</label>
            <input className="form-control" id="openadr-ven-name"
              value={config.ven_name} onChange={e => update('ven_name', e.target.value)} />
            <div className="form-text">Leave empty to use the default derived from the Ethernet MAC.</div>
          </div>
          <div className="col-md-6 mb-3">
            <label className="form-label" htmlFor="openadr-source">Forecast source</label>
            <select className="form-select" id="openadr-source" value={config.forecast_source}
              onChange={e => update('forecast_source', e.target.value as OpenAdrConfig['forecast_source'])}>
              <option value="net">Net grid</option>
              <option value="gross">Gross consumption</option>
            </select>
          </div>
        </div>

        <div className="mb-3">
          <label className="form-label" htmlFor="openadr-mqtt-host">MQTT host override <span style={{ color: '#6c757d' }}>(optional)</span></label>
          <input className="form-control" id="openadr-mqtt-host" placeholder="Taken from the VTN when empty"
            value={config.mqtt_host_override} onChange={e => update('mqtt_host_override', e.target.value)} />
        </div>

        <div style={{ display: 'flex', alignItems: 'center', gap: 12 }}>
          <button className="btn btn-primary btn-sm" type="submit" disabled={saveState === 'saving'}>Save</button>
          {saveState === 'saving' && <span style={{ fontSize: '0.85rem', color: '#6c757d' }}>Saving…</span>}
          {saveState === 'saved' && <span style={{ fontSize: '0.85rem', color: '#198754' }}>Saved — the VEN is restarting</span>}
          {saveState === 'error' && <span style={{ fontSize: '0.85rem', color: '#dc3545' }}>Save failed — check the values</span>}
        </div>
      </form>
    </Card>
  )
}

function OpenAdr() {
  const [status, setStatus] = useState<OpenAdrStatus | null>(null)
  const [actionError, setActionError] = useState('')

  // The firmware pushes a fresh status over the websocket whenever the state or
  // the activity log changes.
  const handleWsMessage = useCallback((msg: WsMessage) => {
    if (msg.type === 'openadr_status') setStatus(msg.data as OpenAdrStatus)
  }, [])
  const wsState = useWebSocket(handleWsMessage)

  // Fetch on load and whenever the socket (re)opens, to pick up anything pushed
  // while it was down. While the socket is not open, poll instead so the page
  // still works without live updates.
  useEffect(() => {
    const load = () => {
      fetch('/api/openadr/status')
        .then(r => (r.ok ? (r.json() as Promise<OpenAdrStatus>) : Promise.reject(new Error(`HTTP ${r.status}`))))
        .then(setStatus)
        .catch(() => { /* keep showing the last known status */ })
    }
    load()
    if (wsState === 'open') return
    const timer = setInterval(load, STATUS_POLL_MS)
    return () => clearInterval(timer)
  }, [wsState])

  function post(path: string) {
    setActionError('')
    fetch(path, { method: 'POST' })
      .then(r => { if (!r.ok) throw new Error(`HTTP ${r.status}`) })
      .catch(() => setActionError('Request failed'))
  }

  const disabled = !status || status.state === 'DISABLED'

  return (
    <>
      <div className="mt-3 mb-2">
        <h1 className="mb-0">OpenADR</h1>
      </div>
      <hr />

      <ConfigCard />

      <Card title="Status">
        {!status && <span style={{ color: '#6c757d' }}>Loading…</span>}
        {status && (
          <>
            <Row label="State">
              <span className={`badge ${STATE_BADGE[status.state] ?? 'text-bg-warning'}`}>{status.state}</span>
              {status.state === 'BACKOFF' && status.retry_ts > 0 && (
                <span style={{ marginLeft: 8, fontSize: '0.85rem', color: '#6c757d' }}>retry at {formatTime(status.retry_ts)}</span>
              )}
            </Row>
            {status.last_error && (
              <Row label="Last error"><span style={{ color: '#dc3545' }}>{status.last_error}</span></Row>
            )}
            <Row label="VEN ID">{status.ven_id || '—'}</Row>
            <Row label="Program">{status.program_name || status.program_id || '—'}</Row>
            <Row label="Forecast event">{status.event_found ? status.event_id : 'Not found'}</Row>
            <Row label="MQTT">
              <span className={`badge ${status.mqtt_connected ? 'text-bg-success' : 'text-bg-secondary'}`}>
                {status.mqtt_connected ? 'Connected' : 'Not connected'}
              </span>
            </Row>
            <Row label="Last report">
              {status.last_report_ts > 0 ? `${formatTime(status.last_report_ts)} — HTTP ${status.last_report_http}` : '—'}
            </Row>
            <Row label="Next report due">{formatTime(status.next_report_ts)}</Row>
            <Row label="Forecast source used">{SOURCE_LABEL[status.forecast_source_used] ?? '—'}</Row>
          </>
        )}

        <div style={{ display: 'flex', alignItems: 'center', gap: 8, marginTop: 16 }}>
          <button className="btn btn-primary btn-sm" disabled={disabled} onClick={() => post('/api/openadr/send-now')}>
            Send forecast now
          </button>
          <button className="btn btn-outline-danger btn-sm" disabled={disabled} onClick={() => post('/api/openadr/reset')}>
            Reset registration
          </button>
          {actionError && <span style={{ fontSize: '0.85rem', color: '#dc3545' }}>{actionError}</span>}
          {wsState !== 'open' && <span style={{ fontSize: '0.85rem', color: '#6c757d' }}>Live updates reconnecting — refreshing every 5 s</span>}
        </div>
      </Card>

      <Card title="Activity">
        {(!status || status.activity.length === 0) && <span style={{ color: '#6c757d' }}>Nothing yet.</span>}
        {status && status.activity.map((a, i) => (
          <div key={`${a.ts}-${i}`} style={{ display: 'flex', gap: 12, padding: '5px 0', borderBottom: '1px solid #f1f5f9', fontSize: '0.875rem' }}>
            <span style={{ width: 80, flexShrink: 0, color: '#94a3b8', fontVariantNumeric: 'tabular-nums' }}>
              {new Date(a.ts * 1000).toLocaleTimeString()}
            </span>
            <span style={{ width: 56, flexShrink: 0, fontSize: 11, fontWeight: 700, textTransform: 'uppercase', letterSpacing: '.06em', color: KIND_COLOR[a.kind] ?? '#64748b', paddingTop: 2 }}>
              {a.kind}
            </span>
            <span style={{ minWidth: 0, overflowWrap: 'anywhere' }}>{a.text}</span>
          </div>
        ))}
      </Card>
    </>
  )
}

export default OpenAdr
