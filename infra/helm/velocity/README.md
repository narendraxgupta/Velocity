# Velocity Helm Chart

```bash
helm repo add velocity https://velocity-platform.github.io/charts
helm install demo velocity/velocity \
     --create-namespace --namespace velocity-system \
     -f my-overrides.yaml
```

## What this chart deploys

The full Velocity platform: api-gateway, submission-engine, bot-controller,
bot-worker, telemetry-ingester, correctness-validator, scoring-service,
leaderboard-ws, frontend, and the auxiliary services (chaos-orchestrator,
pcap-recorder/replayer, marketdata-generator, critique-service,
anomaly-detector, audit-log, plugin-orchestrator).

The data plane (Redis, Redpanda, QuestDB, MinIO) ships in `mode: internal`
single-replica StatefulSets — fine for evaluation; switch each to
`mode: external` for a real deployment.

## Common overrides

### Prod with external data plane

```yaml
global:
  registry: ghcr.io/your-org
  tag: v0.5.0

apiGateway:
  requireAuth: true
  jwtSecretBase64: "yourBase64UrlSecretHere"

redis:
  mode: external
  external:
    addr: "velocity.cache.use1.cache.amazonaws.com:6379"

redpanda:
  mode: external
  external:
    brokers: "b-1.velocity.x.kafka.use1.amazonaws.com:9094"

questdb:
  mode: external
  external:
    pg:  "postgres://admin@questdb.internal:8812/qdb"
    ilp: "questdb.internal:9009"

minio:
  mode: external
  external:
    endpoint:  "s3.amazonaws.com"
    accessKey: "AKIA..."
    secretKey: "..."

ingress:
  enabled: true
  className: alb
  host: velocity.your-org.com
  tls: { enabled: true, secretName: velocity-tls }
```

### Minimal demo (Compose-like dev cluster)

> **Heads-up:** this chart is still being brought to parity with the Kustomize
> base. Its generic service template currently injects only `REDIS_ADDR` /
> `KAFKA_BROKERS`, whereas the C++/Go services read `VELOCITY_*` env vars (and
> the submission-engine needs an in-cluster registry), so a bare `helm install`
> will not yet bring every service up cleanly. For a known-good local deploy,
> use the Kustomize dev overlay (`infra/kubernetes/overlays/dev`) or Docker
> Compose (`make up-apps`). Helm chart parity is tracked as a follow-up.

```bash
helm install demo ./infra/helm/velocity --create-namespace -n velocity-system
kubectl -n demo-control port-forward svc/frontend 3000:3000
kubectl -n demo-control port-forward svc/api-gateway 8080:8080
```

## Provisioning the cluster

For a turnkey provider-managed cluster, use the matching Terraform
module under `infra/terraform/` (AWS EKS today, DigitalOcean and GCP
variants under the `digitalocean/` and `gcp/` subdirectories).
