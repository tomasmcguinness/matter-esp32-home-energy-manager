import { ReactFlow, ReactFlowProvider, Background, BackgroundVariant, useNodesState, useEdgesState, type Node, type Edge, type EdgeChange, type ReactFlowInstance, addEdge, useReactFlow, Handle, Position } from '@xyflow/react'
import { useCallback, useEffect, useRef, useState } from 'react'
import { PowerFlowEdge } from './PowerFlowEdge'
import { GridModal } from './GridModal'
import { useWebSocket, type WsMessage } from './useWebSocket'
import { DnDProvider, useDnD } from './DnDContext';

const edgeTypes = { powerFlow: PowerFlowEdge }

function attributeKey(clusterId: number, attributeId: number): 'voltage' | 'current' | 'power' | null {
  if (clusterId === 0x0090) {   // Electrical Power Measurement
    if (attributeId === 0x0004) return 'voltage'
    if (attributeId === 0x0005) return 'current'
    if (attributeId === 0x0008) return 'power'
  }
  return null
}

function fmt(value: number | undefined, unit: string): string {
  return value === undefined ? '—' : `${value.toFixed(2)} ${unit}`
}

function ConsumerUnitNode({ data }: { data: { label: string } }) {
  return (
    <div style={{ border: '1px solid #e2e8f0', borderRadius: 6, background: '#fff', overflow: 'hidden' }}>
      <Handle type="target" position={Position.Left} id="power-in" />
      <div style={{ padding: '5px 12px', fontSize: 13, fontWeight: 500, color: '#1e293b', whiteSpace: 'nowrap' }}>
        {data.label}
      </div>
      <Handle type="source" position={Position.Right} id="power-out" />
    </div>
  )
}

type DeviceNodeData = { label: string; nodeId?: number; endpointId?: number; voltage?: number; current?: number; power?: number }

function DeviceNode({ data }: { data: DeviceNodeData }) {
  return (
    <div style={{ border: '1px solid #e2e8f0', borderRadius: 6, background: '#fff', overflow: 'hidden', minWidth: 148 }}>
      <Handle type="target" position={Position.Left} id="power-in" />
      <div style={{ padding: '4px 10px', background: '#f1f5f9', borderBottom: '1px solid #e2e8f0', fontSize: 12, fontWeight: 600, color: '#1e293b', whiteSpace: 'nowrap' }}>
        0x{data.nodeId?.toString(16).toUpperCase()}
      </div>
      <div style={{ padding: '5px 10px', display: 'grid', gridTemplateColumns: 'auto 1fr', columnGap: 8, rowGap: 2, fontSize: 12 }}>
        <span style={{ color: '#94a3b8' }}>V</span><span>{fmt(data.voltage, 'V')}</span>
        <span style={{ color: '#94a3b8' }}>I</span><span>{fmt(data.current, 'A')}</span>
        <span style={{ color: '#94a3b8' }}>P</span><span>{fmt(data.power, 'W')}</span>
      </div>
      <Handle type="source" position={Position.Right} id="power-out" />
    </div>
  )
}

const nodeTypes = { consumerUnit: ConsumerUnitNode, device: DeviceNode }

type SavedNodeConfig = { id: string; x: number; y: number; settings: Record<string, unknown> }
type SavedEdgeConfig = { id: string; source: string; target: string; sourceHandle?: string; targetHandle?: string }

