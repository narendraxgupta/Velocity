# =============================================================================
#  Variables for the DigitalOcean Velocity module.
#
#  Defaults target a "demo-scale" cluster suitable for the IICPC and
#  small post-IICPC pilots. Override in env/<name>.tfvars for prod.
# =============================================================================

variable "do_token" {
  description = "DigitalOcean API token. Best supplied via TF_VAR_do_token or the DO_TOKEN env."
  type        = string
  sensitive   = true
}

variable "environment" {
  description = "Logical environment name (dev / staging / prod). Suffixed onto resource names."
  type        = string
  validation {
    condition     = contains(["dev", "staging", "prod"], var.environment)
    error_message = "environment must be one of: dev, staging, prod."
  }
}

variable "region" {
  description = "DigitalOcean region slug."
  type        = string
  default     = "nyc3"
}

variable "kubernetes_version" {
  description = "DOKS version prefix (e.g. 1.30 → latest 1.30.x)."
  type        = string
  default     = "1.30"
}

# --- node pools --------------------------------------------------------------

variable "system_pool_node_size" {
  description = "Droplet slug for the general-purpose pool. s-4vcpu-8gb is sufficient for dev."
  type        = string
  default     = "s-4vcpu-8gb"
}

variable "system_pool_node_count" {
  type    = number
  default = 3
}

variable "sandbox_pool_node_size" {
  description = "CPU-optimised slug for bot-worker / submission pods."
  type        = string
  default     = "c-8"
}

variable "sandbox_pool_min" {
  type    = number
  default = 1
}

variable "sandbox_pool_max" {
  type    = number
  default = 10
}

# --- storage -----------------------------------------------------------------

variable "spaces_artefact_bucket_name" {
  description = "Spaces bucket for submission artefacts. Auto-suffixed with environment."
  type        = string
  default     = "velocity-artefacts"
}

variable "spaces_access_key" {
  type        = string
  sensitive   = true
  description = "Spaces access key. Used to initialise the bucket policy."
}

variable "spaces_secret_key" {
  type      = string
  sensitive = true
}
