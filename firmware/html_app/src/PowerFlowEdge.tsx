import { useMemo } from 'react'
import { type EdgeProps, getBezierPath, EdgeLabelRenderer } from '@xyflow/react'

const CHEVRON_SPACING = 30  // px between chevrons
const SPEED = 80            // px per second
const MIN_KW = 0
const MAX_KW = 10
const MIN_SIZE = 4
const MAX_SIZE = 14

function kwToChevronSize(kw: number | undefined): number {
  if (kw === undefined) return 8
  const t = Math.max(0, Math.min(1, (kw - MIN_KW) / (MAX_KW - MIN_KW)))
  return MIN_SIZE + t * (MAX_SIZE - MIN_SIZE)
}

// kw: flow along the edge. Negative = source → target, positive = target → source.
// idle: the metered appliance is in standby or off, so draw the edge at rest
// even though it still reads a small non-zero power.
type PowerFlowData = { kw?: number; idle?: boolean }

function measurePath(d: string): number {
  const el = document.createElementNS('http://www.w3.org/2000/svg', 'path')
  el.setAttribute('d', d)
  return el.getTotalLength()
}

export function PowerFlowEdge({
  sourceX, sourceY, targetX, targetY,
  sourcePosition, targetPosition, data,
}: EdgeProps) {
  const [edgePath, labelX, labelY] = getBezierPath({ sourceX, sourceY, sourcePosition, targetX, targetY, targetPosition })

  const { kw, idle } = (data ?? {}) as PowerFlowData
  const isIdle = kw === 0 || idle === true
  const isReverse = kw !== undefined && kw > 0
  const formattedkw = kw == undefined ? '' : Math.abs(kw!).toFixed(1);

  // For target → source, animate along the geometrically reversed path so chevrons travel
  // target → source. Swapping source ↔ target with their handle positions gives
  // the exact reverse bezier, so chevrons follow the drawn line correctly.
  const [animPath] = isReverse
    ? getBezierPath({ sourceX: targetX, sourceY: targetY, sourcePosition: targetPosition, targetX: sourceX, targetY: sourceY, targetPosition: sourcePosition })
    : [edgePath]

  // Neutral grey until good/bad flow colouring is decided; direction is shown by the chevrons.
  const lineColor = isIdle ? 'rgba(148,163,184,0.4)' : 'rgba(100,116,139,0.35)'
  const chevronColor = 'rgba(71,85,105,0.8)'
  const size = kwToChevronSize(Math.abs(kw!))
  const chevronPoints = `-${size},-${(size * 0.7).toFixed(1)} 0,0 -${size},${(size * 0.7).toFixed(1)}`

  const { chevronCount, duration } = useMemo(() => {
    const length = measurePath(edgePath)
    return {
      chevronCount: Math.max(1, Math.round(length / CHEVRON_SPACING)),
      duration: length / SPEED,
    }
  }, [edgePath])

  const labelStyle: React.CSSProperties = {
    position: 'absolute',
    transform: `translate(-50%, -50%) translate(${labelX}px, ${labelY}px)`,
    background: '#F1F5F9',
    color: '#475569',
    border: '1px solid #CBD5E1',
    borderRadius: 6,
    padding: '2px 8px',
    fontSize: 12,
    fontWeight: 500,
    whiteSpace: 'nowrap',
    pointerEvents: 'none',
  }

  return (
    <>
      <path d={edgePath} fill="none" stroke={lineColor} strokeWidth={2.5} strokeLinecap="round" />
      {!isIdle && Array.from({ length: chevronCount }, (_, i) => (
        <polyline
          key={i}
          points={chevronPoints}
          fill="none"
          stroke={chevronColor}
          strokeWidth={2}
          strokeLinecap="round"
          strokeLinejoin="round"
        >
          <animateMotion
            dur={`${duration}s`}
            begin={`${-((i + 0.5) / chevronCount) * duration}s`}
            repeatCount="indefinite"
            path={animPath}
            rotate="auto"
          />
        </polyline>
      ))}
      {kw !== undefined && !isIdle && (
        <EdgeLabelRenderer>
          <div style={labelStyle} className="nodrag nopan">
            {formattedkw} kW
          </div>
        </EdgeLabelRenderer>
      )}
    </>
  )
}
