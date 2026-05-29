# =============================================================================
#  GCP Velocity provisioning — GKE Standard with gVisor sandbox pool.
# =============================================================================

locals {
  prefix = "velocity-${var.environment}"
}

# ---------- networking -------------------------------------------------------
resource "google_compute_network" "velocity" {
  name                    = "${local.prefix}-vpc"
  auto_create_subnetworks = false
}

resource "google_compute_subnetwork" "velocity" {
  name                     = "${local.prefix}-subnet"
  ip_cidr_range            = "10.46.0.0/20"
  region                   = var.region
  network                  = google_compute_network.velocity.id
  private_ip_google_access = true

  secondary_ip_range {
    range_name    = "pods"
    ip_cidr_range = "10.50.0.0/16"
  }
  secondary_ip_range {
    range_name    = "services"
    ip_cidr_range = "10.60.0.0/20"
  }
}

# ---------- GKE cluster (private control plane) -----------------------------
resource "google_container_cluster" "velocity" {
  name       = local.prefix
  location   = var.region
  network    = google_compute_network.velocity.id
  subnetwork = google_compute_subnetwork.velocity.id

  # Pin the GKE minor version; the REGULAR release channel below still
  # selects the latest patch within it. Without this the var.cluster_version
  # knob is silently ignored and the cluster floats to the channel default.
  min_master_version = var.cluster_version

  # We delete the default pool and add named pools below so we can
  # configure them independently.
  remove_default_node_pool = true
  initial_node_count       = 1

  release_channel {
    # REGULAR == steady upgrades; pin to RAPID for prod if you want
    # the newest fixes faster.
    channel = "REGULAR"
  }

  workload_identity_config {
    workload_pool = "${var.project_id}.svc.id.goog"
  }

  ip_allocation_policy {
    cluster_secondary_range_name  = "pods"
    services_secondary_range_name = "services"
  }

  private_cluster_config {
    enable_private_nodes    = true
    enable_private_endpoint = false # flip true once your bastion / IAP is wired
    master_ipv4_cidr_block  = "172.16.0.0/28"
  }

  # Required so the sandbox pool below can opt in to gVisor.
  enable_shielded_nodes = true
}

# ---------- system pool ------------------------------------------------------
resource "google_container_node_pool" "system" {
  name       = "system"
  cluster    = google_container_cluster.velocity.id
  node_count = var.system_pool_min

  autoscaling {
    min_node_count = var.system_pool_min
    max_node_count = var.system_pool_max
  }

  node_config {
    machine_type = var.system_pool_machine
    oauth_scopes = ["https://www.googleapis.com/auth/cloud-platform"]
    labels = {
      "velocity.tier" = "system"
    }
    workload_metadata_config {
      mode = "GKE_METADATA"
    }
  }
}

# ---------- sandbox pool (gVisor) -------------------------------------------
# Uses google-beta: the `sandbox_config { sandbox_type = "gvisor" }` block
# below only exists in the beta provider schema.
resource "google_container_node_pool" "sandbox" {
  provider = google-beta
  name     = "sandbox"
  cluster  = google_container_cluster.velocity.id

  autoscaling {
    min_node_count = var.sandbox_pool_min
    max_node_count = var.sandbox_pool_max
  }

  node_config {
    machine_type = var.sandbox_pool_machine
    oauth_scopes = ["https://www.googleapis.com/auth/cloud-platform"]
    labels = {
      "velocity.tier"               = "sandbox"
      "runtime.velocity.io/sandbox" = "true"
    }
    taint {
      key    = "velocity.tier"
      value  = "sandbox"
      effect = "NO_SCHEDULE"
    }
    sandbox_config {
      sandbox_type = "gvisor"
    }
    workload_metadata_config {
      mode = "GKE_METADATA"
    }
  }
}

# ---------- artefact bucket --------------------------------------------------
resource "google_storage_bucket" "artefacts" {
  name                        = "${local.prefix}-artefacts-${var.project_id}"
  location                    = var.region
  force_destroy               = var.artefact_bucket_force_destroy
  uniform_bucket_level_access = true

  versioning { enabled = true }

  lifecycle_rule {
    condition { age = 30 }
    action { type = "Delete" }
  }
}

# ---------- artifact registry ------------------------------------------------
resource "google_artifact_registry_repository" "velocity" {
  location      = var.region
  repository_id = local.prefix
  format        = "DOCKER"
  description   = "Velocity container images for ${var.environment}."
}
