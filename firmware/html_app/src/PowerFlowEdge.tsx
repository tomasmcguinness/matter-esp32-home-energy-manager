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

type PowerFlowData = { kw?: number }

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

  const { kw } = (data ?? {}) as PowerFlowData
  const isIdle = kw === 0
  const isOut = kw && kw < 0;
  const formattedkw = kw == undefined ? '' : Math.abs(kw!).toFixed(1);

  // For 'out', animate along the geometrically reversed path so chevrons travel
  // target → source. Swapping source ↔ target with their handle positions gives
  // the exact reverse bezier, so chevrons follow the drawn line correctly.
  const [animPath] = isOut
    ? getBezierPath({ sourceX: targetX, sourceY: targetY, sourcePosition: targetPosition, targetX: sourceX, targetY: sourceY, targetPosition: sourcePosition })
    : [edgePath]

  const lineColor = isIdle ? 'rgba(148,163,184,0.4)' : isOut ? 'rgba(99,153,34,0.3)' : 'rgba(226,75,74,0.3)'
  const chevronColor = isOut ? 'rgba(99,153,34,0.8)' : 'rgba(226,75,74,0.8)'
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
    background: isOut ? '#EAF3DE' : '#FCEBEB',
    color: isOut ? '#3B6D11' : '#A32D2D',
    border: `1px solid ${isOut ? '#97C459' : '#F09595'}`,
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
