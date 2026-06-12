import { ReactFlow, ReactFlowProvider, Background, BackgroundVariant, useNodesState, useEdgesState, type Node, type Edge, type EdgeChange, type ReactFlowInstance, addEdge, useReactFlow, Handle, Position } from '@xyflow/react'
import { useCallback, useEffect, useRef, useState } from 'react'
import { PowerFlowEdge } from './PowerFlowEdge'
import { GridModal } from './GridModal'
import { AddLoadModal, type AddedLoad } from './AddLoadModal'
import { useWebSocket, type WsMessage } from './useWebSocket'
import { DnDProvider, useDnD } from './DnDContext';

const edgeTypes = { powerFlow: PowerFlowEdge }

function fmt(value: number | undefined, unit: string): string {
  return value === undefined ? '—' : `${(value/1000).toFixed(1)} ${unit}`
}

// One output circuit per appliance slot on the Home screen.
const CU_CIRCUITS = [0, 1, 2, 3, 4]

function ConsumerUnitNode({ data }: { data: { label: string } }) {
  return (
    <>
      <Handle type="target" position={Position.Left} id="grid" />
      <Handle type="target" position={Position.Left} id="solar_input" />
      <div style={{ padding: '5px 12px', fontSize: 13, fontWeight: 500, color: '#1e293b', whiteSpace: 'nowrap' }}>
        {data.label}
      </div>
      {CU_CIRCUITS.map((slot, i) => (
        <Handle
          key={slot}
          type="source"
          position={Position.Right}
          id={`circuit_${slot + 1}`}
          style={{ top: `${((i + 1) * 100) / (CU_CIRCUITS.length + 1)}%` }}
        />
      ))}
    </>
  )
}

type PowerMeasurement = { voltage?: number, current?: number, power?: number }
type DeviceNodeData = { label: string; nodeId?: number; endpointId?: number; power?: PowerMeasurement; batteryPercent?: number }

// ElectricalPowerMeasurement cluster (0x0090) and its attribute ids.
const EPM_CLUSTER = 144
const EPM_VOLTAGE = 0x04
const EPM_CURRENT = 0x05
const EPM_POWER = 0x08

// Power Source cluster (0x002F). BatPercentRemaining is a nullable uint8 in
// half-percent units (0..200), so state of charge = value / 2.
const POWER_SOURCE_CLUSTER = 0x2f
const BAT_PERCENT_REMAINING = 0x0c

// A cached attribute value as delivered by /api/nodes and by websocket updates.
type ValueEntry = { clusterId: number; attributeId: number; value: number }

// Map a single attribute (cluster + attribute id) to the PowerMeasurement field it sets.
// Shared by the initial /api/nodes hydration and live websocket updates.
function powerFromAttribute(clusterId: number, attributeId: number, value: number): PowerMeasurement {
  const pm: PowerMeasurement = {}
  if (clusterId === EPM_CLUSTER) {
    if (attributeId === EPM_VOLTAGE) pm.voltage = value
    else if (attributeId === EPM_CURRENT) pm.current = value
    else if (attributeId === EPM_POWER) pm.power = value
  }
  return pm
}

// Map a Power Source BatPercentRemaining report to a 0..100 state of charge, or
// undefined for any other attribute. Half-percent units are halved and clamped.
function batteryPercentFromAttribute(clusterId: number, attributeId: number, value: number): number | undefined {
  if (clusterId === POWER_SOURCE_CLUSTER && attributeId === BAT_PERCENT_REMAINING) {
    console.log('PowerSource update received!')
    return Math.max(0, Math.min(100, value / 2))
  }
  return undefined
}

function DeviceNode({ data }: { data: DeviceNodeData }) {

  return (
    <>
      <Handle type="target" position={Position.Left} id="power-in" />
      <div style={{ padding: '4px 10px', background: '#f1f5f9', borderBottom: '1px solid #e2e8f0', fontSize: 12, fontWeight: 600, color: '#1e293b', whiteSpace: 'nowrap' }}>
        0x{data.nodeId?.toString(16).toUpperCase()} | {data.endpointId} | {data.label}
      </div>

      <div style={{ minWidth: '100px', padding: '5px 10px', display: 'grid', gridTemplateColumns: 'auto 1fr', columnGap: 8, rowGap: 2, fontSize: 12 }}>
        <span style={{ color: '#94a3b8' }}>V</span><span style={{textAlign: 'right'}}>{fmt(data.power?.voltage, 'V')}</span>
        <span style={{ color: '#94a3b8' }}>I</span><span style={{textAlign: 'right'}}>{fmt(data.power?.current, 'A')}</span>
        <span style={{ color: '#94a3b8' }}>P</span><span style={{textAlign: 'right'}}>{fmt(data.power?.power, 'W')}</span>
      </div>
      <Handle type="source" position={Position.Right} id="power-out" />
    </>
  )
}