type DeviceSpec = {
  nodeId: number,
  endpointId: number,
  label: string,
  hasPowerMeasurement: boolean,
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

function Home() {
  const [nodes, setNodes, onNodesChange] = useNodesState(initialNodes)
  const [edges, setEdges, onEdgesChangeBase] = useEdgesState(initialEdges)
  const [gridModalOpen, setGridModalOpen] = useState(false)
  const [palette, setPalette] = useState<DeviceSpec[]>([])
  const [paletteLoading, setPaletteLoading] = useState(true)
  const [toasts, setToasts] = useState<Toast[]>([])
  const reactFlowInstance = useRef<ReactFlowInstance | null>(null)
  const nodeIdCounter = useRef(10)
  const toastIdCounter = useRef(0)
  const { screenToFlowPosition } = useReactFlow();
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
    } else {
      const d = msg.data as { nodeId: number; endpointId: number; clusterId: number; attributeId: number; value: number }
      const key = attributeKey(d.clusterId, d.attributeId)
      if (key) {
        setNodes(nds => nds.map(n =>
          n.data.nodeId === d.nodeId && n.data.endpointId === d.endpointId
            ? { ...n, data: { ...n.data, [key]: d.value } }
            : n
        ))
      }
    }
  }, [addToast])

  const wsState = useWebSocket(handleWsMessage)

  useEffect(() => {
    const handleKeyDown = (e: KeyboardEvent) => {
      if (e.key !== 'Delete' && e.key !== 'Backspace') return
      setNodes(nds => {
        const deletedIds = new Set(
          nds.filter(n => n.selected && n.deletable !== false).map(n => n.id)
        )
        if (deletedIds.size === 0) return nds
        setEdges(eds => eds.filter(e => !deletedIds.has(e.source) && !deletedIds.has(e.target)))
        for (const id of deletedIds) {
          fetch(`/api/nodes/${id}`, { method: 'DELETE' }).catch(() => { })
        }
        return nds.filter(n => !deletedIds.has(n.id))
      })
    }
    window.addEventListener('keydown', handleKeyDown)
    return () => window.removeEventListener('keydown', handleKeyDown)
  }, [setNodes, setEdges])

  useEffect(() => {
    fetch('/api/devices/simple')
      .then(r => r.ok ? r.json() : Promise.reject())
      .then((data: { devices: DeviceSpec[] }) => {
        setPalette(data.devices)
      })
      .catch(() => { })
      .finally(() => setPaletteLoading(false))
  }, [])

  const onDragStart = (e: React.DragEvent, device: DeviceSpec) => {
    e.dataTransfer.setData('application/reactflow', JSON.stringify(device))
    e.dataTransfer.effectAllowed = 'copy'
  }

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

  const onInit = useCallback((instance: ReactFlowInstance) => {
    reactFlowInstance.current = instance
    fetch('/api/nodes')
      .then(r => r.ok ? r.json() : Promise.reject())
      .then((data: { nodes: SavedNodeConfig[]; edges?: SavedEdgeConfig[] }) => {
        const restoredNodes: Node[] = data.nodes.map(n => ({
          id: n.id,
          type: typeof n.settings?.type === 'string' ? n.settings.type as string : undefined,
          position: { x: n.x, y: n.y },
          draggable: true,
          deletable: n.settings?.deletable !== false,
          data: {
            label: (n.settings?.label as string) ?? n.id,
            nodeId: n.settings?.nodeId as number | undefined,
            endpointId: n.settings?.endpointId as number | undefined,
          },
        }))

        for (const n of restoredNodes) {
          const m = n.id.match(/^node_(\d+)$/)
          if (m) nodeIdCounter.current = Math.max(nodeIdCounter.current, parseInt(m[1]))
        }

        setNodes(restoredNodes)

        if (data.edges?.length) {
          setEdges(data.edges.map(e => ({
            id: e.id,
            source: e.source,
            target: e.target,
            sourceHandle: e.sourceHandle,
            targetHandle: e.targetHandle,
            type: 'powerFlow',
            data: { kw: 0 },
          })))
        }

        const cu = data.nodes.find(n => n.id === 'consumer_unit')
        instance.setCenter((cu?.x ?? 0) + 75, (cu?.y ?? 0) + 18, { zoom: 1 })
      })
      .catch(() => console.log('Failed to load nodes from API'))
  }, [setNodes, setEdges])

  return (
    <div style={{ display: 'flex', height: 'calc(100vh - 60px)' }}>
      <aside style={{ width: 240, flexShrink: 0, background: '#fff', borderRight: '1px solid #e2e8f0', display: 'flex', flexDirection: 'column', overflow: 'hidden' }}>
        <div style={{ padding: '12px 14px', borderBottom: '1px solid #e2e8f0' }}>
          <h2 style={{ fontSize: 12, fontWeight: 600, textTransform: 'uppercase', letterSpacing: '.06em', color: '#94a3b8', margin: 0 }}>Available Devices</h2>
        </div>
        <div style={{ flex: 1, overflowY: 'auto', padding: '8px 0' }}>
          {paletteLoading && (
            <div style={{ fontSize: 12, color: '#94a3b8', padding: '16px 14px' }}>Loading devices…</div>
          )}
          {!paletteLoading && palette.length === 0 && (
            <div style={{ fontSize: 12, color: '#94a3b8', padding: '16px 14px' }}>No devices found</div>
          )}
          {palette.map(device => (
            <div
              key={`${device.nodeId}-${device.endpointId}`}
              draggable
              onDragStart={e => onDragStart(e, device)}
              style={{ display: 'flex', alignItems: 'center', gap: 10, padding: '8px 14px', cursor: 'grab', userSelect: 'none', transition: 'background .12s' }}
              onMouseEnter={e => (e.currentTarget.style.background = '#f1f5f9')}
              onMouseLeave={e => (e.currentTarget.style.background = '')}
            >
              <div style={{ flex: 1, minWidth: 0 }}>
                <div style={{ fontWeight: 500, color: '#1e293b' }}>{device.label}</div>
                <div style={{ fontSize: 11, color: '#94a3b8', marginTop: 1 }}>
                  EP {device.endpointId}
                  {device.hasPowerMeasurement && <span style={{ marginLeft: 6, color: '#f59e0b' }}>⚡</span>}
                </div>
              </div>
            </div>
          ))}
        </div>
      </aside>

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

      <ToastStack toasts={toasts} onDismiss={dismissToast} />
    </div>
  )
}

export default () => (
  <ReactFlowProvider>
    <DnDProvider>
      <Home />
    </DnDProvider>
  </ReactFlowProvider>
);
