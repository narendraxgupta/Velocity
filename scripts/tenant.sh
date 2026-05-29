#!/usr/bin/env bash
# =============================================================================
#  tenant.sh — CLI wrapper around infra/kubernetes/base/tenants/tenant-template.yaml
#
#  Provisions or tears down a Velocity tenant: namespace + ResourceQuota +
#  LimitRange + NetworkPolicy + (optionally) a minted HS256 JWT the user
#  can hand to the gateway.
#
#  Usage:
#
#    scripts/tenant.sh create acme [--cpu 16] [--mem 32Gi] [--pods 64] \
#                                  [--mint-jwt --role submitter]
#    scripts/tenant.sh delete acme
#    scripts/tenant.sh list
#    scripts/tenant.sh jwt acme --role submitter [--ttl 86400]
#
#  Tenant ID rule: [a-z][a-z0-9-]{1,30}[a-z0-9] — matches Kubernetes
#  label values AND our Redis/Kafka/MinIO key prefix conventions.
# =============================================================================
set -euo pipefail

TEMPLATE_PATH="${TEMPLATE_PATH:-$(dirname "$0")/../infra/kubernetes/base/tenants/tenant-template.yaml}"
JWT_SECRET_B64="${VELOCITY_JWT_HS256_SECRET_B64:-}"

step() { printf "\n\033[1;36m==>\033[0m %s\n" "$*" >&2; }
fail() { printf "\n\033[1;31mFAIL:\033[0m %s\n" "$*" >&2; exit 1; }

validate_tid() {
    [[ "$1" =~ ^[a-z][a-z0-9-]{1,30}[a-z0-9]$ ]] || \
        fail "invalid tenant id '$1' (must match [a-z][a-z0-9-]{1,30}[a-z0-9])"
}

cmd_create() {
    local tid="$1"; shift
    validate_tid "$tid"

    local cpu="8" mem="16Gi" pods="32" mint_jwt=0 role="submitter"
    while [[ $# -gt 0 ]]; do
        case "$1" in
            --cpu)      cpu="$2"; shift 2 ;;
            --mem)      mem="$2"; shift 2 ;;
            --pods)     pods="$2"; shift 2 ;;
            --mint-jwt) mint_jwt=1; shift ;;
            --role)     role="$2"; shift 2 ;;
            *)          fail "unknown flag: $1" ;;
        esac
    done

    step "rendering tenant manifest for '$tid' (cpu=$cpu mem=$mem pods=$pods)"
    sed -e "s/__TID__/$tid/g" \
        -e "s/__CPU__/$cpu/g" \
        -e "s|__MEM__|$mem|g" \
        -e "s/__PODS__/$pods/g" \
        "$TEMPLATE_PATH" | kubectl apply -f -

    if [[ "$mint_jwt" == "1" ]]; then
        cmd_jwt "$tid" --role "$role"
    fi
}

cmd_delete() {
    local tid="$1"; shift || true
    validate_tid "$tid"
    step "deleting tenant '$tid' (namespace velocity-tenant-$tid)"
    kubectl delete namespace "velocity-tenant-$tid" --wait=true
}

cmd_list() {
    kubectl get namespaces -l velocity.tier=tenant \
        -o custom-columns='TENANT:.metadata.labels.velocity\.tenant,STATUS:.status.phase,CREATED:.metadata.creationTimestamp'
}

# Mint an HS256 JWT for a tenant. Pure bash + openssl so it works in CI
# without pulling node/python. NOT for production — production should
# mint tokens from a proper IdP. This is the demo / IICPC path.
cmd_jwt() {
    local tid="$1"; shift
    validate_tid "$tid"

    local role="submitter" ttl="86400"
    while [[ $# -gt 0 ]]; do
        case "$1" in
            --role) role="$2"; shift 2 ;;
            --ttl)  ttl="$2";  shift 2 ;;
            *)      fail "unknown flag: $1" ;;
        esac
    done

    [[ -n "$JWT_SECRET_B64" ]] || \
        fail "VELOCITY_JWT_HS256_SECRET_B64 not set"

    local now exp
    now=$(date +%s)
    exp=$((now + ttl))

    local header_b64 payload_b64
    header_b64=$(printf '{"alg":"HS256","typ":"JWT"}' | b64url)
    payload_b64=$(printf '{"tid":"%s","sub":"%s","role":"%s","iat":%d,"exp":%d}' \
                  "$tid" "tenant-$tid-bootstrap" "$role" "$now" "$exp" | b64url)

    local signing_input="$header_b64.$payload_b64"
    local key_hex sig_b64
    # Decode base64url → hex. We must (a) normalise base64url ('-_') to
    # standard base64 ('+/') and right-pad with '=' before piping through
    # `base64 -d`, and (b) hand the key to openssl as hex via -macopt
    # hexkey:. Going through a shell variable as raw bytes silently
    # truncates the key at the first NUL (the argv boundary is
    # null-terminated), which produces signatures the gateway rejects as
    # forged whenever the secret happens to contain a 0x00 byte.
    local b64_std padded
    b64_std=$(printf '%s' "$JWT_SECRET_B64" | tr '_-' '/+')
    case $(( ${#b64_std} % 4 )) in
        0) padded="$b64_std" ;;
        2) padded="${b64_std}==" ;;
        3) padded="${b64_std}=" ;;
        *) fail "VELOCITY_JWT_HS256_SECRET_B64 is not valid base64url" ;;
    esac
    key_hex=$(printf '%s' "$padded" | base64 -d 2>/dev/null \
              | od -An -vtx1 | tr -d ' \n')
    [[ -n "$key_hex" ]] || \
        fail "VELOCITY_JWT_HS256_SECRET_B64 is not valid base64url"

    sig_b64=$(printf '%s' "$signing_input" | \
              openssl dgst -binary -sha256 \
                  -mac HMAC -macopt "hexkey:$key_hex" | b64url)

    printf '%s.%s\n' "$signing_input" "$sig_b64"
}

b64url() {
    # base64url without padding — same RFC 4648 §5 the gateway expects.
    # Avoid GNU-only `base64 -w0` so macOS / BSD hosts produce valid JWTs.
    base64 | tr -d '\n' | tr '+/' '-_' | tr -d '='
}

main() {
    local sub="${1:-help}"; shift || true
    case "$sub" in
        create) cmd_create "$@" ;;
        delete) cmd_delete "$@" ;;
        list)   cmd_list   "$@" ;;
        jwt)    cmd_jwt    "$@" ;;
        help|*)
            cat <<'USAGE' >&2
tenant.sh — manage Velocity tenants

  create <tid> [--cpu N] [--mem N] [--pods N] [--mint-jwt --role R]
  delete <tid>
  list
  jwt    <tid> [--role R] [--ttl SECS]

Env:
  VELOCITY_JWT_HS256_SECRET_B64   shared secret (base64url) for HS256 JWTs
USAGE
            ;;
    esac
}

main "$@"
