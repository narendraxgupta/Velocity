// Resource imports kept on one line each for git-blame friendliness.
// `.js` extensions are intentional — `moduleResolution: "bundler"` in
// tsconfig.json maps them back to the sibling `.ts` source at compile
// time, and the runtime emits real `.js` so the published package
// works under `"type": "module"` without an extension rewrite step.
import { VelocityApiError, VelocityNetworkError } from './errors.js'
import { SubmissionsResource } from './submissions.js'
import { BenchmarksResource } from './benchmarks.js'
import { LeaderboardResource } from './leaderboard.js'
import { AuditResource } from './audit.js'

export type VelocityClientOptions = {
  baseUrl: string
  bearer?: string
  userAgent?: string
  fetchImpl?: typeof fetch
  maxRetries?: number
}

export class VelocityClient {
  readonly submissions: SubmissionsResource
  readonly benchmarks:  BenchmarksResource
  readonly leaderboard: LeaderboardResource
  readonly audit:       AuditResource

  readonly baseUrl: string
  readonly bearer?: string

  private readonly userAgent: string
  private readonly fetchImpl: typeof fetch
  private readonly maxRetries: number

  constructor(opts: VelocityClientOptions) {
    if (!opts.baseUrl) throw new Error('VelocityClient: baseUrl is required')
    this.baseUrl   = opts.baseUrl.replace(/\/+$/, '')
    this.bearer    = opts.bearer
    this.userAgent = opts.userAgent ?? 'velocity-sdk-ts/0.1'
    this.fetchImpl = opts.fetchImpl ?? globalThis.fetch.bind(globalThis)
    this.maxRetries = opts.maxRetries ?? 2

    this.submissions = new SubmissionsResource(this)
    this.benchmarks  = new BenchmarksResource(this)
    this.leaderboard = new LeaderboardResource(this)
    this.audit       = new AuditResource(this)
  }

  /**
   * Low-level request runner. Resource classes use this to talk to the
   * gateway. Public on the client so advanced callers can call
   * unmodelled endpoints without rewriting auth/retry plumbing.
   */
  async request<T>(
    method: string,
    path: string,
    body?: BodyInit | object | null,
    extra: RequestInit = {},
  ): Promise<T> {
    const headers = new Headers(extra.headers)
    headers.set('Accept', 'application/json')
    headers.set('User-Agent', this.userAgent)
    if (this.bearer) headers.set('Authorization', `Bearer ${this.bearer}`)

    let serializedBody: BodyInit | undefined
    if (body != null) {
      if (typeof body === 'string' || body instanceof Blob ||
          body instanceof ArrayBuffer || body instanceof FormData ||
          body instanceof URLSearchParams ||
          (typeof ReadableStream !== 'undefined' && body instanceof ReadableStream)) {
        serializedBody = body as BodyInit
      } else {
        headers.set('Content-Type', 'application/json')
        serializedBody = JSON.stringify(body)
      }
    }

    let lastError: unknown = null
    for (let attempt = 0; attempt <= this.maxRetries; attempt++) {
      try {
        const res = await this.fetchImpl(`${this.baseUrl}${path}`, {
          ...extra,
          method,
          body: serializedBody,
          headers,
        })
        if (res.status >= 500 && attempt < this.maxRetries) {
          await sleep(backoffMs(attempt))
          continue
        }
        if (!res.ok) {
          const text = await safeText(res)
          let parsed: unknown
          try { parsed = JSON.parse(text) } catch { parsed = text }
          throw new VelocityApiError(res.status, (parsed as any)?.error ?? text, parsed)
        }
        const contentType = res.headers.get('content-type') ?? ''
        if (contentType.includes('application/json')) {
          return (await res.json()) as T
        }
        return (await res.text()) as unknown as T
      } catch (err) {
        if (err instanceof VelocityApiError) throw err
        lastError = err
        if (attempt === this.maxRetries) break
        await sleep(backoffMs(attempt))
      }
    }
    throw new VelocityNetworkError('request failed', lastError)
  }

  /**
   * Open an SSE stream. Yields parsed event payloads (assumes JSON
   * data lines, which is what the gateway emits). Closes when the
   * server closes the stream OR when the AbortController fires.
   */
  async *stream<T>(path: string, signal?: AbortSignal): AsyncGenerator<T, void, void> {
    const headers = new Headers({
      Accept: 'text/event-stream',
      'User-Agent': this.userAgent,
    })
    if (this.bearer) headers.set('Authorization', `Bearer ${this.bearer}`)

    const res = await this.fetchImpl(`${this.baseUrl}${path}`, { headers, signal })
    if (!res.ok) {
      const text = await safeText(res)
      throw new VelocityApiError(res.status, text, text)
    }
    if (!res.body) {
      throw new VelocityApiError(500, 'streaming not supported by transport', null)
    }
    const reader = res.body.getReader()
    const decoder = new TextDecoder('utf-8')
    let buffer = ''
    while (true) {
      const { value, done } = await reader.read()
      if (done) break
      buffer += decoder.decode(value, { stream: true })
      let idx: number
      while ((idx = buffer.indexOf('\n\n')) >= 0) {
        const frame = buffer.slice(0, idx)
        buffer = buffer.slice(idx + 2)
        const dataLine = frame
          .split('\n')
          .filter(l => l.startsWith('data:'))
          .map(l => l.slice(5).trim())
          .join('\n')
        if (!dataLine) continue
        try {
          yield JSON.parse(dataLine) as T
        } catch {
          // Skip malformed frames — the server occasionally heartbeats
          // with `:keepalive\n` lines and we should not blow up.
        }
      }
    }
  }
}

async function safeText(res: Response): Promise<string> {
  try { return await res.text() } catch { return '' }
}

function sleep(ms: number): Promise<void> {
  return new Promise(resolve => setTimeout(resolve, ms))
}

function backoffMs(attempt: number): number {
  return Math.min(1000, (1 << attempt) * 100 + Math.floor(Math.random() * 50))
}
