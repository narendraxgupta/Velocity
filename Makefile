# =============================================================================
#  Velocity — Developer Workflow Makefile
#
#  Self-documenting: `make` with no arguments prints a categorized help.
#
#  All targets are designed to work on Windows (via WSL2 / Git Bash) and
#  Linux/macOS uniformly. They shell out to `docker compose` for everything
#  Linux-native (io_uring, gVisor); no native C++ build is required on the
#  developer's host.
# =============================================================================

SHELL := bash
.SHELLFLAGS := -eu -o pipefail -c
.DEFAULT_GOAL := help

# Colors (gracefully degrade on terminals that don't support them).
COLOR_BOLD := $(shell tput bold 2>/dev/null || true)
COLOR_DIM  := $(shell tput dim  2>/dev/null || true)
COLOR_OFF  := $(shell tput sgr0 2>/dev/null || true)

COMPOSE  := docker compose
PROFILES_DEFAULT := --profile default
PROFILES_APPS    := --profile apps
PROFILES_DEBUG   := --profile debug

##@ Lifecycle

.PHONY: help
help: ## Show this help.
    @awk 'BEGIN {FS = ":.*##"; printf "$(COLOR_BOLD)Velocity$(COLOR_OFF) — make targets\n\n"} \
        /^[a-zA-Z_0-9-]+:.*?##/ { printf "  $(COLOR_BOLD)%-20s$(COLOR_OFF) %s\n", $$1, $$2 } \
        /^##@/ { printf "\n$(COLOR_BOLD)%s$(COLOR_OFF)\n", substr($$0, 5) }' $(MAKEFILE_LIST)

.PHONY: bootstrap
bootstrap: ## One-time setup: pull base images, generate proto stubs.
    @echo "→ pulling base images"
    $(COMPOSE) pull --ignore-pull-failures
    @$(MAKE) proto
    @echo "$(COLOR_BOLD)✓ bootstrap complete$(COLOR_OFF)"

.PHONY: up
up: ## Start the full dev stack (infra + observability).
    $(COMPOSE) $(PROFILES_DEFAULT) up -d
    @echo "$(COLOR_BOLD)✓ infra ready$(COLOR_OFF)"
    @echo "  → Redpanda Console : http://localhost:8085"
    @echo "  → QuestDB Console  : http://localhost:9000"
    @echo "  → MinIO Console    : http://localhost:9101 (velocity / velocity-dev-secret)"
    @echo "  → Grafana          : http://localhost:3001 (anonymous viewer)"
    @echo "  → Prometheus       : http://localhost:9090"

.PHONY: up-apps
up-apps: ## Bring up infra + application services (requires built images).
    $(COMPOSE) $(PROFILES_DEFAULT) $(PROFILES_APPS) up -d
    @echo "$(COLOR_BOLD)✓ platform fully running$(COLOR_OFF)"
    @echo "  → Frontend         : http://localhost:3000"
    @echo "  → API Gateway      : http://localhost:8080"
    @echo "  → Leaderboard WS   : ws://localhost:8090/v1/leaderboard"

.PHONY: down
down: ## Stop services; keep volumes.
    $(COMPOSE) down

.PHONY: nuke
nuke: ## Stop services AND delete all volumes. Destroys local data.
    $(COMPOSE) down -v
    @echo "$(COLOR_BOLD)✓ all volumes removed$(COLOR_OFF)"

.PHONY: logs
logs: ## Tail logs from every service.
    $(COMPOSE) logs -f --tail=100

.PHONY: ps
ps: ## Show running services.
    $(COMPOSE) ps

##@ Build

.PHONY: build
build: build-cpp-base proto-ensure ## Build every container image (ensures proto stubs + the shared C++ base first).
    $(COMPOSE) $(PROFILES_APPS) build

.PHONY: proto-ensure
proto-ensure: ## Generate proto stubs (and restore the go.mod stub) only when missing.
    @# The submission-engine Docker build COPYs proto/gen/go/go.mod and compiles the
    @# generated .pb.go bindings — both must be present in the build context. go.mod is
    @# a committed stub (older buf `clean:true` configs used to wipe it); the .pb.* files
    @# are gitignored and produced by `make proto`. Self-heal both so a fresh checkout can
    @# `make build` without first running `make bootstrap`.
    @test -f proto/gen/go/go.mod || git checkout -- proto/gen/go/go.mod 2>/dev/null || true
    @if [ ! -f proto/gen/go/go.mod ]; then \
        echo "→ recreating committed proto/gen/go/go.mod stub"; \
        mkdir -p proto/gen/go; \
        printf 'module github.com/velocity/platform/proto/gen/go\n\ngo 1.22\n\nrequire (\n\tgoogle.golang.org/grpc v1.67.1\n\tgoogle.golang.org/protobuf v1.35.1\n)\n' > proto/gen/go/go.mod; \
    fi
    @if [ -n "$$(find proto/gen/go -name '*.pb.go' -print -quit 2>/dev/null)" ]; then \
        echo "$(COLOR_DIM)✓ proto stubs present$(COLOR_OFF)"; \
    else \
        echo "→ proto stubs missing — generating with buf"; \
        $(MAKE) proto; \
    fi

