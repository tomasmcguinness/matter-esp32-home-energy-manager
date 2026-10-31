import { useCallback, useEffect, useMemo, useRef, useState, type ReactNode } from 'react'
import { useLocation } from 'react-router'
import { queuedFetch } from './fetchQueue'
import { deriveSetup, EMPTY_SETUP, readAdvanced, SetupContext, writeAdvanced, type Setup } from './setup'
import type { SavedEdgeConfig, SavedNodeConfig } from './topology'

// Loads the topology and device list and works out which tabs the user has
// unlocked. Re-reads on every navigation and whenever a page calls refresh()
// after changing the configuration.
function SetupProvider({ children }: { children: ReactNode }) {
  const [setup, setSetup] = useState<Setup>(() => ({ ...EMPTY_SETUP, advanced: readAdvanced() }))
  const location = useLocation()
  const latest = useRef(0)

  const refresh = useCallback(() => {
    const call = ++latest.current
    Promise.all([
      queuedFetch('/api/nodes').then(r => r.ok ? r.json() : Promise.reject(`HTTP ${r.status}`)),
      queuedFetch('/api/devices').then(r => r.ok ? r.json() : Promise.reject(`HTTP ${r.status}`)),
    ])
      .then(([graph, devs]: [{ nodes?: SavedNodeConfig[]; edges?: SavedEdgeConfig[] }, { devices?: unknown[] }]) => {
        if (call !== latest.current) return // a newer refresh superseded this one
        const derived = deriveSetup(graph.nodes ?? [], graph.edges ?? [], devs.devices?.length ?? 0)
        setSetup(prev => ({ ...prev, ...derived, loaded: true }))
      })
      .catch(() => {
        // Keep what we had, but stop waiting so hidden pages show their notice.
        // The next navigation retries.
        if (call === latest.current) setSetup(prev => ({ ...prev, loaded: true }))
      })
  }, [])

  useEffect(() => { refresh() }, [refresh, location.pathname])

  const setAdvanced = useCallback((on: boolean) => {
    writeAdvanced(on)
    setSetup(prev => ({ ...prev, advanced: on }))
  }, [])

  const value = useMemo(() => ({ ...setup, refresh, setAdvanced }), [setup, refresh, setAdvanced])
  return <SetupContext.Provider value={value}>{children}</SetupContext.Provider>
}

export default SetupProvider