// The Solar Power inverter. PV strings feed DC power into `dc_in` (left); the
// battery hangs off `battery` (bottom); AC output leaves via `power-out` (right)
// into the consumer unit's solar_input. Body shows the inverter's own AC power.
function SolarInverterNode({ data }: { data: DeviceNodeData }) {
  return (
    <>
      <Handle type="target" position={Position.Left} id="dc_in" />
      <Handle type="source" position={Position.Bottom} id="battery" />
      <div style={{ padding: '4px 10px', background: '#fef9c3', borderBottom: '1px solid #fde68a', fontSize: 12, fontWeight: 600, color: '#1e293b', whiteSpace: 'nowrap' }}>
        ☀ {data.label}
      </div>
      <div style={{ minWidth: '100px', padding: '5px 10px', display: 'grid', gridTemplateColumns: 'auto 1fr', columnGap: 8, rowGap: 2, fontSize: 12 }}>
        <span style={{ color: '#94a3b8' }}>V</span><span style={{textAlign: 'right'}}>{fmt(data.power?.voltage, 'V')}</span>
        <span style={{ color: '#94a3b8' }}>P</span><span style={{textAlign: 'right'}}>{fmt(data.power?.power, 'W')}</span>
      </div>
      <Handle type="source" position={Position.Right} id="power-out" />
    </>
  )
}

// A single PV string / MPPT input. Shows live DC generation flowing into the inverter.
function PvStringNode({ data }: { data: DeviceNodeData }) {
  return (
    <>
      <div style={{ padding: '4px 10px', background: '#dcfce7', borderBottom: '1px solid #bbf7d0', fontSize: 12, fontWeight: 600, color: '#166534', whiteSpace: 'nowrap' }}>
        ▦ {data.label}
      </div>
      <div style={{ minWidth: '90px', padding: '5px 10px', display: 'flex', justifyContent: 'space-between', gap: 8, fontSize: 12 }}>
        <span style={{ color: '#94a3b8' }}>DC</span><span>{fmt(data.power?.power, 'W')}</span>
      </div>
      <Handle type="source" position={Position.Right} id="power-out" />
    </>
  )
}

// The home battery, hanging off the inverter. Renders charge/discharge power and
// direction from the sign of ActivePower. Convention: positive = discharging to
// the house, negative = charging. Flip here if the inverter reports the opposite.
function BatteryNode({ data }: { data: DeviceNodeData }) {
  const w = data.power?.power
  let label = 'Idle'
  let color = '#94a3b8'
  let bg = '#f1f5f9'
  if (w !== undefined && w !== 0) {
    const discharging = w > 0
    label = discharging ? `Discharging ${fmt(Math.abs(w), 'W')}` : `Charging ${fmt(Math.abs(w), 'W')}`
    color = discharging ? '#a32d2d' : '#3b6d11'
    bg = discharging ? '#fcebeb' : '#eaf3de'
  }
  // State of charge gauge. Fill colour tracks how full the battery is, reusing
  // the green/amber/red palette already used for charge/discharge above.
  const pct = data.batteryPercent
  const socColor = pct === undefined ? '#94a3b8' : pct >= 50 ? '#3b6d11' : pct >= 20 ? '#b45309' : '#a32d2d'
  return (
    <>
      <Handle type="target" position={Position.Top} id="power-in" />
      <div style={{ padding: '4px 10px', background: '#e0e7ff', borderBottom: '1px solid #c7d2fe', fontSize: 12, fontWeight: 600, color: '#1e293b', whiteSpace: 'nowrap' }}>
        🔋 {data.label}
      </div>
      {pct !== undefined && (
        <div style={{ padding: '6px 10px 0', textAlign: 'center' }}>
          <div style={{ fontSize: 20, fontWeight: 700, color: socColor, lineHeight: 1 }}>{Math.round(pct)}%</div>
          <div style={{ marginTop: 5, height: 8, borderRadius: 4, background: '#e2e8f0', overflow: 'hidden' }}>
            <div style={{ width: `${pct}%`, height: '100%', background: socColor, borderRadius: 4, transition: 'width .4s ease' }} />
          </div>
        </div>
      )}
      <div style={{ minWidth: '120px', padding: '6px 10px', fontSize: 12, fontWeight: 600, color, background: bg, borderRadius: 4, margin: 6, textAlign: 'center' }}>
        {label}
      </div>
    </>
  )
}

