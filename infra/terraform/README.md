# Terraform — Cloud Provisioning

> Infrastructure-as-code for Velocity's AWS production target. Pairs with
> the Kubernetes manifests in [`../kubernetes/`](../kubernetes/).

## What this provisions

- **VPC** — three AZs, three subnet tiers: `private` (NAT-routed) for the
  control-plane **and** sandbox node groups, `public` for NAT + load balancers,
  and a reserved NAT-free `intra` tier. (Sandbox **nodes** sit in `private` so
  they can bootstrap and pull from ECR; sandbox **pod** egress is blocked at the
  NetworkPolicy layer, not by subnet — see Notes.)
- **EKS cluster** with KMS-encrypted secrets, IRSA, and control-plane logs
  shipped to CloudWatch.
- Two **managed node groups**:
  - `system` — general-purpose (`m6i.large/xlarge`) running api-gateway,
    bot-controller, telemetry-ingester, etc.
  - `sandbox` — compute-optimised (`c6i.{2,4,8}xlarge`) with `runsc`
    (gVisor) installed at boot and labelled `runtime.velocity.io/gvisor=true`.
    Tainted `NO_SCHEDULE` so only the gVisor `RuntimeClass` lands here.
- **ECR repos** (one per service image, immutable, scan-on-push, 30-tag
  retention).
- **S3 bucket** for submission artefacts (the production replacement for
  MinIO), versioned, encrypted, public-access blocked.
- **IAM/IRSA role** for the submission-engine ServiceAccount giving it
  least-privileged S3 + ECR push.

## Layout

```
infra/terraform/
├── versions.tf        # provider + Terraform version pins
├── backend.tf         # S3 + DynamoDB remote state
├── main.tf            # providers + locals + data sources
├── variables.tf       # root inputs
├── vpc.tf             # VPC, subnets, NAT, flow logs
├── eks.tf             # EKS control plane + add-ons + IRSA OIDC
├── node-groups.tf     # system + sandbox managed node groups
├── ecr.tf             # one repo per service image
├── s3.tf              # submission artefact bucket
├── iam.tf             # IRSA roles for submission-engine
├── outputs.tf
├── bootstrap.sh       # creates the tfstate bucket + DDB lock table
└── env/
    ├── dev.tfvars
    ├── dev.backend.tfvars
    ├── prod.tfvars
    └── prod.backend.tfvars
```

## Usage

```bash
cd infra/terraform/

# One-time, per AWS account.
./bootstrap.sh us-east-1

# Per env.
terraform init   -backend-config=env/dev.backend.tfvars
terraform plan   -var-file=env/dev.tfvars
terraform apply  -var-file=env/dev.tfvars

# Wire kubectl + apply manifests.
aws eks update-kubeconfig --name velocity-dev --region us-east-1
kubectl apply -k ../kubernetes/overlays/prod
```

## Module versions

| Module | Version | Why |
|---|---|---|
| `terraform-aws-modules/vpc` | `~> 5.13` | Stable post-`enable_flow_log` rename. |
| `terraform-aws-modules/eks` | `~> 20.20` | Adds first-class `access_entries`. |
| `terraform-aws-modules/iam` (IRSA submodule) | `~> 5.44` | Latest OIDC provider URL handling. |

## Notes

- The sandbox node bootstrap downloads `runsc` directly from
  `storage.googleapis.com/gvisor/releases` rather than building it from
  source. For an air-gapped deployment, mirror the tarball into your
  internal artifact store and override `local.sandbox_bootstrap_userdata`.
- Sandbox **nodes** run in the NAT-routed `private` subnets (so the node
  bootstrap can fetch `runsc` and pull images from ECR). Untrusted submission
  **pods** are cut off from the public internet not by the subnet but by the
  default-deny NetworkPolicy in the `velocity-sandbox` namespace
  (`infra/kubernetes/base/networkpolicy-sandbox.yaml`); they can still reach
  in-cluster services (Redis, Redpanda, validators). This is intentional.
- For prod we recommend toggling `cluster_endpoint_public_access` off and
  reaching the API server only through the VPN / Tailscale / Session Mgr.
