// Phase 7: browser SDK, mirroring client/include/sovd/client/sovd_client.hpp's
// shape (same wire format, same rules) rather than reinventing them:
// - Discover the API version from `/` instead of hardcoding `/v1/` (CLAUDE.md:
//   "/" is deliberately unversioned so a client can read api_versions before
//   it knows which prefix to use).
// - Retry only 503/504, never 423 (CLAUDE.md: retrying a lock conflict just
//   hammers whoever holds it).

export interface EntityInfo {
  path: string
  type: string
  has_backend: boolean
}

export interface FaultInfo {
  code: string
  status: string
}

export interface DataItemDoc {
  id: string
  did: string
  type: 'string' | 'float' | 'enum' | 'raw'
  access: 'read' | 'write' | 'read_write'
  io_control?: boolean
  requires_session?: string
  length?: number
  scale?: number
  unit?: string
  values?: Record<string, string>
}

export interface OperationDoc {
  id: string
  routine_id: string
  async: boolean
  requires_session?: string
}

export interface Capabilities {
  supports_batch_read: boolean
  supports_async_operations: boolean
  supports_io_control: boolean
}

export interface DocsResult {
  path: string
  type: string
  has_backend: boolean
  capabilities?: Capabilities
  data: DataItemDoc[]
  operations: OperationDoc[]
}

export interface DataValue {
  id: string
  did?: string
  value?: unknown
  unit?: string
  error?: string
  message?: string
}

export class SovdError extends Error {
  status: number
  code: string
  constructor(status: number, code: string, message: string) {
    super(`${code}: ${message}`)
    this.status = status
    this.code = code
  }
}

export class SovdClient {
  apiBase: string | null = null
  readonly baseUrl: string
  // SOVD_REVIEW_FEEDBACK.md Task 1b: optional OAuth2 bearer token. Empty
  // (the default) sends no Authorization header at all, so the no-auth demo
  // path this project started with keeps working unchanged -- same
  // "unset means no check" shape as the server's own SOVD_OAUTH2_SECRET.
  token: string | null = null

  constructor(baseUrl: string) {
    this.baseUrl = baseUrl
  }

  async ensureApiBase(): Promise<string> {
    if (this.apiBase) return this.apiBase
    const res = await fetch(`${this.baseUrl}/`)
    if (!res.ok) throw new SovdError(res.status, 'ROOT_UNREACHABLE', 'failed to reach server root')
    const body = await res.json()
    const version: string = (body.api_versions ?? [])[0] ?? 'v1'
    this.apiBase = `${this.baseUrl}/${version}`
    return this.apiBase
  }

  async request(
    method: string,
    path: string,
    opts: { body?: unknown; lockId?: string } = {},
  ): Promise<any> {
    const base = await this.ensureApiBase()
    const headers: Record<string, string> = {}
    if (opts.body !== undefined) headers['Content-Type'] = 'application/json'
    if (opts.lockId) headers['X-SOVD-Lock-Id'] = opts.lockId
    if (this.token) headers['Authorization'] = `Bearer ${this.token}`

    let backoffMs = 200
    for (let attempt = 0; ; attempt++) {
      const res = await fetch(base + path, {
        method,
        headers,
        body: opts.body !== undefined ? JSON.stringify(opts.body) : undefined,
      })
      if ((res.status === 503 || res.status === 504) && attempt < 3) {
        await new Promise((r) => setTimeout(r, backoffMs))
        backoffMs *= 2
        continue
      }
      if (!res.ok) {
        const raw = await res.text()
        let code = `HTTP_${res.status}`
        let message = raw
        try {
          const err = JSON.parse(raw)
          code = err.error ?? code
          message = err.message ?? message
        } catch {
          // body wasn't the usual {error,message} shape -- keep the raw text
        }
        throw new SovdError(res.status, code, message)
      }
      const text = await res.text()
      return text ? JSON.parse(text) : {}
    }
  }

  async listEntities(): Promise<EntityInfo[]> {
    return (await this.request('GET', '/entities')).items
  }

  async getDocs(path: string): Promise<DocsResult> {
    return this.request('GET', `/entities/${path}/docs`)
  }

  async getFaults(path: string, statusFilter = ''): Promise<FaultInfo[]> {
    const qs = statusFilter ? `?status=${encodeURIComponent(statusFilter)}` : ''
    return (await this.request('GET', `/entities/${path}/faults${qs}`)).faults
  }

  async clearFaults(path: string, lockId: string): Promise<void> {
    await this.request('DELETE', `/entities/${path}/faults`, { lockId })
  }

  async getDataBatch(path: string, ids: string[]): Promise<DataValue[]> {
    const qs = ids.map(encodeURIComponent).join(',')
    return (await this.request('GET', `/entities/${path}/data?ids=${qs}`)).items
  }

  async putData(path: string, id: string, value: unknown, lockId: string): Promise<void> {
    await this.request('PUT', `/entities/${path}/data/${id}`, { body: { value }, lockId })
  }

  async acquireLock(path: string, ttlSeconds: number): Promise<string> {
    return (await this.request('POST', `/entities/${path}/locks`, { body: { ttl_seconds: ttlSeconds } })).lock_id
  }

  async renewLock(path: string, lockId: string, ttlSeconds: number): Promise<void> {
    await this.request('PUT', `/entities/${path}/locks/${lockId}`, { body: { ttl_seconds: ttlSeconds } })
  }

  async releaseLock(path: string, lockId: string): Promise<void> {
    await this.request('DELETE', `/entities/${path}/locks/${lockId}`)
  }

  // Best-effort, synchronous-dispatch release for a `beforeunload` handler --
  // no retry, doesn't await the response, uses fetch's `keepalive` flag so
  // the request can survive the page tearing down (the modern replacement
  // for sendBeacon, which is POST-only and can't do DELETE). Silently does
  // nothing if the API base was never resolved (no lock could exist yet).
  releaseLockBestEffort(path: string, lockId: string): void {
    if (!this.apiBase) return
    const headers: Record<string, string> = { 'X-SOVD-Lock-Id': lockId }
    if (this.token) headers['Authorization'] = `Bearer ${this.token}`
    fetch(`${this.apiBase}/entities/${path}/locks/${lockId}`, {
      method: 'DELETE',
      headers,
      keepalive: true,
    }).catch(() => {})
  }

  // SOVD_REVIEW_FEEDBACK.md Task 1c: EventSource cannot set an Authorization
  // header, so the stream endpoint can't be gated the same way every other
  // route is once OAuth2 is on. When a token is configured, mint a
  // short-lived single-use ticket through a normal bearer-authenticated
  // POST first and carry *that* in the query string instead of the token
  // itself -- 30s TTL, burned on first use, so it's useless the moment the
  // stream has actually opened. No-op (no ticket param at all) when no
  // token is set, matching every other "unset means no check" seam in this
  // project -- the plain no-auth demo path is unaffected.
  async streamUrl(path: string, id: string, intervalMs: number): Promise<string> {
    const base = await this.ensureApiBase()
    let ticketParam = ''
    if (this.token) {
      const ticket = await this.request('POST', `/entities/${path}/data/${id}/stream-ticket`)
      ticketParam = `&ticket=${encodeURIComponent(ticket.ticket)}`
    }
    return `${base}/entities/${path}/data/${id}/stream?interval_ms=${intervalMs}${ticketParam}`
  }
}
