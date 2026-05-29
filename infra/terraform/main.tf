################################################################################
# main.tf — providers + root data sources.
################################################################################

provider "aws" {
  region = var.region

  default_tags {
    tags = {
      Project     = "velocity"
      Environment = var.environment
      ManagedBy   = "terraform"
      Owner       = "velocity-platform-team"
    }
  }
}

data "aws_caller_identity" "current" {}
data "aws_partition" "current" {}
data "aws_availability_zones" "available" {
  state = "available"
}

locals {
  name            = "velocity-${var.environment}"
  account_id      = data.aws_caller_identity.current.account_id
  azs             = slice(data.aws_availability_zones.available.names, 0, 3)
  cluster_version = var.cluster_version
  service_images = [
    "api-gateway",
    "bot-controller",
    "bot-worker",
    "correctness-validator",
    "leaderboard-ws",
    "scoring-service",
    "submission-engine",
    "telemetry-ingester",
    "frontend",
  ]
}

# The Kubernetes provider gets its credentials from the EKS module's outputs
# so it can wire up the aws-auth ConfigMap and add helm charts (gVisor
# DaemonSet, etc.). We *do not* create resources here that depend on a
# fresh cluster — those live in `kubernetes/` overlays applied after apply.
provider "kubernetes" {
  host                   = module.eks.cluster_endpoint
  cluster_ca_certificate = base64decode(module.eks.cluster_certificate_authority_data)
  exec {
    api_version = "client.authentication.k8s.io/v1beta1"
    command     = "aws"
    args        = ["eks", "get-token", "--cluster-name", module.eks.cluster_name, "--region", var.region]
  }
}

provider "helm" {
  kubernetes {
    host                   = module.eks.cluster_endpoint
    cluster_ca_certificate = base64decode(module.eks.cluster_certificate_authority_data)
    exec {
      api_version = "client.authentication.k8s.io/v1beta1"
      command     = "aws"
      args        = ["eks", "get-token", "--cluster-name", module.eks.cluster_name, "--region", var.region]
    }
  }
}
