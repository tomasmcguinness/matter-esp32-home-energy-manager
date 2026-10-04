import { ReactFlow, ReactFlowProvider, useNodeConnections, useEdges, Background, BackgroundVariant, useNodesState, useEdgesState, type Node, type Edge, type EdgeChange, type ReactFlowInstance, type Viewport, addEdge, useReactFlow, Handle, Position } from '@xyflow/react'
import { useCallback, useEffect, useRef, useState } from 'react'
import { PowerFlowEdge } from './PowerFlowEdge'
import { GridModal } from './GridModal'
import { AddLoadModal, type AddedLoad } from './AddLoadModal'
import { EditNodeModal, type NodeSettings } from './EditNodeModal'
import { useWebSocket, type WsMessage } from './useWebSocket'
import { DnDProvider, useDnD } from './DnDContext';
import { fmtMoney, fmtUnitPrice, useCurrentPrice } from './tariff'

const edgeTypes = { powerFlow: PowerFlowEdge }

function fmt(value: number | undefined, unit: string): string {
  return value === undefined ? '—' : `${(value/1000).toFixed(1)} ${unit}`
}

// One output circuit per appliance slot on the Home screen.
const CU_CIRCUITS = Array.from({ length: 10 }, (_, i) => i)

function ConsumerUnitNode({ data }: { data: { label: string } }) {
  return (
    <>
      <Handle type="target" position={Position.Left} id="grid" style={{ top: '30%' }} />
      {/* The tariff source prices the grid import; it carries no power. */}
      <Handle type="target" position={Position.Left} id="tariff" style={{ top: '70%' }} />
      <Handle type="target" position={Position.Bottom} id="solar_input" />
      {/* Fixed feed to the Unallocated node; not a circuit the user can wire. */}
      <Handle type="source" position={Position.Top} id="unallocated" isConnectable={false} />
      {/* Tall enough that the circuit handles on the right edge don't overlap. */}
      <div style={{ padding: '5px 12px', fontSize: 13, fontWeight: 500, color: '#1e293b', whiteSpace: 'nowrap', minHeight: CU_CIRCUITS.length * 12, display: 'flex', alignItems: 'center' }}>
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
type DeviceNodeData = { label: string; nodeId?: number; endpointId?: number; power?: PowerMeasurement; batteryPercent?: number; standbyW?: number; status?: LoadStatus }

// Running state of an appliance, derived from its live draw and learned standby.
type LoadStatus = 'running' | 'standby' | 'off'

// Learned profile fields served by GET /api/appliance/profiles (see Appliances.tsx).
type ProfilesResponse = { appliances: { graph_id: string; trained: boolean; standby_w?: number }[] }

// The learned standby is the centre of a 5 W histogram band, so the threshold is
// the top of that band to stop readings that wobble around the centre flickering.
const STANDBY_BAND_HALF_W = 2.5
// Below this an appliance with a real standby is unplugged / switched off at the wall.
const OFF_FLOOR_W = 1

// standbyW is 0 for appliances that idle at 0-5 W (and untrained ones), which
// show "Off" while inside that band. Undefined power (no reading yet) => no status.
function loadStatus(powerMw: number | undefined, standbyW: number): LoadStatus | undefined {
  if (powerMw === undefined) return undefined
  const w = powerMw / 1000
  if (standbyW <= 0) return w <= STANDBY_BAND_HALF_W * 2 ? 'off' : 'running'
  if (w < OFF_FLOOR_W) return 'off'
  return w <= standbyW + STANDBY_BAND_HALF_W ? 'standby' : 'running'
}

const STATUS_BADGE: Record<'standby' | 'off', { text: string; background: string; color: string }> = {
  standby: { text: 'Standby', background: '#fef3c7', color: '#92400e' },
  off: { text: 'Off', background: '#e2e8f0', color: '#475569' },
}

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

// Price in force and what importing at the current rate costs per hour. Shown
// on the grid meter (positive power = import) whenever a price is available.
function ImportCost({ powerMw }: { powerMw?: number }) {
  const { price, currency } = useCurrentPrice()
  if (price === null) return null
  const importKw = powerMw !== undefined && powerMw > 0 ? powerMw / 1000000 : 0
  return (
    <>
      <span style={{ color: '#94a3b8' }}>Price</span><span style={{ textAlign: 'right' }}>{fmtUnitPrice(price, currency)}</span>
      <span style={{ color: '#94a3b8' }}>Cost</span><span style={{ textAlign: 'right' }}>{fmtMoney(importKw * price, currency)}/h</span>
    </>
  )
}

function DeviceNode({ data }: { data: DeviceNodeData }) {
  const badge = data.status && data.status !== 'running' ? STATUS_BADGE[data.status] : null
  // The handle is the role: a meter feeding the consumer unit's grid handle is the grid meter.
  const isGrid = useNodeConnections({ handleType: 'source', handleId: 'power-out' })
    .some(c => c.target === 'consumer_unit' && c.targetHandle === 'grid')

  return (
    <>
      <Handle type="target" position={Position.Left} id="power-in" />
      <div style={{ padding: '4px 10px', background: '#f1f5f9', borderBottom: '1px solid #e2e8f0', fontSize: 12, fontWeight: 600, color: '#1e293b', whiteSpace: 'nowrap', display: 'flex', alignItems: 'center', gap: 6 }}>
        <span>0x{data.nodeId?.toString(16).toUpperCase()} | {data.endpointId} | {data.label}</span>
        {badge && (
          <span
            title={`Learned standby ≈ ${(data.standbyW ?? 0).toFixed(1)} W`}
            style={{ marginLeft: 'auto', padding: '1px 6px', borderRadius: 999, fontSize: 10, fontWeight: 600, background: badge.background, color: badge.color }}
          >
            {badge.text}
          </span>
        )}
      </div>

      <div style={{ minWidth: '100px', padding: '5px 10px', display: 'grid', gridTemplateColumns: 'auto 1fr', columnGap: 8, rowGap: 2, fontSize: 12 }}>
        <span style={{ color: '#94a3b8' }}>V</span><span style={{textAlign: 'right'}}>{fmt(data.power?.voltage, 'V')}</span>
        <span style={{ color: '#94a3b8' }}>I</span><span style={{textAlign: 'right'}}>{fmt(data.power?.current, 'A')}</span>
        <span style={{ color: '#94a3b8' }}>P</span><span style={{textAlign: 'right'}}>{fmt(data.power?.power, 'W')}</span>
        {isGrid && <ImportCost powerMw={data.power?.power} />}
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
// direction from the sign of ActivePower. Matter convention: positive = power
// into the battery (charging), negative = power out of it (discharging).
function BatteryNode({ data }: { data: DeviceNodeData }) {
  const w = data.power?.power
  let label = 'Idle'
  let color = '#94a3b8'
  let bg = '#f1f5f9'
  if (w !== undefined && w !== 0) {
    const discharging = w < 0
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
        <span className="nodrag" style={{ display: 'flex', gap: 2 }} onDoubleClick={e => e.stopPropagation()}>
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

// The consumer unit's unmetered remainder: whatever enters the main consumer
// unit and isn't accounted for by a metered circuit. Derived, not measured, so
// it has no device; the value rides on its feed edge (see withUnallocated).
// A remainder that stays negative means the metered loads add up to more than
// the supply, which points at an inverted CT or a missing meter rather than at
// real power, so it is shown as zero with a warning.
const UNALLOCATED_ID = 'unallocated'
const UNALLOCATED_EDGE_ID = 'consumer_unit-unallocated-unallocated-power-in'
// Meters report at different moments, so a brief or small negative is just skew.
const UNALLOCATED_NEGATIVE_W = -50
const UNALLOCATED_NEGATIVE_HOLD_MS = 30000

function UnallocatedNode({ data }: { data: { label: string } }) {
  const edge = useEdges().find(e => e.id === UNALLOCATED_EDGE_ID)
  const rawKw = edge?.data?.rawKw as number | undefined
  const negative = rawKw !== undefined && rawKw * 1000 < UNALLOCATED_NEGATIVE_W
  const [warn, setWarn] = useState(false)

  // Raise the warning only once the remainder has stayed negative; clear it at once.
  useEffect(() => {
    const timer = setTimeout(() => setWarn(negative), negative ? UNALLOCATED_NEGATIVE_HOLD_MS : 0)
    return () => clearTimeout(timer)
  }, [negative])

  return (
    <>
      <div style={{ padding: '4px 10px', background: '#f1f5f9', borderBottom: '1px solid #e2e8f0', fontSize: 12, fontWeight: 600, color: '#1e293b', whiteSpace: 'nowrap', display: 'flex', alignItems: 'center', gap: 6 }}>
        <span>{data.label}</span>
        {warn && (
          <span
            title={`Metered loads exceed the supply by ${Math.abs(rawKw! * 1000).toFixed(0)} W. Check for an inverted CT or a missing meter.`}
            style={{ marginLeft: 'auto', padding: '1px 6px', borderRadius: 999, fontSize: 10, fontWeight: 600, background: '#fee2e2', color: '#b91c1c' }}
          >
            ⚠ Check metering
          </span>
        )}
      </div>
      <div style={{ minWidth: '100px', padding: '5px 10px', display: 'grid', gridTemplateColumns: 'auto 1fr', columnGap: 8, fontSize: 12 }}>
        <span style={{ color: '#94a3b8' }}>P</span>
        <span style={{ textAlign: 'right' }}>{rawKw === undefined ? '—' : `${(Math.max(0, rawKw) * 1000).toFixed(1)} W`}</span>
      </div>
      <Handle type="target" position={Position.Bottom} id="power-in" isConnectable={false} />
    </>
  )
}

// Appliance nodes are functionally identical to device nodes on the canvas (a metered
// endpoint with a power-in handle); they only differ by their semantic role/type.
// The device publishing the Commodity Tariff. Wired to the consumer unit's
// `tariff` handle; shows the price in force right now.
function TariffNode({ data }: { data: DeviceNodeData }) {
  const { price, currency } = useCurrentPrice()
  return (
    <>
      <div style={{ padding: '4px 10px', background: '#fef3c7', borderBottom: '1px solid #fde68a', fontSize: 12, fontWeight: 600, color: '#1e293b', whiteSpace: 'nowrap' }}>
        Tariff | {data.label}
      </div>
      <div style={{ padding: '5px 10px', fontSize: 12, textAlign: 'right' }}>
        {price !== null ? fmtUnitPrice(price, currency) : '—'}
      </div>
      <Handle type="source" position={Position.Right} id="tariff-out" />
    </>
  )
}

// Tariff edges carry pricing, not power: draw them as a plain dashed line.
const TARIFF_EDGE_STYLE = { strokeDasharray: '4 4', stroke: '#f59e0b' }
const isTariffEdge = (e: { targetHandle?: string | null; sourceHandle?: string | null }) =>
  e.targetHandle === 'tariff' || e.sourceHandle === 'tariff'

const nodeTypes = {
  consumerUnit: ConsumerUnitNode,
  device: DeviceNode,
  appliance: DeviceNode,
  solarInverter: SolarInverterNode,
  pvString: PvStringNode,
  battery: BatteryNode,
  subConsumerUnit: SubConsumerUnitNode,
  tariff: TariffNode,
  unallocated: UnallocatedNode,
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

// Convert a metered ActivePower reading (mW) into the edge's kW flow. Matter's
// convention is positive = into the metered device, negative = out of it. Edge
// kW is negative for source → target and positive for target → source, so a
// reading metered at the target is negated to express it along the edge.
// A grid meter is the exception (Matter spec 9.2.6.1): its reading is relative
// to the premises, positive = flowing into them, so a grid edge counts as
// metered at whichever end carries the consumer unit's grid handle. The edge
// may be saved in either orientation.
function edgeKw(
  e: { target: string; sourceHandle?: string | null; targetHandle?: string | null },
  meteredId: string,
  powerMw: number,
): number {
  const kw = powerMw / 1000000
  const atTarget = e.targetHandle === 'grid' ? true
    : e.sourceHandle === 'grid' ? false
    : meteredId === e.target
  return atTarget ? -kw : kw
}

const SUB_CU_TYPES = ['subConsumerUnit', 'henley']

function subCuIdsOf(nodes: Node[]): Set<string> {
  return new Set(nodes.filter(n => SUB_CU_TYPES.includes(n.type ?? '')).map(n => n.id))
}

// A sub consumer unit has no meter of its own, so the flow on its feed (the edge
// into its `power-in` handle) is the sum of the flows on its circuit edges, i.e.
// the loads hanging off it. Whatever sits upstream (a grid meter, a CU circuit)
// must not stamp its own reading onto the feed. Sub-CUs can nest, so a board's
// total is resolved recursively; `visiting` guards against a wiring loop.
//
// Power that goes to a sub-CU never reaches whatever else hangs off the same
// upstream handle. Where a metered handle feeds sub-CUs plus exactly one other
// edge (a grid meter feeding both the main consumer unit and a sub-CU), that
// edge carries the remainder: the meter's reading (`meteredKw`, the raw value
// stamped on it) less the sub-CU feeds. With no such single edge there is
// nothing to attribute the remainder to, so the siblings are left alone.
function withSubCuFeeds(edges: Edge[], subCuIds: Set<string>): Edge[] {
  if (subCuIds.size === 0) return edges
  const totals = new Map<string, { kw: number; idle: boolean }>()
  const visiting = new Set<string>()

  const totalFor = (subCuId: string): { kw: number; idle: boolean } => {
    const known = totals.get(subCuId)
    if (known) return known
    if (visiting.has(subCuId)) return { kw: 0, idle: true }
    visiting.add(subCuId)
    let kw = 0
    let idle = true
    for (const e of edges) {
      if (e.source !== subCuId) continue
      const flow = subCuIds.has(e.target)
        ? totalFor(e.target)
        : { kw: (e.data?.kw as number | undefined) ?? 0, idle: e.data?.idle === true }
      kw += flow.kw
      if (!flow.idle && flow.kw !== 0) idle = false
    }
    visiting.delete(subCuId)
    const total = { kw, idle }
    totals.set(subCuId, total)
    return total
  }

  // Flow leaving each upstream node + handle towards sub-CUs. Edge kW is negative
  // for source → target, so the flow away from the source is its negation.
  const handleKey = (nodeId: string, handle?: string | null) => `${nodeId}|${handle ?? ''}`
  const toSubCus = new Map<string, number>()
  for (const e of edges) {
    if (!subCuIds.has(e.target)) continue
    const key = handleKey(e.source, e.sourceHandle)
    toSubCus.set(key, (toSubCus.get(key) ?? 0) - totalFor(e.target).kw)
  }

  // The other edges on each of those handles. A grid edge may be saved in either
  // orientation, so the shared handle can be at its source or its target end.
  const siblings = new Map<string, { edge: Edge; atSource: boolean }[]>()
  for (const e of edges) {
    if (subCuIds.has(e.target)) continue
    for (const atSource of [true, false]) {
      const key = atSource ? handleKey(e.source, e.sourceHandle) : handleKey(e.target, e.targetHandle)
      if (!toSubCus.has(key)) continue
      siblings.set(key, [...(siblings.get(key) ?? []), { edge: e, atSource }])
    }
  }
  const remainderKw = new Map<string, number>()
  for (const [key, list] of siblings) {
    if (list.length !== 1) continue
    const { edge, atSource } = list[0]
    const metered = edge.data?.meteredKw as number | undefined
    if (metered === undefined) continue
    const away = (atSource ? -metered : metered) - toSubCus.get(key)!
    remainderKw.set(edge.id, atSource ? -away : away)
  }

  return edges.map(e => {
    if (subCuIds.has(e.target)) {
      const { kw, idle } = totalFor(e.target)
      if (e.data?.kw === kw && e.data?.idle === idle) return e
      return { ...e, data: { ...(e.data ?? {}), kw, idle } }
    }
    const kw = remainderKw.get(e.id)
    if (kw === undefined || e.data?.kw === kw) return e
    return { ...e, data: { ...(e.data ?? {}), kw } }
  })
}

// Set the Unallocated feed to whatever the main consumer unit takes in and
// doesn't hand to a metered circuit: the net of every other power edge touching
// it (edge kW is negative for source → target, so flow into the CU is -kw where
// the CU is the target and +kw where it is the source). `rawKw` keeps the true
// remainder for the node's metering warning; the drawn flow never goes negative.
function withUnallocated(edges: Edge[]): Edge[] {
  let rawKw = 0
  for (const e of edges) {
    if (e.id === UNALLOCATED_EDGE_ID || isTariffEdge(e)) continue
    const kw = (e.data?.kw as number | undefined) ?? 0
    if (e.target === 'consumer_unit') rawKw -= kw
    else if (e.source === 'consumer_unit') rawKw += kw
  }
  const kw = -Math.max(0, rawKw)
  return edges.map(e => {
    if (e.id !== UNALLOCATED_EDGE_ID) return e
    if (e.data?.kw === kw && e.data?.rawKw === rawKw) return e
    return { ...e, data: { ...(e.data ?? {}), kw, rawKw } }
  })
}

// Every derived (unmetered) flow, applied after meter readings are stamped on.
function withDerivedFlows(edges: Edge[], subCuIds: Set<string>): Edge[] {
  return withUnallocated(withSubCuFeeds(edges, subCuIds))
}

type DeviceSpec = {
  nodeId: number,
  endpointId: number,
  label: string
}

const initialNodes: Node[] = []

const initialEdges: Edge[] = []

// The canvas is a route, so it unmounts on every navigation away and comes back with a
// fresh viewport. Persisting the zoom keeps the user's chosen scale across visits and
// reloads; the pan still recentres on the consumer unit (see onInit).
const ZOOM_STORAGE_KEY = 'hem.topology.zoom'
const MIN_ZOOM = 0.2
const MAX_ZOOM = 2

// Local storage can throw (private mode, blocked site data) and can hold junk from an
// older build, so every read is guarded and range-checked before it reaches setCenter.
function readStoredZoom(): number | null {
  try {
    const raw = localStorage.getItem(ZOOM_STORAGE_KEY)
    if (raw === null) return null
    const zoom = parseFloat(raw)
    if (!Number.isFinite(zoom) || zoom < MIN_ZOOM || zoom > MAX_ZOOM) return null
    return zoom
  } catch {
    return null
  }
}

function writeStoredZoom(zoom: number) {
  try {
    localStorage.setItem(ZOOM_STORAGE_KEY, String(zoom))
  } catch { /* storage unavailable — zoom just won't persist */ }
}

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

// Node types the user can edit (double-click) or delete (right-click): loads and
// sub consumer units. 'henley' is accepted for legacy graphs that predate the sub-CU.
const EDITABLE_TYPES = ['subConsumerUnit', 'henley', 'device', 'appliance']

// The settings object to edit for a node. The circuit count is changed on the
// node itself (see SubConsumerUnitNode), so the live value wins over the copy
// captured when the graph loaded.
function editSettings(node: Node): NodeSettings {
  const settings = { type: node.type, ...((node.data.settings as NodeSettings | undefined) ?? {}) }
  return typeof node.data.circuits === 'number' ? { ...settings, circuits: node.data.circuits } : settings
}

function Topology() {
  const [nodes, setNodes, onNodesChange] = useNodesState(initialNodes)
  const [edges, setEdges, onEdgesChangeBase] = useEdgesState(initialEdges)
  const [gridModalOpen, setGridModalOpen] = useState(false)
  const [editNode, setEditNode] = useState<Node | null>(null)
  const [nodeMenu, setNodeMenu] = useState<{ node: Node; x: number; y: number } | null>(null)
  const [paneMenu, setPaneMenu] = useState<{ x: number; y: number } | null>(null)
  // Flow-space position where an "Add load" node should be created (captured from
  // the right-click), held while the picker modal is open.
  const [pendingLoadPos, setPendingLoadPos] = useState<{ x: number; y: number } | null>(null)
  const [toasts, setToasts] = useState<Toast[]>([])
  const reactFlowInstance = useRef<ReactFlowInstance | null>(null)
  const nodeIdCounter = useRef(10)
  const toastIdCounter = useRef(0)
  // Learned standby (W) per appliance graph id. Only appliances are present, so
  // membership doubles as "this node gets a Standby/Off status".
  const standbyByNodeId = useRef(new Map<string, number>())
  const { screenToFlowPosition, getNodes, getEdges } = useReactFlow();
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
      const standbyW = standbyByNodeId.current.get(n.id)
      const status = power?.power !== undefined && standbyW !== undefined ? loadStatus(power.power, standbyW) : undefined
      return {
        ...n,
        data: {
          ...n.data,
          ...(power ? { power: { ...(n.data.power ?? {}), ...power } } : {}),
          ...(battery !== undefined ? { batteryPercent: battery } : {}),
          ...(status ? { status } : {}),
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
        const subCuIds = subCuIdsOf(getNodes())
        setEdges(eds => withDerivedFlows(eds.map(e => {
          // A sub-CU's feed is derived from its loads, not metered upstream.
          if (subCuIds.has(e.target)) return e
          const meteredId = edgePowerNodeId(e)
          const power = powerByRfId.get(meteredId)
          if (!power) return e
          const standbyW = standbyByNodeId.current.get(meteredId)
          const idle = standbyW !== undefined && loadStatus(power.power, standbyW) !== 'running'
          const kw = edgeKw(e, meteredId, power.power!)
          return { ...e, data: { ...(e.data ?? {}), kw, meteredKw: kw, idle } }
        }), subCuIds))
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
        // A solar inverter takes its PV strings (dc_in) and battery with it,
        // mirroring the firmware's cascade in node_manager_delete.
        const children = getEdges()
          .filter(e => (deletedIds.has(e.target) && e.targetHandle === 'dc_in') || (deletedIds.has(e.source) && e.sourceHandle === 'battery'))
          .map(e => (deletedIds.has(e.target) ? e.source : e.target))
        children.forEach(id => deletedIds.add(id))
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
  }, [setNodes, setEdges, getEdges])

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
      const settings = { label: device.label, type: 'device', nodeId: device.nodeId, endpointId: device.endpointId }

      const newNode: Node = {
        id,
        type: 'device',
        position,
        draggable: true,
        data: { label: device.label, nodeId: device.nodeId, endpointId: device.endpointId, settings },
      }

      fetch(`/api/nodes/${id}`, {
        method: 'PUT',
        headers: { 'Content-Type': 'application/json' },
        body: JSON.stringify({
          x: position.x,
          y: position.y,
          settings,
        }),
      }).catch(() => { })

      setNodes(prev => [...prev, newNode])

    },
    [screenToFlowPosition, type, setNodes],
  );

  // Loads and sub consumer units are edited in place; the grid meter keeps its own
  // modal. Other node types have nothing editable here.
  const onNodeDoubleClick = useCallback((_: React.MouseEvent, node: Node) => {
    if (node.id === 'meter') setGridModalOpen(true)
    else if (EDITABLE_TYPES.includes(node.type ?? '')) setEditNode(node)
  }, [])

  // Apply a saved edit (already persisted by the modal) to the canvas. A load that
  // moved to a different endpoint drops its readings, which belong to the old one.
  const handleNodeEdited = useCallback((id: string, settings: NodeSettings) => {
    setEditNode(null)
    setNodes(nds => nds.map(n => {
      if (n.id !== id) return n
      const nodeId = settings.nodeId as number | undefined
      const endpointId = settings.endpointId as number | undefined
      const deviceChanged = n.data.nodeId !== nodeId || n.data.endpointId !== endpointId
      return {
        ...n,
        data: {
          ...n.data,
          ...(deviceChanged ? { power: undefined, batteryPercent: undefined, status: undefined } : {}),
          label: (settings.name as string) || (settings.label as string) || n.id,
          nodeId,
          endpointId,
          settings,
        },
      }
    }))
  }, [setNodes])

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
    const newEdge = isTariffEdge(params)
      ? { ...params, id: edgeId, style: TARIFF_EDGE_STYLE }
      : { ...params, id: edgeId, type: 'powerFlow', data: { kw: 0 } }
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
    const settings = { label, type: 'subConsumerUnit', circuits }

    setNodes(prev => [...prev, { id, type: 'subConsumerUnit', position, draggable: true, data: { label, circuits, settings } }])
    fetch(`/api/nodes/${id}`, {
      method: 'PUT',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify({ x: position.x, y: position.y, settings }),
    }).catch(() => { })

    setPaneMenu(null)
  }, [screenToFlowPosition, setNodes])

  // Right-clicking a sub consumer unit or a load offers edit and delete actions.
  // Loads are device/appliance nodes; the protected main CU / meter
  // (deletable === false) get no menu. Other node types have none either.
  const onNodeContextMenu = useCallback((event: React.MouseEvent, node: Node) => {
    if (!EDITABLE_TYPES.includes(node.type ?? '')) return
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
    const settings = { label: device.label, name: device.name, type: 'device', nodeId: device.nodeId, endpointId: device.endpointId }

    setNodes(prev => [...prev, {
      id,
      type: 'device',
      position,
      draggable: true,
      data: { label, nodeId: device.nodeId, endpointId: device.endpointId, settings },
    }])
    fetch(`/api/nodes/${id}`, {
      method: 'PUT',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify({
        x: position.x,
        y: position.y,
        settings,
      }),
    }).catch(() => { })
  }, [pendingLoadPos, screenToFlowPosition, setNodes])

  // Fires once at the end of a pan/zoom gesture, so no debounce is needed. Saving here
  // rather than on unmount also avoids StrictMode's double-mount firing it spuriously.
  const onMoveEnd = useCallback((_: MouseEvent | TouchEvent | null, viewport: Viewport) => {
    writeStoredZoom(viewport.zoom)
  }, [])

  const onInit = useCallback((instance: ReactFlowInstance) => {
    reactFlowInstance.current = instance
    // Profiles are optional: if they fail to load the canvas still renders, just
    // without Standby/Off statuses.
    const profiles: Promise<ProfilesResponse> = fetch('/api/appliance/profiles')
      .then(r => r.ok ? r.json() : Promise.reject())
      .catch(() => ({ appliances: [] }))
    Promise.all([fetch('/api/nodes').then(r => r.ok ? r.json() : Promise.reject()), profiles])
      .then(([data, profileData]: [{ nodes: SavedNodeConfig[]; edges?: SavedEdgeConfig[] }, ProfilesResponse]) => {
        // Untrained appliances count as standby 0. Profiles saved before the firmware
        // reported 0 for a 0-5 W idle carry the 2.5 W band centre; treat that as 0 too.
        const standbyMap = new Map<string, number>()
        for (const p of profileData.appliances ?? []) {
          const w = p.trained ? (p.standby_w ?? 0) : 0
          standbyMap.set(p.graph_id, w <= STANDBY_BAND_HALF_W ? 0 : w)
        }
        standbyByNodeId.current = standbyMap

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
          const standbyW = standbyMap.get(n.id)
          const status = standbyW !== undefined ? loadStatus(power.power, standbyW) : undefined
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
              ...(standbyW !== undefined ? { standbyW } : {}),
              ...(status ? { status } : {}),
              settings: n.settings ?? {},
            },
          }
        })

        // The Unallocated node is fixed: create it above the consumer unit the
        // first time, and persist it so it keeps wherever the user drags it.
        const cu = data.nodes.find(n => n.id === 'consumer_unit')
        if (!restoredNodes.some(n => n.id === UNALLOCATED_ID)) {
          const position = { x: cu?.x ?? 0, y: (cu?.y ?? 0) - 140 }
          const settings = { label: 'Unallocated', type: 'unallocated', deletable: false }
          restoredNodes.push({ id: UNALLOCATED_ID, type: 'unallocated', position, draggable: true, deletable: false, data: { label: settings.label, settings } })
          fetch(`/api/nodes/${UNALLOCATED_ID}`, {
            method: 'PUT',
            headers: { 'Content-Type': 'application/json' },
            body: JSON.stringify({ x: position.x, y: position.y, settings }),
          }).catch(() => { })
        }

        for (const n of restoredNodes) {
          const m = n.id.match(/^node_(\d+)$/)
          if (m) nodeIdCounter.current = Math.max(nodeIdCounter.current, parseInt(m[1]))
        }

        setNodes(restoredNodes)

        // Its feed is derived, so the edge lives only on the canvas: the firmware
        // and the Power tab read roles off the consumer unit's saved edges.
        const unallocatedEdge: Edge = {
          id: UNALLOCATED_EDGE_ID, source: 'consumer_unit', sourceHandle: 'unallocated', target: UNALLOCATED_ID, targetHandle: 'power-in',
          type: 'powerFlow', deletable: false, selectable: false, data: { kw: 0 },
        }
        {
          const savedEdges = (data.edges ?? []).filter(e => e.id !== UNALLOCATED_EDGE_ID)
          setEdges(withDerivedFlows([...savedEdges.map((e): Edge => {
            // The metered node may be at either end: a meter feeds into the CU
            // (it is the source), an appliance hangs off the CU (it is the target).
            if (isTariffEdge(e)) {
              return { id: e.id, source: e.source, target: e.target, sourceHandle: e.sourceHandle, targetHandle: e.targetHandle, style: TARIFF_EDGE_STYLE }
            }
            const meteredId = edgePowerNodeId(e)
            const power = powerByNode.get(meteredId)
            const standbyW = standbyMap.get(meteredId)
            const status = standbyW !== undefined ? loadStatus(power?.power, standbyW) : undefined
            const kw = power?.power !== undefined ? edgeKw(e, meteredId, power.power) : 0
            return {
              id: e.id,
              source: e.source,
              target: e.target,
              sourceHandle: e.sourceHandle,
              targetHandle: e.targetHandle,
              type: 'powerFlow',
              data: {
                kw,
                meteredKw: kw,
                idle: status !== undefined && status !== 'running',
              },
            }
          }), unallocatedEdge], subCuIdsOf(restoredNodes)))
        }

        instance.setCenter((cu?.x ?? 0) + 75, (cu?.y ?? 0) + 18, { zoom: readStoredZoom() ?? 1 })
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
          onMoveEnd={onMoveEnd}
          nodesDraggable={true}
          nodesConnectable={true}
          fitViewOptions={{ padding: 2 }}
          minZoom={MIN_ZOOM}
          maxZoom={MAX_ZOOM}
          defaultViewport={{ x: 0, y: 0, zoom: readStoredZoom() ?? 0.5 }}
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

      {editNode && (
        <EditNodeModal
          nodeId={editNode.id}
          settings={editSettings(editNode)}
          hasDevice={editNode.type === 'device' || editNode.type === 'appliance'}
          onSave={settings => handleNodeEdited(editNode.id, settings)}
          onCancel={() => setEditNode(null)}
        />
      )}

      {nodeMenu && (
        <>
          {/* Backdrop closes the menu on any outside click. */}
          <div onClick={() => setNodeMenu(null)} style={{ position: 'fixed', inset: 0, zIndex: 99 }} />
          <div style={{ position: 'fixed', top: nodeMenu.y, left: nodeMenu.x, zIndex: 100, background: '#fff', border: '1px solid #e2e8f0', borderRadius: 8, boxShadow: '0 4px 16px rgba(0,0,0,.14)', padding: 4, minWidth: 180 }}>
            <button
              onClick={() => { setEditNode(nodeMenu.node); setNodeMenu(null) }}
              style={{ display: 'block', width: '100%', textAlign: 'left', padding: '8px 12px', border: 'none', background: 'none', cursor: 'pointer', fontSize: 13, color: '#1e293b', borderRadius: 6 }}
            >
              ✎ Edit
            </button>
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
