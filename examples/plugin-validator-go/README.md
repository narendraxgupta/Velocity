# Example plugin: STP-Strict (Go)

A reference implementation of a Velocity validator plugin that enforces
"strict self-trade prevention" — every fill where the bid and offer
share an account-tag is flagged as a violation.

Use this as a template for your own plugins.

## Run locally

```bash
go build ./...
./plugin-validator-go --grpc :50061
```

## Package and register

```bash
docker build -t ghcr.io/your-org/velocity-plugin-stp:0.1 .
docker push   ghcr.io/your-org/velocity-plugin-stp:0.1
```

Register it in your tenant's manifest:

```yaml
apiVersion: velocity/v1
kind: PluginRegistry
tenant: your-tenant
plugins:
  - id:      your-org/stp-strict/0.1
    image:   ghcr.io/your-org/velocity-plugin-stp:0.1
    enabled: true
    events:
      - FILL
    cpu: "200m"
    memory: "128Mi"
```

Hand the file to the plugin-orchestrator:

```bash
curl -X POST -H "Authorization: Bearer $VELOCITY_ADMIN_JWT" \
     --data-binary @plugins.yaml \
     https://demo.velocityhq.io/v1/plugins/your-tenant/reconcile
```

The next benchmark run for any submission in `your-tenant` will fan
out fills to your plugin alongside the first-party validators.

## Contract

Plugins implement [`velocity.plugin.v1.PluginValidator`](../../proto/plugin.proto).
Three RPCs:

- `GetInfo`   — returns metadata; called once at startup + on reconnect.
- `Validate`  — called per-batch; return zero or more Violations.
- `Heartbeat` — orchestrator polls every 30s.

Plugins MUST be idempotent: the orchestrator may retry calls on
transient failures.
