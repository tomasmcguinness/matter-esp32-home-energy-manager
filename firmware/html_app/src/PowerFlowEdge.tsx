import { type EdgeProps, getBezierPath, EdgeLabelRenderer } from '@xyflow/react'

const CHEVRON_COUNT = 8
const DURATION = 2

type PowerFlowData = { direction?: 'in' | 'out'; kw?: number }

export function PowerFlowEdge({
  sourceX, sourceY, targetX, targetY,
  sourcePosition, targetPosition, data,
}: EdgeProps) {
  const [edgePath, labelX, labelY] = getBezierPath({ sourceX, sourceY, sourcePosition, targetX, targetY, targetPosition })

  const { direction, kw } = (data ?? {}) as PowerFlowData
  const isOut = direction === 'out'
  const lineColor = isOut ? 'rgba(99,153,34,0.3)' : 'rgba(226,75,74,0.3)'
  const chevronColor = isOut ? 'rgba(99,153,34,0.8)' : 'rgba(226,75,74,0.8)'
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
      {Array.from({ length: CHEVRON_COUNT }, (_, i) => (
        <polyline
          key={i}
          points="-8,-5.6 0,0 -8,5.6"
          fill="none"
          stroke={chevronColor}
          strokeWidth={2}
          strokeLinecap="round"
          strokeLinejoin="round"
        >
          <animateMotion
            dur={`${DURATION}s`}
            begin={`${-(i / CHEVRON_COUNT) * DURATION}s`}
            repeatCount="indefinite"
            path={edgePath}
            rotate="auto"
          />
        </polyline>
      ))}
      {kw !== undefined && (
        <EdgeLabelRenderer>
          <div style={labelStyle} className="nodrag nopan">
            {kw} kW
          </div>
        </EdgeLabelRenderer>
      )}
    </>
  )
}
