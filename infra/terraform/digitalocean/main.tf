# =============================================================================
#  DigitalOcean Velocity provisioning — main module.
# =============================================================================

locals {
  prefix = "velocity-${var.environment}"
  tags = [
    "velocity",
    "env:${var.environment}",
    "managed-by:terraform",
  ]
}

data "digitalocean_kubernetes_versions" "doks" {
  version_prefix = var.kubernetes_version
}

# ---------- VPC --------------------------------------------------------------
resource "digitalocean_vpc" "velocity" {
  name        = "${local.prefix}-vpc"
  region      = var.region
  ip_range    = "10.45.0.0/16"
  description = "Velocity ${var.environment} VPC"
}

# ---------- DOKS cluster -----------------------------------------------------
resource "digitalocean_kubernetes_cluster" "velocity" {
  name          = local.prefix
  region        = var.region
  version       = data.digitalocean_kubernetes_versions.doks.latest_version
  vpc_uuid      = digitalocean_vpc.velocity.id
  auto_upgrade  = false
  surge_upgrade = true

  tags = local.tags

  # Default pool — the "system" tier. Static size; the sandbox pool is
  # the auto-scaler.
  node_pool {
    name       = "system"
    size       = var.system_pool_node_size
    node_count = var.system_pool_node_count
    labels = {
      "velocity.tier" = "system"
    }
    tags = concat(local.tags, ["pool:system"])
  }
}

# ---------- Sandbox pool (separate so we can auto-scale it independently)
resource "digitalocean_kubernetes_node_pool" "sandbox" {
  cluster_id = digitalocean_kubernetes_cluster.velocity.id

  name       = "sandbox"
  size       = var.sandbox_pool_node_size
  auto_scale = true
  min_nodes  = var.sandbox_pool_min
  max_nodes  = var.sandbox_pool_max

  labels = {
    "velocity.tier" = "sandbox"
    # Must match the gVisor RuntimeClass nodeSelector
    # (infra/kubernetes/base/runtimeclass-gvisor.yaml: runtime.velocity.io/gvisor).
    # A "...sandbox" label left sandbox pods unschedulable here.
    "runtime.velocity.io/gvisor" = "true"
  }

  taint {
    key    = "velocity.tier"
    value  = "sandbox"
    effect = "NoSchedule"
  }

  tags = concat(local.tags, ["pool:sandbox"])
}

# ---------- Spaces (S3-compatible) artefact bucket --------------------------
resource "digitalocean_spaces_bucket" "artefacts" {
  name   = "${var.spaces_artefact_bucket_name}-${var.environment}"
  region = var.region
  acl    = "private"

  versioning {
    enabled = true
  }

  lifecycle_rule {
    id      = "expire-old-uploads"
    enabled = true
    expiration {
      days = 30
    }
  }
}

# ---------- Container registry ----------------------------------------------
resource "digitalocean_container_registry" "velocity" {
  name                   = "${local.prefix}-cr"
  region                 = var.region
  subscription_tier_slug = "basic"
}
