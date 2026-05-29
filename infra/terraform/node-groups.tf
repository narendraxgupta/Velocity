################################################################################
# node-groups.tf — `system` + `sandbox` managed node groups.
#
# `system` runs the platform's own services (api-gateway, controller, …).
# `sandbox` runs gVisor-isolated workloads: submission matching engines and
#   adversarial bot workers. The `runtime.velocity.io/gvisor=true` label
#   means our gVisor `RuntimeClass` will only schedule onto these nodes.
#
# We install `runsc` (gVisor) on the sandbox nodes via a userdata script.
# The script is intentionally small — it grabs the official tarball, drops
# `runsc` into /usr/local/bin, and wires up containerd to know about it.
################################################################################

locals {
  # Bootstrap script for sandbox nodes. Runs at first boot via cloud-init.
  # We use the AL2023 EKS-optimised AMI which already has containerd
  # configured; we only need to add a runtime entry.
  sandbox_bootstrap_userdata = <<-EOT
    MIME-Version: 1.0
    Content-Type: multipart/mixed; boundary="//"

    --//
    Content-Type: text/x-shellscript; charset="us-ascii"

    #!/bin/bash
    set -euo pipefail

    # ---- Install gVisor (runsc) ---------------------------------------------
    GVISOR_VER=$(curl -fsSL https://storage.googleapis.com/gvisor/releases/release/latest)
    ARCH=$(uname -m)
    URL_BASE="https://storage.googleapis.com/gvisor/releases/release/$${GVISOR_VER}/$${ARCH}"
    curl -fsSL "$${URL_BASE}/runsc" -o /usr/local/bin/runsc
    curl -fsSL "$${URL_BASE}/containerd-shim-runsc-v1" -o /usr/local/bin/containerd-shim-runsc-v1
    chmod +x /usr/local/bin/runsc /usr/local/bin/containerd-shim-runsc-v1

    # ---- Register runsc with containerd -------------------------------------
    mkdir -p /etc/containerd
    cat >> /etc/containerd/config.toml <<'CFG'
    [plugins."io.containerd.grpc.v1.cri".containerd.runtimes.runsc]
      runtime_type = "io.containerd.runsc.v1"
    [plugins."io.containerd.grpc.v1.cri".containerd.runtimes.runsc.options]
      TypeUrl = "io.containerd.runsc.v1.options"
      ConfigPath = "/etc/containerd/runsc.toml"
    CFG

    cat > /etc/containerd/runsc.toml <<'RSC'
    log_path = "/var/log/runsc-%ID%.log"
    log_format = "json"
    platform = "kvm"
    net-raw = false
    RSC

    systemctl restart containerd

    --//--
  EOT
}

module "system_node_group" {
  source  = "terraform-aws-modules/eks/aws//modules/eks-managed-node-group"
  version = "~> 20.20"

  name            = "${local.name}-system"
  cluster_name    = module.eks.cluster_name
  cluster_version = module.eks.cluster_version

  subnet_ids = module.vpc.private_subnets

  instance_types = var.system_node_instance_types
  capacity_type  = "ON_DEMAND"

  min_size     = var.system_node_min_size
  desired_size = var.system_node_desired_size
  max_size     = var.system_node_max_size

  labels = {
    "node.velocity.io/role" = "system"
  }

  # The system pool intentionally does not taint — only sandbox pods need
  # to be steered onto the sandbox pool (and they are, via RuntimeClass).
  taints = []

  cluster_service_cidr              = module.eks.cluster_service_cidr
  cluster_primary_security_group_id = module.eks.cluster_primary_security_group_id
  vpc_security_group_ids            = [module.eks.node_security_group_id]
}

module "sandbox_node_group" {
  source  = "terraform-aws-modules/eks/aws//modules/eks-managed-node-group"
  version = "~> 20.20"

  name            = "${local.name}-sandbox"
  cluster_name    = module.eks.cluster_name
  cluster_version = module.eks.cluster_version

  subnet_ids = module.vpc.private_subnets

  instance_types = var.sandbox_node_instance_types
  capacity_type  = "ON_DEMAND"

  min_size     = var.sandbox_node_min_size
  desired_size = var.sandbox_node_desired_size
  max_size     = var.sandbox_node_max_size

  labels = {
    "node.velocity.io/role"      = "sandbox"
    "runtime.velocity.io/gvisor" = "true"
  }

  taints = [{
    key    = "runtime.velocity.io/gvisor"
    value  = "true"
    effect = "NO_SCHEDULE"
  }]

  # Run our bootstrap to install runsc + register with containerd.
  enable_bootstrap_user_data = true
  pre_bootstrap_user_data    = local.sandbox_bootstrap_userdata

  # Larger root volume — Kaniko builds + ephemeral state need it.
  block_device_mappings = {
    xvda = {
      device_name = "/dev/xvda"
      ebs = {
        volume_size           = 100
        volume_type           = "gp3"
        iops                  = 3000
        throughput            = 125
        encrypted             = true
        delete_on_termination = true
      }
    }
  }

  cluster_service_cidr              = module.eks.cluster_service_cidr
  cluster_primary_security_group_id = module.eks.cluster_primary_security_group_id
  vpc_security_group_ids            = [module.eks.node_security_group_id]
}
