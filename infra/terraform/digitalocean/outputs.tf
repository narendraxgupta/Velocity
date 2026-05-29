output "cluster_name" {
  value       = digitalocean_kubernetes_cluster.velocity.name
  description = "DOKS cluster name — pair with `doctl kubernetes cluster kubeconfig save`."
}

output "cluster_endpoint" {
  value       = digitalocean_kubernetes_cluster.velocity.endpoint
  description = "Kubernetes API server URL."
  sensitive   = true
}

output "registry_endpoint" {
  value       = digitalocean_container_registry.velocity.endpoint
  description = "Container registry endpoint — push images here, then point Helm's global.registry at this URL."
}

output "spaces_bucket_endpoint" {
  value       = "https://${digitalocean_spaces_bucket.artefacts.bucket_domain_name}"
  description = "S3-compatible Spaces endpoint for submission artefacts."
}

output "helm_values_hint" {
  value       = <<EOT
# Suggested values overrides:
global:
  registry: ${digitalocean_container_registry.velocity.endpoint}
minio:
  mode: external
  external:
    endpoint:  "${digitalocean_spaces_bucket.artefacts.bucket_domain_name}"
    accessKey: "<your-spaces-key>"
    secretKey: "<your-spaces-secret>"
EOT
  description = "Copy into your -f overrides.yaml."
}
