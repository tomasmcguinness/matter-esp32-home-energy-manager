// The topology graph is the source of truth for which nodes exist and how they
// are wired. The handle a node connects to on the consumer unit IS its role.
export const CU_ID = 'consumer_unit'

export type NodeRole = 'grid' | 'solar' | 'appliance' | 'other'

export type SavedNodeConfig = { id: string; settings?: Record<string, unknown> }
export type SavedEdgeConfig = { id: string; source: string; target: string; sourceHandle?: string; targetHandle?: string }

export type ConnectedNode = { graphId: string; label: string; role: NodeRole }

export function roleForHandle(handle?: string | null): NodeRole {
  if (handle === 'grid') return 'grid'
  if (handle?.startsWith('solar')) return 'solar'
  if (handle?.startsWith('circuit')) return 'appliance'
  return 'other'
}

export function nodeLabel(node: SavedNodeConfig | undefined, fallback: string): string {
  if (!node) return fallback
  const name = node.settings?.name
  const label = node.settings?.label
  if (typeof name === 'string' && name) return name
  if (typeof label === 'string' && label) return label
  return fallback
}

const ROLE_ORDER: Record<NodeRole, number> = { grid: 0, solar: 1, appliance: 2, other: 3 }

// The consumer unit handle each edge touching it uses, keyed by the node on the other end.
function cuHandles(edges: SavedEdgeConfig[]): { otherId: string; cuHandle?: string }[] {
  const result: { otherId: string; cuHandle?: string }[] = []
  for (const e of edges) {
    if (e.source === CU_ID) result.push({ otherId: e.target, cuHandle: e.sourceHandle })
    else if (e.target === CU_ID) result.push({ otherId: e.source, cuHandle: e.targetHandle })
  }
  return result
}

// Derive the connected nodes (and their roles) from the consumer unit's edges.
export function connectedNodes(nodes: SavedNodeConfig[], edges: SavedEdgeConfig[]): ConnectedNode[] {
  const byId = new Map(nodes.map(n => [n.id, n]))
  const seen = new Set<string>()
  const result: ConnectedNode[] = []

  for (const { otherId, cuHandle } of cuHandles(edges)) {
    if (seen.has(otherId)) continue
    if (cuHandle === 'tariff') continue // the tariff source prices power; it doesn't meter any
    seen.add(otherId)
    result.push({
      graphId: otherId,
      label: nodeLabel(byId.get(otherId), otherId),
      role: roleForHandle(cuHandle),
    })
  }

  return result.sort((a, b) =>
    ROLE_ORDER[a.role] - ROLE_ORDER[b.role] || a.label.localeCompare(b.label))
}

// True when something is wired to the consumer unit's tariff handle.
export function hasTariff(edges: SavedEdgeConfig[]): boolean {
  return cuHandles(edges).some(h => h.cuHandle === 'tariff')
}
