import { ReactFlow, Background, BackgroundVariant, useNodesState, useEdgesState, type Node, type Edge, type ReactFlowInstance, addEdge } from '@xyflow/react'
import '@xyflow/react/dist/style.css'
import { useCallback, useEffect, useRef, useState } from 'react'
import { PowerFlowEdge } from './PowerFlowEdge'
import { GridModal } from './GridModal'
import { useWebSocket, type WsMessage } from './useWebSocket'

const edgeTypes = { powerFlow: PowerFlowEdge }

type SavedNodeConfig = { id: string; x: number; y: number; settings: Record<string, unknown> }

type DeviceSpec = {
  nodeId: number,
  type: string
  label: string
  desc: string
  icon: string
  iconBg: string
  dropTarget: 'canvas' | 'edge'
  badge: 'node' | 'edge'
}

type ApiEndpoint = {
  endpointId: number
  label: string
  included: boolean
  deviceTypes: number[]
}

type ApiDevice = {
  nodeId: number
  vendorName: string
  productName: string
  endpoints: ApiEndpoint[]
}

const initialNodes: Node[] = [
  {
    id: 'consumer_unit',
    position: { x: 0, y: 0 },
    draggable: true,
    deletable: false,
    data: { label: '🏠 Consumer Unit' },
  },
]

const initialEdges: Edge[] = [
  //   {
  //     id: 'meter-consumer_unit',
  //     source: 'meter',
  //     target: 'consumer_unit',
  //     type: 'powerFlow',
  //     data: { kw: 0 }
  //   },
]

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
  const [edges, setEdges, onEdgesChange] = useEdgesState(initialEdges)
  const [gridModalOpen, setGridModalOpen] = useState(false)
  const [palette, setPalette] = useState<DeviceSpec[]>([])
  const [paletteLoading, setPaletteLoading] = useState(true)
  const [toasts, setToasts] = useState<Toast[]>([])
  const reactFlowInstance = useRef<ReactFlowInstance | null>(null)
  const nodeIdCounter = useRef(10)
  const toastIdCounter = useRef(0)

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
      const d = msg.data as { productName?: string; vendorName?: string }
      const name = [d.vendorName, d.productName].filter(Boolean).join(' ')
      addToast(`New device commissioned: ${name || 'Unknown device'}`)
    } else if (msg.type === 'power_update') {
      const d = msg.data as { nodeId: number; endpointId: number; mw: number }
      setEdges(prev => prev.map(e => {
        if (e.source === 'meter' && e.target === 'consumer_unit' && e.type === 'powerFlow') {
          return { ...e, data: { ...e.data, kw: d.mw } }
        }
        return e
      }))
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
    fetch('/api/devices')
      .then(r => r.ok ? r.json() : Promise.reject())
      .then((data: { devices: ApiDevice[] }) => setPalette(data.devices))
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

  const onDrop = useCallback((e: React.DragEvent) => {
    e.preventDefault()
    const raw = e.dataTransfer.getData('application/reactflow')
    if (!raw || !reactFlowInstance.current) return
    const device: DeviceSpec = JSON.parse(raw)
    //if (device.dropTarget !== 'canvas') return

    const position = reactFlowInstance.current.screenToFlowPosition({ x: e.clientX, y: e.clientY })
    const id = `node_${++nodeIdCounter.current}`
    setNodes(prev => [
      ...prev,
      { id, position, draggable: true, data: { label: `${device.icon} ${device.label}` } },
    ])
    fetch(`/api/nodes/${id}`, {
      method: 'PUT',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify({ x: position.x, y: position.y }),
    }).catch(() => { })
  }, [setNodes])

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

  const onConnect = useCallback((params: any) => {
    setEdges((eds) => addEdge({ ...params, type: 'powerFlow', data: { kw: 0 } }, eds))
  },
    [],
  );

  const onInit = useCallback((instance: ReactFlowInstance) => {
    reactFlowInstance.current = instance
    fetch('/api/nodes')
      .then(r => r.ok ? r.json() : Promise.reject())
      .then((data: { nodes: SavedNodeConfig[] }) => {
        setNodes(prev => {
          const existingIds = new Set(prev.map(n => n.id))
          const updated = prev.map(node => {
            const saved = data.nodes.find(n => n.id === node.id)
            return saved ? { ...node, position: { x: saved.x, y: saved.y } } : node
          })
          const added = data.nodes
            .filter(n => !existingIds.has(n.id) && typeof n.settings?.label === 'string')
            .map(n => ({
              id: n.id,
              position: { x: n.x, y: n.y },
              draggable: true,
              data: { label: n.settings.label as string },
            }))

          // Keep nodeIdCounter ahead of any restored node_N ids
          for (const n of added) {
            const m = n.id.match(/^node_(\d+)$/)
            if (m) nodeIdCounter.current = Math.max(nodeIdCounter.current, parseInt(m[1]))
          }
          return [...updated, ...added]
        })
        const cu = data.nodes.find(n => n.id === 'consumer_unit')
        const refNode = instance.getNode('consumer_unit')
        const w = refNode?.measured?.width ?? 150
        const h = refNode?.measured?.height ?? 36
        const x = (cu?.x ?? refNode?.position.x ?? 0) + w / 2
        const y = (cu?.y ?? refNode?.position.y ?? 0) + h / 2
        instance.setCenter(x, y, { zoom: 1 })
      })
      .catch(() => {
        console.log('Failed to load saved node positions, using defaults')
        const refNode = instance.getNode('consumer_unit')
        const w = refNode?.measured?.width ?? 150
        const h = refNode?.measured?.height ?? 36
        instance.setCenter(
          (refNode?.position.x ?? 0) + w / 2,
          (refNode?.position.y ?? 0) + h / 2,
          { zoom: 1 },
        )
      })
  }, [setNodes])

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
              key={device.nodeId}
              draggable
              onDragStart={e => onDragStart(e, device)}
              style={{ display: 'flex', alignItems: 'center', gap: 10, padding: '8px 14px', cursor: 'grab', userSelect: 'none', transition: 'background .12s' }}
              onMouseEnter={e => (e.currentTarget.style.background = '#f1f5f9')}
              onMouseLeave={e => (e.currentTarget.style.background = '')}
            >
              <div style={{ flex: 1, minWidth: 0 }}>
                <div style={{ fontWeight: 500, color: '#1e293b' }}>0x{device.nodeId}</div>
              </div>
            </div>
          ))}
        </div>
      </aside>

      <div className="wrapper" style={{ flex: 1, position: 'relative' }} onDrop={onDrop} onDragOver={onDragOver}>
        <ReactFlow
          style={{ height: '100%' }}
          nodes={nodes}
          onNodesChange={onNodesChange}
          edges={edges}
          onEdgesChange={onEdgesChange}
          onConnect={onConnect}
          edgeTypes={edgeTypes}
          onInit={onInit}
          onNodeDoubleClick={onNodeDoubleClick}
          onNodeDragStop={onNodeDragStop}
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

export default Home
