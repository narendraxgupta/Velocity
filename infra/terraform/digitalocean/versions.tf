terraform {
  required_version = ">= 1.7.0"
  required_providers {
    digitalocean = {
      source  = "digitalocean/digitalocean"
      version = "~> 2.42"
    }
  }
}

provider "digitalocean" {
  token = var.do_token

  # Spaces (S3-compatible object storage) authenticates with its own
  # access-key pair, NOT the DO API token. Without these the
  # `digitalocean_spaces_bucket.artefacts` resource fails at apply with a
  # 403 — the token alone can't reach the Spaces endpoint.
  spaces_access_id  = var.spaces_access_key
  spaces_secret_key = var.spaces_secret_key
}
