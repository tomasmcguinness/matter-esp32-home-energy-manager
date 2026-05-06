import { ReactFlow, Background, BackgroundVariant, useNodesState, type Node, type Edge } from '@xyflow/react'
import '@xyflow/react/dist/style.css'
import { PowerFlowEdge } from './PowerFlowEdge'

const edgeTypes = { powerFlow: PowerFlowEdge }

const initialNodes: Node[] = [
  {
    id: 'grid',
    position: { x: 300, y: 200 },
    draggable: true,
    data: { label: 'Grid' },
  },
  {
    id: 'consumer_unit',
    position: { x: 300, y: 0 },
    draggable: true,
    data: { label: 'Consumer Unit' },
  },
]

const initialEdges: Edge[] = [
  {
    id: 'grid-consumer_unit',
    source: 'grid',
    target: 'consumer_unit',
    type: 'powerFlow',
    data: { direction: 'in', kw: 3.2 },
  },
]

function Home() {
  const [nodes, , onNodesChange] = useNodesState(initialNodes)

  return (
    <div style={{ height: 'calc(100vh - 60px)' }}>
      <ReactFlow
        nodes={nodes}
        onNodesChange={onNodesChange}
        edges={initialEdges}
        edgeTypes={edgeTypes}
        defaultViewport={{ x: 0, y: 0, zoom: 1 }}
        nodesDraggable={true}
        nodesConnectable={false}
      >
        <Background variant={BackgroundVariant.Dots} color="#cbd5e1" gap={24} size={1.5} />
      </ReactFlow>
    </div>
  )
}

export default Home