// A Sub Consumer Unit: a sub-distribution board fed from one incoming feed
// (power-in, left) that fans out across several circuit handles (circuit_1..
// circuit_N, right) which loads hang off. This mirrors a real installation where
// a Henley splice feeds a sub-board and circuits branch from there. The circuit
// count is configurable via the +/- buttons and uses the same `circuit_N` handle
// ids as the main consumer unit, so the firmware's appliance detection treats
// loads here identically to loads on the main board.
function SubConsumerUnitNode({ id, data }: { id: string; data: { label: string; circuits?: number } }) {
  const { updateNodeData } = useReactFlow()
  const circuits = data.circuits ?? 2

  const setCircuits = (n: number) => {
    const next = Math.max(1, Math.min(8, n))
    if (next === circuits) return
    updateNodeData(id, { circuits: next })
    // Settings endpoint takes the raw settings object; backend replaces it
    // wholesale, so send every field.
    fetch(`/api/nodes/${id}/settings`, {
      method: 'PUT',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify({ label: data.label, type: 'subConsumerUnit', circuits: next }),
    }).catch(() => { })
  }

  const btn: React.CSSProperties = {
    width: 18, height: 18, lineHeight: '14px', padding: 0,
    border: '1px solid #e2e8f0', borderRadius: 4, background: '#fff', cursor: 'pointer',
  }

  return (
    <>
      <Handle type="target" position={Position.Left} id="power-in" />
      <div style={{ padding: '5px 10px', background: '#ecfeff', border: '1px solid #67e8f9', borderRadius: 4, fontSize: 12, fontWeight: 600, color: '#1e293b', whiteSpace: 'nowrap', display: 'flex', alignItems: 'center', gap: 8 }}>
        <span>▦ {data.label}</span>
        <span className="nodrag" style={{ display: 'flex', gap: 2 }}>
          <button style={btn} onClick={() => setCircuits(circuits - 1)} disabled={circuits <= 1} title="Remove circuit">−</button>
          <button style={btn} onClick={() => setCircuits(circuits + 1)} disabled={circuits >= 8} title="Add circuit">+</button>
        </span>
      </div>
      {Array.from({ length: circuits }, (_, i) => (
        <Handle
          key={i}
          type="source"
          position={Position.Right}
          id={`circuit_${i + 1}`}
          style={{ top: `${((i + 1) * 100) / (circuits + 1)}%` }}
        />
      ))}
    </>
  )
}

// Appliance nodes are functionally identical to device nodes on the canvas (a metered
// endpoint with a power-in handle); they only differ by their semantic role/type.
const nodeTypes = {
  consumerUnit: ConsumerUnitNode,
  device: DeviceNode,
  appliance: DeviceNode,
  solarInverter: SolarInverterNode,
  pvString: PvStringNode,
  battery: BatteryNode,
  subConsumerUnit: SubConsumerUnitNode,
  // Legacy: graphs saved before the sub-CU replaced the Henley block render with
  // the same component so they still display.
  henley: SubConsumerUnitNode,
}

type SavedNodeConfig = { id: string; x: number; y: number; settings: Record<string, unknown>; values?: ValueEntry[] }
type SavedEdgeConfig = { id: string; source: string; target: string; sourceHandle?: string; targetHandle?: string }

// Which node's measurement drives an edge's power label. A source node's reading
// is the flow on the edge only when it leaves via the source's `power-out` handle.
// Anything leaving a different handle (e.g. the inverter's `battery` tap) is
// metered by the node at the other end, so we use the target there instead. This
// stops a node that fans out across several handles (inverter → CU and
// inverter → battery) from stamping its single reading onto every outgoing edge.
function edgePowerNodeId(e: { source: string; sourceHandle?: string | null; target: string }): string {
  return e.sourceHandle === 'power-out' ? e.source : e.target
}

type DeviceSpec = {
  nodeId: number,
  endpointId: number,
  label: string
}

const initialNodes: Node[] = []

const initialEdges: Edge[] = []

const WS_DOT: Record<string, { color: string; title: string }> = {
  open: { color: '#22c55e', title: 'Live' },
  connecting: { color: '#f59e0b', title: 'Connecting…' },
  closed: { color: '#94a3b8', title: 'Disconnected' },
}

