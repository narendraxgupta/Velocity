# ADR-006: Multi-tenant isolation strategy

- **Status**: Accepted
- **Date**: 2026-05-19
- **Deciders**: platform team

## Context

Velocity needs to host multiple competitive tenants (IICPC teams, plus
post-IICPC quant prop firms) on shared cluster hardware without:

1. **Data leakage** — Team A must never read Team B's submission
   artefacts, benchmark snapshots, leaderboard PII, or pcap captures.
2. **Resource starvation** — A buggy submission from Team A burning a
   core in a tight loop must not push Team B's benchmark over its
   p99 SLO.
3. **Side-channel scoring leakage** — Two tenants running at the same
   time on the same kernel scheduler will see correlated noise. We
   accept this for IICPC (it's the same noise floor for everyone) but
   document it loudly in the report.

The constraints are:

- Single Kubernetes cluster (DigitalOcean Premium AMD, 3 × 16-core nodes
  in the IICPC budget). Dedicated clusters per tenant is wrong-shaped
  spend.
- Existing services already wire to a single Redis, single Redpanda,
  single MinIO. Splitting per-tenant clusters would be a 4-week refactor.
- We need this working **before** IICPC submissions open, ~3 weeks.

## Decision

Three-layer scheme:

1. **Identity**: HS256 JWT in `Authorization: Bearer …`. Claims:
   `tid`, `sub`, `role`, `iat`, `exp`. Verified by the api-gateway and
   every Go service via `services/common-go/tenant`. The secret is
   shared (one cluster = one IdP), not per-tenant. Tokens are minted by
   `scripts/tenant.sh` for the IICPC demo path; production will swap in
   an OIDC issuer later (Phase 4.2 hook is already in place).
2. **Logical scoping in shared data plane** — every Redis key, Kafka
   topic, MinIO object path is prefixed:
   - Redis : `t:<tid>:<original>`
   - Kafka : `t.<tid>.<original>`
   - MinIO : `t/<tid>/<original>`
   The helpers in `api_gateway/tenant.h` (C++) and
   `services/common-go/tenant` (Go) are idempotent — calling
   `ScopedKey` twice doesn't double-prefix, which lets us migrate one
   service at a time.
3. **Physical scoping in compute plane** — each tenant gets a
   Kubernetes namespace `velocity-tenant-<tid>` with:
   - `ResourceQuota` (CPU, memory, ephemeral-storage, pod count)
   - `LimitRange` (default pod requests so quota math works)
   - Default-deny NetworkPolicy + targeted allow-rules to
     `velocity-data` and `velocity-control`
   - Pod Security Standard `restricted` enforced
   Sandbox pods (untrusted submission code) run in this namespace under
   the gVisor runtime class.

## Consequences

Good:

- Cross-tenant data access requires a sandbox escape AND a JWT forgery
  AND a Kubernetes namespace breach. Defense in depth.
- We keep a single Redis / Redpanda / MinIO; ops simpler.
- Idempotent prefixing means migration is non-disruptive: services not
  yet ported keep working against the `default` tenant.
- `tenant.sh` makes provisioning a tenant a 30-second operation.

Bad:

- Shared scheduler means two tenants benchmarking concurrently see
  correlated noise on p999 (~5% variance from co-tenancy in our
  micro-benchmarks). We document this on the report card; for the
  IICPC final round we'll move to one-tenant-at-a-time.
- Shared Redpanda means one tenant's pathological traffic pattern can
  affect another's broker latency. We'll add per-topic quotas in
  Redpanda v23.3 + (Phase 4.5+).
- Shared HS256 secret rotation is annoying — all services must restart.
  Acceptable today; ADR-007 will introduce JWKS rotation.

## Alternatives considered

**Cluster-per-tenant.** Strongest isolation. Rejected: 4× cluster bill,
4× operational surface, and the IICPC budget can't justify it for the
30-team workload.

**Single namespace + RBAC only.** Cheaper, but ResourceQuota is
namespace-scoped in Kubernetes so we'd lose quota enforcement. Also the
default-deny NetworkPolicy story gets messy.

**mTLS-only auth (no JWT).** Considered for service→service paths.
Rejected for client→gateway because browsers can't easily present
client certs. We'll layer SPIFFE/SPIRE in for service mesh later.

## References

- Kubernetes Pod Security Standards — <https://kubernetes.io/docs/concepts/security/pod-security-standards/>
- RFC 7519 — JSON Web Tokens
- RFC 4648 §5 — Base64url
- ADR-001 — gVisor over Firecracker (sandbox layer)
- ADR-005 — Redpanda over Kafka (data plane that this scopes)
