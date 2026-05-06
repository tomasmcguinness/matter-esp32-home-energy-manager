import { ReactFlow, Background, BackgroundVariant, useNodesState, type Node, type Edge, type ReactFlowInstance } from '@xyflow/react'
import '@xyflow/react/dist/style.css'
import { useCallback } from 'react'
import { PowerFlowEdge } from './PowerFlowEdge'
import { GridModal } from './GridModal'
import { useState } from 'react'

const edgeTypes = { powerFlow: PowerFlowEdge }

type SavedNodeConfig = { id: string; x: number; y: number; settings: Record<string, unknown> }

const initialNodes: Node[] = [
  {
    id: 'grid',
    position: { x: 0, y: -200 },
    draggable: true,
    data: { label: 'Grid' },
  },
  {
    id: 'consumer_unit',
    position: { x: 0, y: 0 },
    draggable: true,
    data: { label: 'Consumer Unit' },
  },
  {
    id: 'appliance_1',
    position: { x: 110, y: 110 },
    draggable: true,
    data: { label: 'Appliance 1' },
  },
  {
    id: 'inverter',
    position: { x: 110, y: 310 },
    draggable: true,
    data: { label: 'Solax Inverter' },
  },
]

const initialEdges: Edge[] = [
  {
    id: 'grid-consumer_unit',
    source: 'grid',
    target: 'consumer_unit',
    type: 'powerFlow',
    data: { direction: 'out', kw: 3.2 },
  },
  {
    id: 'consumer_unit-appliance_0',
    source: 'consumer_unit',
    target: 'appliance_1',
    type: 'powerFlow',
    data: { direction: 'in', kw: 0 },
  },
  {
    id: 'inverter-consumer_unit',
    source: 'inverter',
    target: 'consumer_unit',
    type: 'powerFlow',
    data: { direction: '  ', kw: 2.8 },
  }
]

function Home() {
  const [nodes, setNodes, onNodesChange] = useNodesState(initialNodes)
  const [gridModalOpen, setGridModalOpen] = useState(false)

  const onNodeDoubleClick = useCallback((_: React.MouseEvent, node: Node) => {
    if (node.id === 'grid') setGridModalOpen(true)
  }, [])

  const onNodeDragStop = useCallback((_: React.MouseEvent, node: Node) => {
    fetch(`/api/nodes/${node.id}`, {
      method: 'PUT',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify({ x: node.position.x, y: node.position.y }),
    }).catch(() => {})
  }, [])

  const onInit = useCallback((instance: ReactFlowInstance) => {
    fetch('/api/nodes')
      .then(r => r.ok ? r.json() : Promise.reject())
      .then((data: { nodes: SavedNodeConfig[] }) => {
        setNodes(prev => prev.map(node => {
          const saved = data.nodes.find(n => n.id === node.id)
          return saved ? { ...node, position: { x: saved.x, y: saved.y } } : node
        }))
        const cu = data.nodes.find(n => n.id === 'consumer_unit')
        const refNode = instance.getNode('consumer_unit')
        const w = refNode?.measured?.width ?? 150
        const h = refNode?.measured?.height ?? 36
        const x = (cu?.x ?? refNode?.position.x ?? 0) + w / 2
        const y = (cu?.y ?? refNode?.position.y ?? 0) + h / 2
        instance.setCenter(x, y, { zoom: 1 })
      })
      .catch(() => {
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
    <div style={{ height: 'calc(100vh - 60px)' }}>
      <ReactFlow
        nodes={nodes}
        onNodesChange={onNodesChange}
        edges={initialEdges}
        edgeTypes={edgeTypes}
        onInit={onInit}
        onNodeDoubleClick={onNodeDoubleClick}
        onNodeDragStop={onNodeDragStop}
        nodesDraggable={true}
        nodesConnectable={true}
      >
        <Background variant={BackgroundVariant.Dots} color="#cbd5e1" gap={24} size={1.5} />
      </ReactFlow>
      {gridModalOpen && (
        <GridModal
          onSave={() => setGridModalOpen(false)}
          onCancel={() => setGridModalOpen(false)}
        />
      )}
    </div>
  )
}

export default Home
