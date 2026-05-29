variable "project_id" {
  description = "GCP project id."
  type        = string
}

variable "environment" {
  type = string
  validation {
    condition     = contains(["dev", "staging", "prod"], var.environment)
    error_message = "environment must be one of: dev, staging, prod."
  }
}

variable "region" {
  type    = string
  default = "us-central1"
}

variable "cluster_version" {
  description = "GKE minor — actual patch picked by 'latest in channel'."
  type        = string
  default     = "1.30"
}

variable "system_pool_machine" {
  type    = string
  default = "e2-standard-4"
}

variable "system_pool_min" {
  type    = number
  default = 2
}

variable "system_pool_max" {
  type    = number
  default = 6
}

variable "sandbox_pool_machine" {
  type    = string
  default = "c3-standard-8"
}

variable "sandbox_pool_min" {
  type    = number
  default = 1
}

variable "sandbox_pool_max" {
  type    = number
  default = 12
}

variable "artefact_bucket_force_destroy" {
  description = "Allow terraform destroy to wipe a non-empty submissions bucket. Set false in prod."
  type        = bool
  default     = true
}
