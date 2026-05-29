################################################################################
# variables.tf — root inputs.
#
# Most defaults target a dev-scale cluster (3 small system nodes, 2 sandbox
# nodes). For prod, override via `env/prod.tfvars`.
################################################################################

variable "region" {
  description = "AWS region for all resources."
  type        = string
  default     = "us-east-1"
}

variable "environment" {
  description = "Logical environment name (dev/staging/prod). Suffixed onto resource names."
  type        = string
  validation {
    condition     = contains(["dev", "staging", "prod"], var.environment)
    error_message = "environment must be one of: dev, staging, prod."
  }
}

variable "cluster_version" {
  description = "Kubernetes minor version for the EKS control plane."
  type        = string
  default     = "1.30"
}

variable "vpc_cidr" {
  description = "Primary CIDR for the VPC. /16 gives us plenty of room for sandbox pods."
  type        = string
  default     = "10.40.0.0/16"
}

# ---------------------------------------------------------------- node groups

variable "system_node_instance_types" {
  description = "Instance types for the general-purpose 'system' node group."
  type        = list(string)
  default     = ["m6i.large", "m6i.xlarge"]
}

variable "system_node_min_size" {
  type    = number
  default = 2
}

variable "system_node_desired_size" {
  type    = number
  default = 3
}

variable "system_node_max_size" {
  type    = number
  default = 10
}

variable "sandbox_node_instance_types" {
  description = "Instance types for gVisor-enabled bot worker / submission pods. Compute-optimised."
  type        = list(string)
  default     = ["c6i.2xlarge", "c6i.4xlarge"]
}

variable "sandbox_node_min_size" {
  type    = number
  default = 1
}

variable "sandbox_node_desired_size" {
  type    = number
  default = 2
}

variable "sandbox_node_max_size" {
  type    = number
  default = 50
}

# ---------------------------------------------------------------- storage

variable "artefact_bucket_force_destroy" {
  description = "Allow `terraform destroy` to nuke a non-empty submissions bucket. Set false in prod."
  type        = bool
  default     = true
}

variable "artefact_retention_days" {
  description = "Days after which submission artefacts are auto-expired."
  type        = number
  default     = 30
}

# ---------------------------------------------------------------- access

variable "admin_iam_principals" {
  description = "IAM principal ARNs that get cluster-admin via aws-auth. Add your CI role + a break-glass user."
  type        = list(string)
  default     = []
}
