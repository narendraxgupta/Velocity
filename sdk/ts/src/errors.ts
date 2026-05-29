/**
 * Errors thrown by the SDK. Two concrete shapes:
 *
 *   - VelocityApiError: any non-2xx response. Carries `status` and a
 *     decoded `body` (string when JSON parse fails).
 *   - VelocityNetworkError: lower-layer failures (DNS, TCP, abort).
 *
 * The common base is `VelocityError` so callers can catch broadly.
 */

export class VelocityError extends Error {
  // Forward the ES2022 `{ cause }` options bag so subclasses can
  // attach the underlying transport/abort error without manually
  // assigning `this.cause` (which would need `override` annotations
  // under tsconfig's noImplicitOverride).
  constructor(message: string, options?: ErrorOptions) {
    super(message, options)
    this.name = 'VelocityError'
  }
}

export class VelocityApiError extends VelocityError {
  readonly status: number
  readonly body: unknown
  constructor(status: number, message: string, body: unknown) {
    super(`HTTP ${status} — ${message}`)
    this.name = 'VelocityApiError'
    this.status = status
    this.body = body
  }
}

export class VelocityNetworkError extends VelocityError {
  // `Error` already declares `cause: unknown` since ES2022. With
  // `noImplicitOverride: true` in tsconfig we have to either mark
  // this with `override` or, cleaner, pass `cause` through the
  // standard Error constructor's options bag. We go with the latter
  // so `err.cause` Just Works in tooling that already understands
  // the spec (Node, devtools, sentry).
  constructor(message: string, cause: unknown) {
    super(message, { cause })
    this.name = 'VelocityNetworkError'
  }
}