.PHONY: build-cpp-base
build-cpp-base: ## Build the shared C++ builder image used by all C++ services.
    docker build -f infra/compose/Dockerfile.cpp-base --target builder -t velocity/cpp-base:builder .

.PHONY: build-api
build-api: build-cpp-base
    $(COMPOSE) $(PROFILES_APPS) build api-gateway

.PHONY: build-bot
build-bot: build-cpp-base
    $(COMPOSE) $(PROFILES_APPS) build bot-controller bot-worker

.PHONY: build-data
build-data: build-cpp-base
    $(COMPOSE) $(PROFILES_APPS) build telemetry-ingester correctness-validator leaderboard-ws

.PHONY: build-frontend
build-frontend:
    $(COMPOSE) $(PROFILES_APPS) build frontend

##@ Proto

.PHONY: proto
proto: ## Regenerate protobuf stubs for C++, Go, and TypeScript.
    @echo "→ generating C++ / Go / TS stubs from proto/"
    docker run --rm -v "$$PWD/proto:/workspace" -w /workspace \
        bufbuild/buf:1.45.0 generate

##@ Test

.PHONY: test
test: test-cpp test-go test-frontend ## Run all tests.

.PHONY: test-cpp
test-cpp: ## Run C++ unit tests (inside the cpp-base image).
    docker run --rm -v "$$PWD:/app" -w /app velocity/cpp-base:builder \
        bash -c "ctest --preset conan-release --output-on-failure"

.PHONY: test-go
test-go: ## Run Go unit tests.
    cd services/submission-engine && go test ./...

.PHONY: test-frontend
test-frontend: ## Frontend type-check and lint.
    cd frontend && npm run typecheck && npm run lint

##@ Quality

.PHONY: fmt
fmt: ## Format C++, Go, and frontend code.
    @echo "→ clang-format C++"
    @find services scripts cmake -name '*.cpp' -o -name '*.h' -o -name '*.hpp' \
        | xargs -r clang-format -i
    @echo "→ gofmt Go"
    @cd services/submission-engine && go fmt ./...
    @echo "→ prettier frontend"
    @cd frontend && npm run format

.PHONY: lint
lint: ## Lint everything.
    @cd services/submission-engine && go vet ./...
    @cd frontend && npm run lint

##@ Demo

.PHONY: sample-submit
sample-submit: ## Build the bundled sample exchange and push it through the gateway.
    @bash scripts/e2e-smoke.sh

.PHONY: bench
bench: ## Run a baseline benchmark against the most recent submission.
    @if [ -z "$$SUBMISSION_ID" ]; then \
        echo "set SUBMISSION_ID=<id> first (see 'make sample-submit')"; exit 2; \
    fi
    curl -fsS -X POST \
        -H 'Content-Type: application/json' \
        -d '{"submission_id":"'"$$SUBMISSION_ID"'","profile":"baseline"}' \
        http://localhost:8080/v1/benchmarks

##@ Deploy

.PHONY: helm-lint
helm-lint: ## Lint the Helm chart locally (no cluster required).
    helm lint infra/helm/velocity

.PHONY: helm-template
helm-template: ## Render the chart with default values; pipe through `kubectl apply --dry-run=client`.
    helm template demo infra/helm/velocity --debug | kubectl apply --dry-run=client -f -

.PHONY: helm-install
helm-install: ## Install the chart against the current kube context. RELEASE=demo NAMESPACE=velocity-system override.
    helm upgrade --install $${RELEASE:-demo} infra/helm/velocity \
      --create-namespace --namespace $${NAMESPACE:-velocity-system} \
      $${VALUES:+-f $$VALUES}

##@ SDKs

.PHONY: sdk-build
sdk-build: proto ## Build all three SDKs (Go, TS, Python) locally.
    @echo "→ Go SDK (smoke build)"
    cd sdk/go && go build ./...
    @echo "→ TS SDK"
    cd sdk/ts && npm install --no-audit --no-fund && npm run build
    @echo "→ Python SDK"
    cd sdk/python && python -m build

.PHONY: sdk-release
sdk-release: ## Cut a release tag for all SDKs. Usage: make sdk-release VERSION=0.1.2
    @if [ -z "$$VERSION" ]; then echo "set VERSION=<x.y.z>"; exit 2; fi
    git tag "sdk-v$$VERSION"
    @echo "$(COLOR_BOLD)✓ tagged sdk-v$$VERSION$(COLOR_OFF) — push with 'git push origin sdk-v$$VERSION'"
    @echo "  The release-sdk workflow will publish Go (via tag), TS (npm), and Python (PyPI)."

##@ Misc

.PHONY: clean
clean: ## Delete build artefacts but keep node_modules.
    rm -rf build/ build-*/ cmake-build-*/ frontend/.next/

.PHONY: tools
tools: ## Print versions of all the tools we expect.
    @docker --version
    @$(COMPOSE) version
    @node --version 2>/dev/null || echo "node not installed (only needed for local frontend dev)"
