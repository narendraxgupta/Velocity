################################################################################
# s3.tf — submission artefact storage (production replacement for MinIO).
#
# Artefacts are uploaded by the submission-engine; Kaniko consumes them
# during the build phase. Each upload is keyed by SHA-256, never overwritten,
# and aged out after `var.artefact_retention_days`.
################################################################################

# Bucket name needs to be globally unique; suffix with the AWS account.
locals {
  artefact_bucket_name = "velocity-${var.environment}-submissions-${local.account_id}"
}

resource "aws_s3_bucket" "artefacts" {
  bucket        = local.artefact_bucket_name
  force_destroy = var.artefact_bucket_force_destroy
}

resource "aws_s3_bucket_versioning" "artefacts" {
  bucket = aws_s3_bucket.artefacts.id
  versioning_configuration {
    status = "Enabled"
  }
}

resource "aws_s3_bucket_public_access_block" "artefacts" {
  bucket                  = aws_s3_bucket.artefacts.id
  block_public_acls       = true
  block_public_policy     = true
  ignore_public_acls      = true
  restrict_public_buckets = true
}

resource "aws_s3_bucket_server_side_encryption_configuration" "artefacts" {
  bucket = aws_s3_bucket.artefacts.id
  rule {
    apply_server_side_encryption_by_default {
      sse_algorithm = "AES256"
    }
    bucket_key_enabled = true
  }
}

resource "aws_s3_bucket_lifecycle_configuration" "artefacts" {
  bucket = aws_s3_bucket.artefacts.id

  rule {
    id     = "expire-old-artefacts"
    status = "Enabled"

    filter {}

    expiration {
      days = var.artefact_retention_days
    }
    noncurrent_version_expiration {
      noncurrent_days = 7
    }
    abort_incomplete_multipart_upload {
      days_after_initiation = 1
    }
  }
}

resource "aws_s3_bucket_ownership_controls" "artefacts" {
  bucket = aws_s3_bucket.artefacts.id
  rule {
    object_ownership = "BucketOwnerEnforced"
  }
}

# CORS — the frontend uploads directly to S3 via the gateway-issued presigned
# URL. Without CORS the browser fetch fails for POST/PUT.
resource "aws_s3_bucket_cors_configuration" "artefacts" {
  bucket = aws_s3_bucket.artefacts.id
  cors_rule {
    allowed_methods = ["GET", "PUT", "POST"]
    allowed_origins = var.environment == "prod" ? ["https://*.velocity.io"] : ["*"]
    allowed_headers = ["*"]
    expose_headers  = ["ETag"]
    max_age_seconds = 3000
  }
}
