# Terraform — Google Cloud (GKE)

> GKE Autopilot or Standard variant of the Velocity cluster. We default
> to Standard with two node pools because Autopilot's hard pod-spec
> restrictions (no privileged, no hostPath) conflict with bot-worker's
> eBPF needs.

## What this provisions

- **VPC** + subnets (private GKE)
- **GKE Standard** cluster with two node pools:
  - `system` — `e2-standard-4`, ≥3 nodes for the control-plane services.
  - `sandbox` — `c3-standard-8`, auto-scaling, tainted for bot-worker
    and submission pods. CPU-optimised + nested-virt off (no Firecracker
    on GKE; we use gVisor exclusively).
- **GCS bucket** for submission artefacts (the production swap for MinIO).
- **Artifact Registry** repository.

## Usage

```bash
cd infra/terraform/gcp/

terraform init \
  -backend-config="bucket=velocity-tfstate-${PROJECT_ID}" \
  -backend-config="prefix=dev"
terraform plan  -var-file=env/dev.tfvars -var="project_id=$PROJECT_ID"
terraform apply -var-file=env/dev.tfvars -var="project_id=$PROJECT_ID"

gcloud container clusters get-credentials velocity-dev --region us-central1
# Deploy with the Kustomize overlay (recommended today) or Helm with your own
# overrides file (see infra/helm/velocity/README.md — there is no bundled
# env/dev.values.yaml):
kubectl apply -k ../../kubernetes/overlays/prod
```

## Notes

- Workload Identity is enabled; we don't currently use it (the
  submission-engine talks to GCS via static credentials in dev), but
  it's wired so a production migration is a values change, not a code
  change.
- gVisor is enabled via the `sandbox-config { sandbox_type = "gvisor" }`
  on the sandbox pool. Bot-worker manifests already request
  `runtimeClassName: gvisor`.
