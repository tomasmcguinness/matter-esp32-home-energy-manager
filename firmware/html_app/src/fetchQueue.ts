// The firmware's HTTP server handles one request at a time and purges the
// least-recently-used socket once its small connection limit is reached, so a
// page that fires many requests at once sees some of them dropped. Requests
// made through queuedFetch run one after another, in the order they were made.

let tail: Promise<unknown> = Promise.resolve()

export function queuedFetch(input: string, init?: RequestInit): Promise<Response> {
  // A request aborted while waiting its turn is skipped rather than sent.
  const run = () => init?.signal?.aborted
    ? Promise.reject(new DOMException('Aborted', 'AbortError'))
    : fetch(input, init)
  const p = tail.then(run, run)
  tail = p.catch(() => { })
  return p
}

export function isAbortError(e: unknown): boolean {
  return e instanceof DOMException && e.name === 'AbortError'
}
