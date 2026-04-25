import React, { useEffect, useRef, useCallback } from 'react'

/**
 * FlowCanvas — draws animated SVG bezier curves between device cards.
 *
 * Props:
 *   sourceRefs   — { [deviceId]: React ref } for source-row cards
 *   hubRef       — React ref for the consumer unit card
 *   applianceRefs— { [deviceId]: React ref } for appliance-row cards
 *   sourceDevices— device objects for the source row (for coloring)
 *   applianceDevices — device objects for the appliance row
 */

const LINE_COLORS = {
  GRID:           '#3b82f6',
  SOLAR_INVERTER: '#f59e0b',
  BATTERY:        '#8b5cf6',
  default:        '#4a90d9',
}

export default function FlowCanvas({ sourceRefs, hubRef, applianceRefs, sourceDevices, applianceDevices }) {
  const svgRef = useRef(null)
  const containerRef = useRef(null)

  const draw = useCallback(() => {
    const svg = svgRef.current
    const container = containerRef.current
    if (!svg || !container) return

    const cvRect = container.getBoundingClientRect()

    function center(el) {
      if (!el) return null
      const r = el.getBoundingClientRect()
      return {
        x:      r.left + r.width / 2  - cvRect.left,
        top:    r.top                  - cvRect.top,
        bottom: r.top + r.height       - cvRect.top,
      }
    }

    const hub = hubRef.current ? center(hubRef.current) : null
    if (!hub) return

    const paths = []

    // Source cards → consumer unit (top)
    sourceDevices.forEach(device => {
      const el = sourceRefs[device.id]?.current
      const pos = center(el)
      if (!pos || !hub) return
      const color = LINE_COLORS[device.type] ?? LINE_COLORS.default
      const isIdle = !device.online || device.powerKw === 0
      const midY = (pos.bottom + hub.top) / 2
      paths.push(
        <path
          key={`src-${device.id}`}
          className={`flow-line ${isIdle ? 'idle' : ''}`}
          style={{ stroke: color }}
          d={`M ${pos.x} ${pos.bottom} C ${pos.x} ${midY}, ${hub.x} ${midY}, ${hub.x} ${hub.top}`}
        />
      )
    })

    // Consumer unit (bottom) → appliance cards
    applianceDevices.forEach(device => {
      const el = applianceRefs[device.id]?.current
      const pos = center(el)
      if (!pos || !hub) return
      const isIdle = !device.online || device.powerKw === 0
      const midY = (hub.bottom + pos.top) / 2
      paths.push(
        <path
          key={`app-${device.id}`}
          className={`flow-line ${isIdle ? 'idle' : ''}`}
          style={{ stroke: LINE_COLORS.default }}
          d={`M ${hub.x} ${hub.bottom} C ${hub.x} ${midY}, ${pos.x} ${midY}, ${pos.x} ${pos.top}`}
        />
      )
    })

    svg.replaceChildren(...paths.map(p => {
      // Convert React element to real DOM node
      const el = document.createElementNS('http://www.w3.org/2000/svg', 'path')
      el.setAttribute('class', p.props.className)
      el.setAttribute('d', p.props.d)
      if (p.props.style?.stroke) el.setAttribute('stroke', p.props.style.stroke)
      return el
    }))
  }, [sourceRefs, hubRef, applianceRefs, sourceDevices, applianceDevices])

  useEffect(() => {
    // Small delay to let layout settle, then draw
    const t = setTimeout(draw, 80)
    window.addEventListener('resize', draw)
    return () => { clearTimeout(t); window.removeEventListener('resize', draw) }
  }, [draw])

  return (
    <div ref={containerRef} className="flow-canvas-wrap">
      <svg ref={svgRef} className="connections-svg" xmlns="http://www.w3.org/2000/svg" />
    </div>
  )
}
