output "cluster_name" {
  value       = google_container_cluster.velocity.name
  description = "GKE cluster name."
}

output "cluster_endpoint" {
  value       = google_container_cluster.velocity.endpoint
  description = "GKE API server endpoint."
  sensitive   = true
}

output "artefact_registry" {
  value       = google_artifact_registry_repository.velocity.name
  description = "Artifact Registry repository id."
}

output "artefact_bucket" {
  value       = google_storage_bucket.artefacts.url
  description = "GCS bucket for submission artefacts."
}

output "helm_values_hint" {
  value = <<EOT
global:
  registry: ${var.region}-docker.pkg.dev/${var.project_id}/${google_artifact_registry_repository.velocity.repository_id}
minio:
  mode: external
  external:
    endpoint:  storage.googleapis.com
    accessKey: "<your-hmac-access-id>"
    secretKey: "<your-hmac-secret>"
EOT
}
