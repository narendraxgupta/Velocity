################################################################################
# iam.tf — IRSA roles consumed by in-cluster service accounts.
#
# We deliberately give each service its own role, scoped to the least-
# privileged set of actions it needs. The submission-engine, for instance,
# needs S3 read/write on artefacts but no DynamoDB.
################################################################################

# ------------------------------------------------------------------ helpers
data "aws_iam_policy_document" "submission_engine_s3" {
  statement {
    sid    = "ReadWriteSubmissionArtefacts"
    effect = "Allow"
    actions = [
      "s3:GetObject", "s3:GetObjectVersion",
      "s3:PutObject", "s3:DeleteObject",
      "s3:AbortMultipartUpload",
      "s3:ListMultipartUploadParts",
    ]
    resources = ["${aws_s3_bucket.artefacts.arn}/*"]
  }
  statement {
    sid       = "ListSubmissionBucket"
    effect    = "Allow"
    actions   = ["s3:ListBucket", "s3:GetBucketLocation"]
    resources = [aws_s3_bucket.artefacts.arn]
  }
}

# ECR push for any service the submission-engine builds (Kaniko in pod).
data "aws_iam_policy_document" "kaniko_ecr" {
  statement {
    effect = "Allow"
    actions = [
      "ecr:GetAuthorizationToken",
    ]
    resources = ["*"]
  }
  statement {
    effect = "Allow"
    actions = [
      "ecr:BatchCheckLayerAvailability",
      "ecr:CompleteLayerUpload",
      "ecr:InitiateLayerUpload",
      "ecr:PutImage",
      "ecr:UploadLayerPart",
      "ecr:BatchGetImage",
      "ecr:GetDownloadUrlForLayer",
    ]
    resources = [for r in aws_ecr_repository.service : r.arn]
  }
}

# ------------------------------------------------------------------ roles

module "submission_engine_irsa" {
  source  = "terraform-aws-modules/iam/aws//modules/iam-role-for-service-accounts-eks"
  version = "~> 5.44"

  role_name = "${local.name}-submission-engine"
  role_policy_arns = {
    s3_artefacts = aws_iam_policy.submission_engine_s3.arn,
    ecr_push     = aws_iam_policy.kaniko_ecr.arn,
  }

  oidc_providers = {
    main = {
      provider_arn               = module.eks.oidc_provider_arn
      namespace_service_accounts = ["velocity-control:submission-engine"]
    }
  }
}

resource "aws_iam_policy" "submission_engine_s3" {
  name        = "${local.name}-submission-engine-s3"
  description = "S3 read/write on the submission artefact bucket."
  policy      = data.aws_iam_policy_document.submission_engine_s3.json
}

resource "aws_iam_policy" "kaniko_ecr" {
  name        = "${local.name}-kaniko-ecr"
  description = "ECR push for the submission-engine's Kaniko build pods."
  policy      = data.aws_iam_policy_document.kaniko_ecr.json
}

# ------------------------------------------------------------------ k8s SA hint
#
# The Kubernetes manifests under `kubernetes/base/` should annotate the
# `submission-engine` ServiceAccount with the role ARN below so EKS-IRSA
# picks it up.
output "submission_engine_irsa_role_arn" {
  value       = module.submission_engine_irsa.iam_role_arn
  description = "Annotate ServiceAccount with eks.amazonaws.com/role-arn=<this>."
}
