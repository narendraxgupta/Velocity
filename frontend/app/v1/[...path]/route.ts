/**
 * Same-origin STREAMING proxy for the API gateway.
 *
 * The browser calls `/v1/*` on its own origin; this handler forwards to the
 * gateway over the internal network and streams the response body straight
 * back to the client.
 *
 * Why a route handler instead of a next.config `rewrites` entry:
 *   A `rewrites` proxy BUFFERS the upstream response — fine for normal JSON,
 *   but fatal for `text/event-stream` (SSE). The live benchmark feed
 *   (`/v1/benchmarks/{id}/stream`) and the marketdata ticker
 *   (`/v1/marketdata/stream`) are infinite streams, so buffering means the
 *   browser's EventSource never receives a byte and flips to `error`. A route
 *   handler returns the upstream `ReadableStream` directly, so frames flow as
 *   the gateway emits them.
 *
 * Request bodies (uploads) are buffered so Content-Length is preserved for the
 * gateway; response bodies are always streamed.
 */
import { type NextRequest } from 'next/server'

// Never statically optimize — this is a live proxy.
export const dynamic = 'force-dynamic'
// Node runtime: we dial an internal Docker hostname and stream with undici.
export const runtime = 'nodejs'

const GATEWAY =
  process.env.INTERNAL_API_GATEWAY_URL ||
  process.env.API_GATEWAY_ORIGIN ||
  'http://api-gateway:8080'

const HOP_BY_HOP = ['connection', 'keep-alive', 'transfer-encoding', 'upgrade']

async function proxy(
  req: NextRequest,
  ctx: { params: Promise<{ path?: string[] }> },
): Promise<Response> {
  const { path = [] } = await ctx.params
  const target = `${GATEWAY}/v1/${path.join('/')}${req.nextUrl.search}`

  const headers = new Headers(req.headers)
  headers.delete('host')
  headers.delete('content-length')
  // Ask the gateway for an unencoded body so we can stream it untouched.
  headers.delete('accept-encoding')
  for (const h of HOP_BY_HOP) headers.delete(h)

  // Buffer request bodies (uploads are small) so Content-Length is set; GET/HEAD
  // carry none.
  let body: ArrayBuffer | undefined
  if (req.method !== 'GET' && req.method !== 'HEAD') {
    const buf = await req.arrayBuffer()
    if (buf.byteLength) body = buf
  }

  let upstream: Response
  try {
    upstream = await fetch(target, {
      method: req.method,
      headers,
      body,
      redirect: 'manual',
      cache: 'no-store',
    })
  } catch (e) {
    return new Response(
      JSON.stringify({ error: 'gateway unreachable', detail: (e as Error).message }),
      { status: 502, headers: { 'content-type': 'application/json' } },
    )
  }

  const respHeaders = new Headers(upstream.headers)
  respHeaders.delete('content-encoding')
  respHeaders.delete('content-length')
  respHeaders.delete('transfer-encoding')
  for (const h of HOP_BY_HOP) respHeaders.delete(h)
  // Tell any intermediary (and Next itself) not to buffer the stream.
  respHeaders.set('X-Accel-Buffering', 'no')

  // Pass the upstream ReadableStream straight through — this is what makes SSE
  // work end-to-end.
  return new Response(upstream.body, {
    status: upstream.status,
    statusText: upstream.statusText,
    headers: respHeaders,
  })
}

export {
  proxy as GET,
  proxy as POST,
  proxy as PUT,
  proxy as PATCH,
  proxy as DELETE,
  proxy as HEAD,
  proxy as OPTIONS,
}
