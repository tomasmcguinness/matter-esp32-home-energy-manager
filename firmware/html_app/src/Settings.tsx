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
    </>
  )
}

export default Settings