function WsStatusDot({ state }: { state: string }) {
  const { color, title } = WS_DOT[state] ?? WS_DOT.closed
  return (
    <div title={title} style={{ position: 'absolute', bottom: 12, right: 12, zIndex: 10, display: 'flex', alignItems: 'center', gap: 6, background: 'rgba(255,255,255,.85)', borderRadius: 8, padding: '4px 8px', fontSize: 11, color: '#64748b', backdropFilter: 'blur(4px)', boxShadow: '0 1px 4px rgba(0,0,0,.08)' }}>
      <span style={{ width: 8, height: 8, borderRadius: '50%', background: color, display: 'inline-block' }} />
      {title}
    </div>
  )
}

type Toast = { id: number; message: string }

function ToastStack({ toasts, onDismiss }: { toasts: Toast[]; onDismiss: (id: number) => void }) {
  return (
    <div style={{ position: 'fixed', bottom: 48, left: '50%', transform: 'translateX(-50%)', zIndex: 100, display: 'flex', flexDirection: 'column', gap: 8, alignItems: 'center', pointerEvents: 'none' }}>
      {toasts.map(t => (
        <div key={t.id} style={{ display: 'flex', alignItems: 'center', gap: 10, background: '#1e293b', color: '#f8fafc', borderRadius: 10, padding: '10px 16px', fontSize: 13, boxShadow: '0 4px 16px rgba(0,0,0,.18)', pointerEvents: 'auto', minWidth: 260, maxWidth: 400 }}>
          <span style={{ fontSize: 16 }}>🔌</span>
          <span style={{ flex: 1 }}>{t.message}</span>
          <button onClick={() => onDismiss(t.id)} style={{ background: 'none', border: 'none', color: '#94a3b8', cursor: 'pointer', fontSize: 16, lineHeight: 1, padding: 0 }}>×</button>
        </div>
      ))}
    </div>
  )
}

