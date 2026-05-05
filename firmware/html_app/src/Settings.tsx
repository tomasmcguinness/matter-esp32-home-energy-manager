import { useEffect, useState } from 'react'

type Settings = {
  name: string
}

function Settings() {
  const [name, setName] = useState('')
  const [loading, setLoading] = useState(true)
  const [saving, setSaving] = useState(false)
  const [error, setError] = useState<string | null>(null)
  const [success, setSuccess] = useState(false)
  const [confirmReset, setConfirmReset] = useState(false)
  const [resetting, setResetting] = useState(false)
  const [resetDone, setResetDone] = useState(false)

  useEffect(() => {
    fetch('/api/settings')
      .then((r) => {
        if (!r.ok) throw new Error(`HTTP ${r.status}`)
        return r.json() as Promise<Settings>
      })
      .then((data) => {
        setName(data.name)
        setLoading(false)
      })
      .catch((e: unknown) => {
        setError(e instanceof Error ? e.message : 'Failed to load settings')
        setLoading(false)
      })
  }, [])

  function handleSubmit(e: React.FormEvent) {
    e.preventDefault()
    setSaving(true)
    setError(null)
    setSuccess(false)
    fetch('/api/settings', {
      method: 'PUT',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify({ name }),
    })
      .then((r) => {
        if (!r.ok) throw new Error(`HTTP ${r.status}`)
        return r.json() as Promise<Settings>
      })
      .then((data) => {
        setName(data.name)
        setSuccess(true)
        setSaving(false)
      })
      .catch((e: unknown) => {
        setError(e instanceof Error ? e.message : 'Failed to save settings')
        setSaving(false)
      })
  }

  function handleFactoryReset() {
    setResetting(true)
    fetch('/api/factory-reset', { method: 'POST' })
      .then((r) => { if (!r.ok) throw new Error(`HTTP ${r.status}`) })
      .then(() => setResetDone(true))
      .catch((e: unknown) => {
        setError(e instanceof Error ? e.message : 'Factory reset failed')
        setResetting(false)
        setConfirmReset(false)
      })
  }

  if (loading) return <p className="mt-3">Loading…</p>

  return (
    <>
      <h1 className="mt-3">Settings</h1>
      <hr />
      {error && <div className="alert alert-danger">{error}</div>}
      {success && <div className="alert alert-success">Settings saved.</div>}
      <form onSubmit={handleSubmit} style={{ maxWidth: 480 }}>
        <div className="mb-3">
          <label htmlFor="name" className="form-label">Device name</label>
          <input
            id="name"
            type="text"
            className="form-control"
            value={name}
            onChange={(e) => setName(e.target.value)}
            required
          />
        </div>
        <button type="submit" className="btn btn-primary" disabled={saving}>
          {saving ? 'Saving…' : 'Save'}
        </button>
      </form>

      <hr className="mt-5" />
      <h5 className="text-danger">Danger Zone</h5>
      <p className="text-muted" style={{ maxWidth: 480, fontSize: '0.9rem' }}>
        A factory reset will erase all Matter fabric credentials, device data, and settings.
        The device will restart and need to be commissioned again.
      </p>

      {resetDone ? (
        <div className="alert alert-warning">
          Factory reset initiated — the device is restarting.
        </div>
      ) : confirmReset ? (
        <div className="d-flex gap-2 align-items-center">
          <span className="text-danger fw-semibold" style={{ fontSize: '0.9rem' }}>Are you sure?</span>
          <button
            className="btn btn-danger btn-sm"
            onClick={handleFactoryReset}
            disabled={resetting}
          >
            {resetting ? 'Resetting…' : 'Yes, factory reset'}
          </button>
          <button
            className="btn btn-outline-secondary btn-sm"
            onClick={() => setConfirmReset(false)}
            disabled={resetting}
          >
            Cancel
          </button>
        </div>
      ) : (
        <button className="btn btn-outline-danger btn-sm" onClick={() => setConfirmReset(true)}>
          Factory Reset
        </button>
      )}
    </>
  )
}

export default Settings
