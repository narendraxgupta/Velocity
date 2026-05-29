# Terraform — DigitalOcean (DOKS)

> A DigitalOcean-flavoured Velocity cluster, mirroring the AWS module
> in `../`. Use this when you want a quick managed-Kubernetes target
> without the IAM/VPC complexity of AWS — DOKS gets you a usable
> cluster in ~7 minutes.

## What this provisions

- **VPC** (one per environment).
- **DOKS cluster** with two node pools:
  - `system` (general-purpose `s-4vcpu-8gb`) for the control plane.
  - `sandbox` (CPU-optimised `c2-8vcpu-16gb`) for bot-worker and
    untrusted submission pods. The pool is auto-scaling.
- **Spaces bucket** for submission artefacts (S3-compatible — point
  Velocity's MinIO config at it).
- **Container registry** (one repository, immutable tags).
- **Optional Postgres** for an external QuestDB swap (not currently
  used by Velocity but reserved for future Phase 4 work).

## Usage

```bash
cd infra/terraform/digitalocean/

# One-time per DO project.
./bootstrap.sh

# Per env.
terraform init \
  -backend-config="endpoints=https://nyc3.digitaloceanspaces.com" \
  -backend-config="bucket=velocity-tfstate" \
  -backend-config="key=dev.tfstate" \
  -backend-config="region=us-east-1" \
  -backend-config="access_key=$DO_SPACES_ACCESS_KEY" \
  -backend-config="secret_key=$DO_SPACES_SECRET_KEY"

terraform plan  -var-file=env/dev.tfvars
terraform apply -var-file=env/dev.tfvars

# Wire kubectl + apply manifests.
doctl kubernetes cluster kubeconfig save velocity-dev
helm install velocity ./infra/helm/velocity -f env/dev.values.yaml
```

## Why DigitalOcean, given we have AWS already?

Pricing: a 3-node `s-4vcpu-8gb` DOKS cluster costs ~$120/mo all-in,
including the load balancer. The equivalent EKS cluster costs ~$150/mo
control-plane PLUS NAT-gateway egress. For IICPC and small prod
deployments this is the right shape.
