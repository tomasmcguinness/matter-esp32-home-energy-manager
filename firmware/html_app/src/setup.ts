import { createContext, useContext } from 'react'
import { connectedNodes, hasTariff, type SavedEdgeConfig, type SavedNodeConfig } from './topology'

// What the user has configured so far. Tabs appear as the home is set up, so a
// new install starts with just Home, Topology and Settings.
export type Setup = {
  loaded: boolean
  hasDevices: boolean      // at least one Matter device commissioned
  hasMeters: boolean       // anything metered wired to the consumer unit
  hasLoads: boolean        // a load on a consumer unit circuit
  hasSolarAndGrid: boolean // what the forecast and scheduler need
  hasTariff: boolean
  advanced: boolean        // per-browser opt-in to developer tabs
}

export type SetupContextValue = Setup & {
  refresh: () => void
  setAdvanced: (on: boolean) => void
}

export const EMPTY_SETUP: Setup = {
  loaded: false,
  hasDevices: false,
  hasMeters: false,
  hasLoads: false,
  hasSolarAndGrid: false,
  hasTariff: false,
  advanced: false,
}

export function deriveSetup(nodes: SavedNodeConfig[], edges: SavedEdgeConfig[], deviceCount: number): Omit<Setup, 'loaded' | 'advanced'> {
  const connected = connectedNodes(nodes, edges)
  const has = (role: string) => connected.some(n => n.role === role)
  return {
    hasDevices: deviceCount > 0,
    hasMeters: connected.length > 0,
    hasLoads: has('appliance'),
    hasSolarAndGrid: has('solar') && has('grid'),
    hasTariff: hasTariff(edges),
  }
}

const ADVANCED_KEY = 'hem.advanced'

export function readAdvanced(): boolean {
  try {
    return localStorage.getItem(ADVANCED_KEY) === '1'
  } catch {
    return false
  }
}

export function writeAdvanced(on: boolean) {
  try {
    if (on) localStorage.setItem(ADVANCED_KEY, '1')
    else localStorage.removeItem(ADVANCED_KEY)
  } catch {
    // Storage blocked: the choice just won't survive a reload.
  }
}

export const SetupContext = createContext<SetupContextValue>({
  ...EMPTY_SETUP,
  refresh: () => { },
  setAdvanced: () => { },
})

export function useSetup(): SetupContextValue {
  return useContext(SetupContext)
}