function Topology() {
  const [nodes, setNodes, onNodesChange] = useNodesState(initialNodes)
  const [edges, setEdges, onEdgesChangeBase] = useEdgesState(initialEdges)
  const [gridModalOpen, setGridModalOpen] = useState(false)
  const [nodeMenu, setNodeMenu] = useState<{ node: Node; x: number; y: number } | null>(null)
  const [paneMenu, setPaneMenu] = useState<{ x: number; y: number } | null>(null)
  // Flow-space position where an "Add load" node should be created (captured from
  // the right-click), held while the picker modal is open.
  const [pendingLoadPos, setPendingLoadPos] = useState<{ x: number; y: number } | null>(null)
  const [toasts, setToasts] = useState<Toast[]>([])
  const reactFlowInstance = useRef<ReactFlowInstance | null>(null)
  const nodeIdCounter = useRef(10)
  const toastIdCounter = useRef(0)
  const { screenToFlowPosition, getNodes } = useReactFlow();
  const [type] = useDnD();

  const dismissToast = useCallback((id: number) => {
    setToasts(prev => prev.filter(t => t.id !== id))
  }, [])

  const addToast = useCallback((message: string) => {
    const id = ++toastIdCounter.current
    setToasts(prev => [...prev, { id, message }])
    setTimeout(() => dismissToast(id), 5000)
  }, [dismissToast])

  const handleWsMessage = useCallback((msg: WsMessage) => {
    console.log('[ws]', msg)

    if (msg.type === 'device_commissioned') {
      console.log('Handling device_commissioned message')
      const d = msg.data as { productName?: string; vendorName?: string }
      const name = [d.vendorName, d.productName].filter(Boolean).join(' ')
      addToast(`New device commissioned: ${name || 'Unknown device'}`)
      return
    }

    // The firmware coalesces attribute reports into one `attribute_batch` frame;
    // a single `attribute_update` is normalised to a one-element batch so both
    // shapes share a path. Accumulate per measured endpoint first, then apply the
    // whole batch in a single setNodes / setEdges pass — one render per frame
    // instead of one render per attribute.
    type AttrEntry = { nodeId: number; endpointId: number; clusterId: number; attributeId: number; value: number }
    const entries: AttrEntry[] = msg.type === 'attribute_batch'
      ? (msg.data as AttrEntry[])
      : [msg.data as AttrEntry]

    const powerByKey = new Map<string, PowerMeasurement>()
    const batteryByKey = new Map<string, number>()
    for (const d of entries) {
      const key = `${d.nodeId}:${d.endpointId}`
      const pm = powerFromAttribute(d.clusterId, d.attributeId, d.value)
      if (Object.keys(pm).length > 0) powerByKey.set(key, { ...(powerByKey.get(key) ?? {}), ...pm })
      const bp = batteryPercentFromAttribute(d.clusterId, d.attributeId, d.value)
      if (bp !== undefined) batteryByKey.set(key, bp)
    }

    if (powerByKey.size === 0 && batteryByKey.size === 0) return

    setNodes(nds => nds.map(n => {
      const key = `${n.data.nodeId}:${n.data.endpointId}`
      const power = powerByKey.get(key)
      const battery = batteryByKey.get(key)
      if (!power && battery === undefined) return n
      return {
        ...n,
        data: {
          ...n.data,
          ...(power ? { power: { ...(n.data.power ?? {}), ...power } } : {}),
          ...(battery !== undefined ? { batteryPercent: battery } : {}),
        },
      }
    }))

    if (powerByKey.size > 0) {
      // Map metered React Flow node ids to the power this batch carried, then
      // update only the edges those nodes meter (see edgePowerNodeId).
      const powerByRfId = new Map<string, PowerMeasurement>()
      for (const n of getNodes()) {
        const power = powerByKey.get(`${n.data.nodeId}:${n.data.endpointId}`)
        if (power && power.power !== undefined) powerByRfId.set(n.id, power)
      }
      if (powerByRfId.size > 0) {
        setEdges(eds => eds.map(e => {
          const power = powerByRfId.get(edgePowerNodeId(e))
          return power ? { ...e, data: { ...(e.data ?? {}), kw: power.power! / 1000000 } } : e
        }))
      }
    }
  }, [addToast, setNodes, setEdges, getNodes])

  const wsState = useWebSocket(handleWsMessage)

  useEffect(() => {
    const handleKeyDown = (e: KeyboardEvent) => {
      if (e.key !== 'Delete' && e.key !== 'Backspace') return
      setNodes(nds => {
        const deletedIds = new Set(
          nds.filter(n => n.selected && n.deletable !== false).map(n => n.id)
        )
        if (deletedIds.size === 0) return nds
        setEdges(eds => {
          const removed = eds.filter(e => deletedIds.has(e.source) || deletedIds.has(e.target))
          removed.forEach(e => fetch(`/api/edges/${e.id}`, { method: 'DELETE' }).catch(() => { }))
          return eds.filter(e => !deletedIds.has(e.source) && !deletedIds.has(e.target))
        })
        for (const id of deletedIds) {
          fetch(`/api/nodes/${id}`, { method: 'DELETE' }).catch(() => { })
        }
        return nds.filter(n => !deletedIds.has(n.id))
      })
    }
    window.addEventListener('keydown', handleKeyDown)
    return () => window.removeEventListener('keydown', handleKeyDown)
  }, [setNodes, setEdges])

  const onDragOver = useCallback((e: React.DragEvent) => {
    e.preventDefault()
    e.dataTransfer.dropEffect = 'copy'
  }, [])

  const onDrop = useCallback(
    (e: React.DragEvent) => {
      e.preventDefault();

      const raw = e.dataTransfer.getData('application/reactflow')
      if (!raw || !reactFlowInstance.current) return
      const device: DeviceSpec = JSON.parse(raw)

      const position = screenToFlowPosition({
        x: e.clientX,
        y: e.clientY,
      });

      const id = `node_${++nodeIdCounter.current}`

      const newNode: Node = {
        id,
        type: 'device',
        position,
        draggable: true,
        data: { label: device.label, nodeId: device.nodeId, endpointId: device.endpointId },
      }

      fetch(`/api/nodes/${id}`, {
        method: 'PUT',
        headers: { 'Content-Type': 'application/json' },
        body: JSON.stringify({
          x: position.x,
          y: position.y,
          settings: { label: device.label, type: 'device', nodeId: device.nodeId, endpointId: device.endpointId },
        }),
      }).catch(() => { })

      setNodes(prev => [...prev, newNode])

    },
    [screenToFlowPosition, type, setNodes],
  );

  const onNodeDoubleClick = useCallback((_: React.MouseEvent, node: Node) => {
    if (node.id === 'meter') setGridModalOpen(true)
  }, [])

  const onNodeDragStop = useCallback((_: React.MouseEvent, node: Node) => {
    fetch(`/api/nodes/${node.id}`, {
      method: 'PUT',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify({ x: node.position.x, y: node.position.y }),
    }).catch(() => { })
  }, [])

  const onEdgesChange = useCallback((changes: EdgeChange[]) => {
    changes.filter(c => c.type === 'remove').forEach(c => {
      fetch(`/api/edges/${(c as { id: string }).id}`, { method: 'DELETE' }).catch(() => { })
    })
    onEdgesChangeBase(changes)
  }, [onEdgesChangeBase])

  const onConnect = useCallback((params: any) => {
    const edgeId = [params.source, params.sourceHandle, params.target, params.targetHandle].filter(Boolean).join('-')
    const newEdge = { ...params, id: edgeId, type: 'powerFlow', data: { kw: 0 } }
    setEdges(eds => addEdge(newEdge, eds))
    fetch('/api/edges', {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify({
        id: edgeId,
        source: params.source,
        target: params.target,
        sourceHandle: params.sourceHandle ?? null,
        targetHandle: params.targetHandle ?? null,
      }),
    }).catch(() => { })
  }, [setEdges]);

  // Right-clicking empty canvas offers "Add load" (a metered device) and "Add sub
  // consumer unit" (a sub-board). Loads are then wired to the relevant handle by
  // hand (persisted by onConnect).
  const onPaneContextMenu = useCallback((event: MouseEvent | React.MouseEvent) => {
    event.preventDefault()
    setPaneMenu({ x: event.clientX, y: event.clientY })
  }, [])

  // Drop a free-standing sub consumer unit where the user right-clicked. The user
  // wires its feed (from a CU circuit) and its loads (onto circuit_N) by hand.
  const addSubConsumerUnit = useCallback((screenX: number, screenY: number) => {
    const id = `node_${++nodeIdCounter.current}`
    const position = screenToFlowPosition({ x: screenX, y: screenY })
    const label = 'Sub Consumer Unit'
    const circuits = 4

    setNodes(prev => [...prev, { id, type: 'subConsumerUnit', position, draggable: true, data: { label, circuits } }])
    fetch(`/api/nodes/${id}`, {
      method: 'PUT',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify({ x: position.x, y: position.y, settings: { label, type: 'subConsumerUnit', circuits } }),
    }).catch(() => { })

    setPaneMenu(null)
  }, [screenToFlowPosition, setNodes])

  // Right-clicking a sub consumer unit or a load offers a delete action. Loads are
  // device/appliance nodes; the protected main CU / meter (deletable === false) get
  // no menu. Other node types have none either. 'henley' is accepted for legacy
  // graphs that predate the sub-CU.
  const onNodeContextMenu = useCallback((event: React.MouseEvent, node: Node) => {
    const MENU_TYPES = ['subConsumerUnit', 'henley', 'device', 'appliance']
    if (!MENU_TYPES.includes(node.type ?? '')) return
    if (node.deletable === false) return
    event.preventDefault()
    setNodeMenu({ node, x: event.clientX, y: event.clientY })
  }, [])

  // Delete a sub consumer unit: drop the board and every load hanging off its
  // circuit outputs, but never the main consumer unit. The upstream feed edge is
  // simply removed (the sub-CU is wired off a CU circuit, not spliced inline, so
  // there is no pass-through link to rebuild).
  const deleteSubConsumerUnit = useCallback((subCuId: string) => {
    const CU = 'consumer_unit'
    setEdges(currentEdges => {
      // Loads are the downstream targets of the sub-CU's circuit handles; remove
      // them along with the board itself. The main consumer unit is preserved.
      const removedNodeIds = new Set<string>([subCuId])
      for (const e of currentEdges) if (e.source === subCuId && e.target !== CU) removedNodeIds.add(e.target)

      const removedEdges = currentEdges.filter(e => removedNodeIds.has(e.source) || removedNodeIds.has(e.target))
      removedEdges.forEach(e => fetch(`/api/edges/${e.id}`, { method: 'DELETE' }).catch(() => { }))

      setNodes(nds => nds.filter(n => !removedNodeIds.has(n.id)))
      removedNodeIds.forEach(id => fetch(`/api/nodes/${id}`, { method: 'DELETE' }).catch(() => { }))

      return currentEdges.filter(e => !removedNodeIds.has(e.source) && !removedNodeIds.has(e.target))
    })
    setNodeMenu(null)
  }, [setEdges, setNodes])

  // Delete a single load: remove the node and the edges connected to it. A load
  // hangs off one CU/sub-CU circuit handle and has no downstream children, so
  // there is nothing to cascade — same node+edge removal as the Delete-key path.
  const deleteLoad = useCallback((loadId: string) => {
    setEdges(currentEdges => {
      const removedEdges = currentEdges.filter(e => e.source === loadId || e.target === loadId)
      removedEdges.forEach(e => fetch(`/api/edges/${e.id}`, { method: 'DELETE' }).catch(() => { }))
      return currentEdges.filter(e => e.source !== loadId && e.target !== loadId)
    })
    setNodes(nds => nds.filter(n => n.id !== loadId))
    fetch(`/api/nodes/${loadId}`, { method: 'DELETE' }).catch(() => { })
    setNodeMenu(null)
  }, [setEdges, setNodes])

  const handleAddLoad = useCallback((device: AddedLoad) => {
    const pos = pendingLoadPos
    setPendingLoadPos(null)
    if (!pos) return
    const id = `node_${++nodeIdCounter.current}`
    const position = screenToFlowPosition({ x: pos.x, y: pos.y })
    const label = device.name || device.label

    setNodes(prev => [...prev, {
      id,
      type: 'device',
      position,
      draggable: true,
      data: { label, nodeId: device.nodeId, endpointId: device.endpointId },
    }])
    fetch(`/api/nodes/${id}`, {
      method: 'PUT',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify({
        x: position.x,
        y: position.y,
        settings: { label: device.label, name: device.name, type: 'device', nodeId: device.nodeId, endpointId: device.endpointId },
      }),
    }).catch(() => { })
  }, [pendingLoadPos, screenToFlowPosition, setNodes])

  const onInit = useCallback((instance: ReactFlowInstance) => {
    reactFlowInstance.current = instance
    fetch('/api/nodes')
      .then(r => r.ok ? r.json() : Promise.reject())
      .then((data: { nodes: SavedNodeConfig[]; edges?: SavedEdgeConfig[] }) => {
        // Current cached values ride along in the node list, so power renders
        // immediately on load without waiting for the first websocket update.
        const powerByNode = new Map<string, PowerMeasurement>()
        const restoredNodes: Node[] = data.nodes.map(n => {
          const power = (n.values ?? []).reduce<PowerMeasurement>(
            (acc, v) => ({ ...acc, ...powerFromAttribute(v.clusterId, v.attributeId, v.value) }), {})
          const hasPower = Object.keys(power).length > 0
          if (hasPower) powerByNode.set(n.id, power)
          const batteryPercent = (n.values ?? []).reduce<number | undefined>(
            (acc, v) => batteryPercentFromAttribute(v.clusterId, v.attributeId, v.value) ?? acc, undefined)
          return {
            id: n.id,
            type: typeof n.settings?.type === 'string' ? n.settings.type as string : undefined,
            position: { x: n.x, y: n.y },
            draggable: true,
            deletable: n.settings?.deletable !== false,
            data: {
              label: (n.settings?.name as string) || (n.settings?.label as string) || n.id,
              nodeId: n.settings?.nodeId as number | undefined,
              endpointId: n.settings?.endpointId as number | undefined,
              ...(typeof n.settings?.outputs === 'number' ? { outputs: n.settings.outputs } : {}),
              ...(typeof n.settings?.circuits === 'number' ? { circuits: n.settings.circuits } : {}),
              ...(hasPower ? { power } : {}),
              ...(batteryPercent !== undefined ? { batteryPercent } : {}),
            },
          }
        })

        for (const n of restoredNodes) {
          const m = n.id.match(/^node_(\d+)$/)
          if (m) nodeIdCounter.current = Math.max(nodeIdCounter.current, parseInt(m[1]))
        }

        setNodes(restoredNodes)

        if (data.edges?.length) {
          setEdges(data.edges.map(e => {
            // The metered node may be at either end: a meter feeds into the CU
            // (it is the source), an appliance hangs off the CU (it is the target).
            const power = powerByNode.get(edgePowerNodeId(e))
            return {
              id: e.id,
              source: e.source,
              target: e.target,
              sourceHandle: e.sourceHandle,
              targetHandle: e.targetHandle,
              type: 'powerFlow',
              data: { kw: power?.power !== undefined ? power.power / 1000000 : 0 },
            }
          }))
        }

        const cu = data.nodes.find(n => n.id === 'consumer_unit')
        instance.setCenter((cu?.x ?? 0) + 75, (cu?.y ?? 0) + 18, { zoom: 1 })
      })
      .catch(() => console.log('Failed to load nodes from API'))
  }, [setNodes, setEdges])

  return (
    <div style={{ display: 'flex', height: 'calc(100vh - 60px)' }}>
      <div className="reactflow-wrapper" style={{ flex: 1, position: 'relative' }} onDragOver={onDragOver}>
        <ReactFlow
          style={{ height: '100%' }}
          nodes={nodes}
          onNodesChange={onNodesChange}
          edges={edges}
          onEdgesChange={onEdgesChange}
          onConnect={onConnect}
          nodeTypes={nodeTypes}
          edgeTypes={edgeTypes}
          onInit={onInit}
          onNodeDoubleClick={onNodeDoubleClick}
          onNodeContextMenu={onNodeContextMenu}
          onPaneContextMenu={onPaneContextMenu}
          onNodeDragStop={onNodeDragStop}
          onDrop={onDrop}
          nodesDraggable={true}
          nodesConnectable={true}
          fitViewOptions={{ padding: 2 }}
          defaultViewport={{ x: 0, y: 0, zoom: 0.5 }}
          nodeOrigin={[0, 0]}
        >
          <Background variant={BackgroundVariant.Lines} color="#cbd5e1" gap={24} size={1.5} />
        </ReactFlow>
        <WsStatusDot state={wsState} />
      </div>

      {gridModalOpen && (
        <GridModal
          onSave={() => setGridModalOpen(false)}
          onCancel={() => setGridModalOpen(false)}
        />
      )}

      {nodeMenu && (
        <>
          {/* Backdrop closes the menu on any outside click. */}
          <div onClick={() => setNodeMenu(null)} style={{ position: 'fixed', inset: 0, zIndex: 99 }} />
          <div style={{ position: 'fixed', top: nodeMenu.y, left: nodeMenu.x, zIndex: 100, background: '#fff', border: '1px solid #e2e8f0', borderRadius: 8, boxShadow: '0 4px 16px rgba(0,0,0,.14)', padding: 4, minWidth: 180 }}>
            {nodeMenu.node.type === 'subConsumerUnit' || nodeMenu.node.type === 'henley' ? (
              <button
                onClick={() => deleteSubConsumerUnit(nodeMenu.node.id)}
                style={{ display: 'block', width: '100%', textAlign: 'left', padding: '8px 12px', border: 'none', background: 'none', cursor: 'pointer', fontSize: 13, color: '#b91c1c', borderRadius: 6 }}
              >
                🗑 Delete sub consumer unit
              </button>
            ) : (
              <button
                onClick={() => deleteLoad(nodeMenu.node.id)}
                style={{ display: 'block', width: '100%', textAlign: 'left', padding: '8px 12px', border: 'none', background: 'none', cursor: 'pointer', fontSize: 13, color: '#b91c1c', borderRadius: 6 }}
              >
                🗑 Delete load
              </button>
            )}
          </div>
        </>
      )}

      {paneMenu && (
        <>
          {/* Backdrop closes the menu on any outside click. */}
          <div onClick={() => setPaneMenu(null)} style={{ position: 'fixed', inset: 0, zIndex: 99 }} />
          <div style={{ position: 'fixed', top: paneMenu.y, left: paneMenu.x, zIndex: 100, background: '#fff', border: '1px solid #e2e8f0', borderRadius: 8, boxShadow: '0 4px 16px rgba(0,0,0,.14)', padding: 4, minWidth: 160 }}>
            <button
              onClick={() => { setPendingLoadPos(paneMenu); setPaneMenu(null) }}
              style={{ display: 'block', width: '100%', textAlign: 'left', padding: '8px 12px', border: 'none', background: 'none', cursor: 'pointer', fontSize: 13, color: '#1e293b', borderRadius: 6 }}
            >
              + Add load
            </button>
            <button
              onClick={() => addSubConsumerUnit(paneMenu.x, paneMenu.y)}
              style={{ display: 'block', width: '100%', textAlign: 'left', padding: '8px 12px', border: 'none', background: 'none', cursor: 'pointer', fontSize: 13, color: '#1e293b', borderRadius: 6 }}
            >
              ▦ Add sub consumer unit
            </button>
          </div>
        </>
      )}

      {pendingLoadPos && (
        <AddLoadModal onAdd={handleAddLoad} onCancel={() => setPendingLoadPos(null)} />
      )}

      <ToastStack toasts={toasts} onDismiss={dismissToast} />
    </div>
  )
}

export default () => (
  <ReactFlowProvider>
    <DnDProvider>
      <Topology />
    </DnDProvider>
  </ReactFlowProvider>
);
