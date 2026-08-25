import { useState } from 'react'

type ResetState = 'idle' | 'confirm' | 'resetting' | 'done' | 'error'

function Settings() {
  const [resetState, setResetState] = useState<ResetState>('idle')

  function handleResetClick() {
    setResetState('confirm')
  }

  function handleConfirm() {
    setResetState('resetting')
    fetch('/api/factory-reset', { method: 'POST' })
      .then(r => {
        if (!r.ok) throw new Error(`HTTP ${r.status}`)
        setResetState('done')
      })
      .catch(() => setResetState('error'))
  }

  function handleCancel() {
    setResetState('idle')
  }

  return (
    <>
      <div className="mt-3 mb-2">
        <h1 className="mb-0">Settings</h1>
      </div>
      <hr />

      <div style={{ border: '1px solid #f5c2c7', borderRadius: 6, overflow: 'hidden' }}>
        <div style={{ background: '#f8d7da', padding: '10px 16px', borderBottom: '1px solid #f5c2c7' }}>
          <strong style={{ color: '#842029' }}>Danger Zone</strong>
        </div>
        <div style={{ padding: '16px' }}>
          <div style={{ display: 'flex', alignItems: 'center', justifyContent: 'space-between', gap: 16 }}>
            <div>
              <div style={{ fontWeight: 500 }}>Factory Reset</div>
              <div style={{ fontSize: '0.85rem', color: '#6c757d', marginTop: 2 }}>
                Removes all devices, canvas layout, and Matter fabric data. The device will reboot.
              </div>
            </div>
            {resetState === 'idle' && (
              <button className="btn btn-danger btn-sm" style={{ flexShrink: 0 }} onClick={handleResetClick}>
                Factory Reset
              </button>
            )}
            {resetState === 'confirm' && (
              <div style={{ display: 'flex', gap: 8, flexShrink: 0 }}>
                <button className="btn btn-outline-secondary btn-sm" onClick={handleCancel}>Cancel</button>
                <button className="btn btn-danger btn-sm" onClick={handleConfirm}>Yes, reset everything</button>
              </div>
            )}
            {resetState === 'resetting' && (
              <span style={{ fontSize: '0.85rem', color: '#6c757d', flexShrink: 0 }}>Resetting…</span>
            )}
            {resetState === 'done' && (
              <span style={{ fontSize: '0.85rem', color: '#198754', flexShrink: 0 }}>Done — device is rebooting</span>
            )}
            {resetState === 'error' && (
              <span style={{ fontSize: '0.85rem', color: '#dc3545', flexShrink: 0 }}>Reset failed</span>
            )}
          </div>
        </div>
      </div>
    </>
  )
}

export default Settings
