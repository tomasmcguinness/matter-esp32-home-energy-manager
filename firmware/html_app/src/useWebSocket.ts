import { useCallback, useEffect, useRef, useState } from 'react'

export type WsMessage = { type: string; data: unknown }

export type WsReadyState = 'connecting' | 'open' | 'closed'

const BASE_RETRY_MS  = 1_000
const MAX_RETRY_MS   = 30_000

export function useWebSocket(onMessage: (msg: WsMessage) => void): WsReadyState {
  const [readyState, setReadyState] = useState<WsReadyState>('connecting')
  const wsRef        = useRef<WebSocket | null>(null)
  const retryMs      = useRef(BASE_RETRY_MS)
  const retryTimer   = useRef<ReturnType<typeof setTimeout> | null>(null)
  const onMessageRef = useRef(onMessage)
  onMessageRef.current = onMessage

  const connect = useCallback(() => {

    console.log('Connecting to WebSocket...')
    
    const proto = window.location.protocol === 'https:' ? 'wss:' : 'ws:'
    const ws = new WebSocket(`${proto}//${window.location.host}/ws`)
    wsRef.current = ws

    ws.onopen = () => {
      setReadyState('open')
      console.log('Connected to WebSocket!')
      retryMs.current = BASE_RETRY_MS
    }

    ws.onmessage = (ev) => {
      try {
        const msg = JSON.parse(ev.data as string) as WsMessage
        onMessageRef.current(msg)
      } catch {
        // ignore non-JSON frames
      }
    }

    ws.onclose = () => {
      setReadyState('closed')
      console.log('Closed connection to WebSocket!')
      retryTimer.current = setTimeout(() => {
        retryMs.current = Math.min(retryMs.current * 2, MAX_RETRY_MS)
        setReadyState('connecting')
        connect()
      }, retryMs.current)
    }

    ws.onerror = () => ws.close()
  }, [])

  useEffect(() => {
    connect()
    return () => {
      if (retryTimer.current) clearTimeout(retryTimer.current)
      wsRef.current?.close()
    }
  }, [connect])

  return readyState
}
