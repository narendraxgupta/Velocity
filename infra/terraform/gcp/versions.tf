terraform {
  required_version = ">= 1.7.0"
  required_providers {
    google = {
      source  = "hashicorp/google"
      version = "~> 6.7"
    }
    # GKE Sandbox (gVisor) `sandbox_config` is a beta-only feature; the GA
    # `hashicorp/google` provider has no such block. The sandbox node pool
    # below is created with this provider so untrusted submissions actually
    # run under gVisor.
    google-beta = {
      source  = "hashicorp/google-beta"
      version = "~> 6.7"
    }
  }
}

provider "google" {
  project = var.project_id
  region  = var.region
}

provider "google-beta" {
  project = var.project_id
  region  = var.region
}
