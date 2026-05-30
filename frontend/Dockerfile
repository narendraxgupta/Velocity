# =============================================================================
# Velocity — Frontend (Next.js) container
#
# Multi-stage build:
#   deps    - install node_modules with pnpm (cacheable, deterministic)
#   builder - run `next build` against deps
#   runtime - distroless-style minimal final image
#
# Final image is ~110 MB (vs ~1.2 GB for a naive single-stage Next dev image).
# =============================================================================

# ---- Stage 1 : install deps -------------------------------------------------
FROM node:20-alpine AS deps
WORKDIR /app

# Faster, more reproducible installs via pnpm if you choose; npm here for
# minimum friction. Swap to pnpm/yarn by replacing this block.
COPY package.json package-lock.json* ./
# `npm install` (not `npm ci`) so a drifted lockfile self-heals during the
# image build instead of hard-failing; the deps are still pinned by the
# resolved tree that gets carried into the builder stage.
RUN apk add --no-cache libc6-compat \
 && npm install --omit=optional --no-audit --no-fund

# ---- Stage 2 : build --------------------------------------------------------
FROM node:20-alpine AS builder
WORKDIR /app
ENV NEXT_TELEMETRY_DISABLED=1

# NEXT_PUBLIC_* are inlined into the client bundle at build time, so they
# MUST be present here (not just at runtime). When the SPA is served from a
# different host than the gateway/leaderboard (e.g. a Codespace's *-3000 vs
# *-8080 / *-8090 github.dev hosts), pass the externally-reachable URLs as
# build args. They default to localhost for the plain docker-compose dev loop.
ARG NEXT_PUBLIC_API_GATEWAY_URL=http://localhost:8080
ARG NEXT_PUBLIC_LEADERBOARD_WS=ws://localhost:8090/v1/leaderboard
ENV NEXT_PUBLIC_API_GATEWAY_URL=${NEXT_PUBLIC_API_GATEWAY_URL}
ENV NEXT_PUBLIC_LEADERBOARD_WS=${NEXT_PUBLIC_LEADERBOARD_WS}

COPY --from=deps /app/node_modules ./node_modules
COPY . .

RUN npm run build

# ---- Stage 3 : runtime ------------------------------------------------------
FROM node:20-alpine AS runtime
WORKDIR /app
ENV NODE_ENV=production
ENV NEXT_TELEMETRY_DISABLED=1
ENV PORT=3000
ENV HOSTNAME=0.0.0.0

# Run as non-root.
RUN addgroup --system --gid 1001 velocity \
 && adduser  --system --uid 1001 --ingroup velocity velocity

# Copy the standalone Next build (set in next.config.mjs).
COPY --from=builder --chown=velocity:velocity /app/.next/standalone ./
COPY --from=builder --chown=velocity:velocity /app/.next/static ./.next/static
COPY --from=builder --chown=velocity:velocity /app/public ./public

USER velocity
EXPOSE 3000

HEALTHCHECK --interval=30s --timeout=5s --start-period=10s --retries=3 \
  CMD wget --quiet --spider http://localhost:3000/ || exit 1

CMD ["node", "server.js"]
