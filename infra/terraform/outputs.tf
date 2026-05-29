################################################################################
# outputs.tf — handy outputs for downstream pipelines.
################################################################################

output "region" {
  value = var.region
}

output "cluster_name" {
  value = module.eks.cluster_name
}

output "cluster_endpoint" {
  value     = module.eks.cluster_endpoint
  sensitive = true
}

output "cluster_certificate_authority_data" {
  value     = module.eks.cluster_certificate_authority_data
  sensitive = true
}

output "kubeconfig_update_command" {
  value       = "aws eks update-kubeconfig --name ${module.eks.cluster_name} --region ${var.region}"
  description = "Run this locally after apply to point kubectl at the new cluster."
}

output "ecr_repositories" {
  value       = { for k, v in aws_ecr_repository.service : k => v.repository_url }
  description = "Push targets for each service image."
}

output "artefact_bucket" {
  value = aws_s3_bucket.artefacts.bucket
}

output "artefact_bucket_arn" {
  value = aws_s3_bucket.artefacts.arn
}

output "vpc_id" {
  value = module.vpc.vpc_id
}
