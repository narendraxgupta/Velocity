#!/usr/bin/env bash
# =============================================================================
#  Velocity — Live Error Tailer
#
#  Follows logs from every velocity-* container at once and highlights only
#  the lines that look like problems (error/fatal/panic/exception/refused...).
#  Run this in a second terminal while you exercise the UI — anything that
#  breaks shows up immediately, prefixed with the service name.
#
#  Usage:
#    bash scripts/tail-errors.sh             # all services, errors only
#    bash scripts/tail-errors.sh --all       # show ALL lines (no filter)
#    bash scripts/tail-errors.sh api-gateway frontend   # only these services
# =============================================================================

set -uo pipefail

FILTER=1
SERVICES_ARG=()
for a in "$@"; do
  case "$a" in
    --all) FILTER=0 ;;
    *) SERVICES_ARG+=("$a") ;;
  esac
done

if [[ -t 1 ]]; then R=$'\e[31m'; Y=$'\e[33m'; X=$'\e[0m'; else R=""; Y=""; X=""; fi

# Build the list of running velocity containers (or the user-named subset).
if [[ ${#SERVICES_ARG[@]} -gt 0 ]]; then
  NAMES=()
  for s in "${SERVICES_ARG[@]}"; do
    [[ "$s" == velocity-* ]] && NAMES+=("$s") || NAMES+=("velocity-$s")
  done
else
  mapfile -t NAMES < <(docker ps --format '{{.Names}}' | grep '^velocity-' || true)
fi

if [[ ${#NAMES[@]} -eq 0 ]]; then
  echo "No running velocity-* containers found. Start the stack first (make up-apps)." >&2
  exit 1
fi

echo "Tailing: ${NAMES[*]}"
echo "Filter:  $([[ $FILTER -eq 1 ]] && echo 'errors only' || echo 'ALL lines')   (Ctrl+C to stop)"
echo "---------------------------------------------------------------------------"

ERR_RE='(ERROR|FATAL|PANIC|panic:|Exception|Traceback|segfault|terminate called|address already in use|connection refused|no such host|OOMKilled|level=error|"level":"error"|HTTP 5[0-9][0-9]| 5[0-9][0-9] )'
IGNORE_RE='(errors=0|errored=0|error_count=0|GET /healthz|/readyz|errorRate=0)'

# Merge every container's logs into one stream, each line prefixed with the
# service name. Backgrounded children inherit this function's stdout, which is
# the left side of the pipe below.
merged() {
  for name in "${NAMES[@]}"; do
    docker logs -f --tail 5 "$name" 2>&1 | sed -u "s/^/[$name] /" &
  done
  wait
}

if [[ $FILTER -eq 1 ]]; then
  merged \
    | grep --line-buffered -aE "$ERR_RE" \
    | grep --line-buffered -avE "$IGNORE_RE" \
    | sed -u "s/.*/${R}&${X}/"
else
  merged
fi
