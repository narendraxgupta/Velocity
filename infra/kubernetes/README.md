# Kubernetes Manifests

Velocity's production target is Kubernetes. These manifests describe the
cluster shape — namespaces, RuntimeClass for gVisor, NetworkPolicies for
the sandbox isolation perimeter, Deployments for the C++/Go services, and
StatefulSets for the data plane (Redpanda, QuestDB, Redis, MinIO).

## Layout (Kustomize)

```
infra/kubernetes/
├── base/                      Shared resources
│   ├── kustomization.yaml
│   ├── namespaces.yaml
│   ├── runtimeclass-gvisor.yaml
│   ├── networkpolicy-sandbox.yaml
│   ├── secrets.yaml
│   ├── servicemonitors.yaml
│   ├── deployments/           Control-plane and stream-processing services
│   └── statefulsets/          Redpanda, QuestDB, Redis, MinIO
└── overlays/
    ├── dev/                   k3d local cluster
    │   └── kustomization.yaml
    └── prod/                  EKS / GKE
        └── kustomization.yaml
```

## Apply locally with k3d

```bash
# Create the local cluster (one time):
k3d cluster create velocity \
    --servers 1 --agents 2 \
    --port "8080:80@loadbalancer" \
    --port "9000:9000@loadbalancer"

# Install gVisor on each node (DaemonSet that drops `runsc` into the
# containerd runtime config — see infra/kubernetes/base/runtimeclass-gvisor.yaml).

# Apply the dev overlay:
kubectl apply -k overlays/dev
```

## Apply to a managed cluster

The `prod` overlay assumes:

- A managed control plane (EKS, GKE Autopilot, or AKS).
- Node pools with `containerd` and the gVisor handler installed.
- A storage class capable of provisioning 100 GiB+ SSDs for QuestDB.

```bash
# Provision the cluster with Terraform first (see infra/terraform/), then:
kubectl apply -k overlays/prod
```

## Conventions

- Every service Deployment gets a `PriorityClass`: bots > ingester > frontend,
  so we shed UI updates before we shed measurements.
- Every sandbox Pod is created in `velocity-sandbox` with
  `runtimeClassName: gvisor`. The base `NetworkPolicy` denies all egress
  from that namespace and admits ingress only from `velocity-load`.
- Pod Security Standards: `baseline` cluster-wide, `restricted` for
  sandbox pods.
