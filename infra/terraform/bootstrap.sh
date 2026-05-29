#!/usr/bin/env bash
# =============================================================================
#  bootstrap.sh — chicken-and-egg setup.
#
#  Terraform's S3 backend needs a bucket + DDB table that exist *before*
#  `terraform init` runs. This script creates them idempotently.
#
#  Usage:  ./bootstrap.sh [region]
#  Defaults to us-east-1.
# =============================================================================
set -euo pipefail

REGION="${1:-us-east-1}"
BUCKET="velocity-tfstate"
TABLE="velocity-tfstate-lock"

echo "==> Region:  $REGION"
echo "==> Bucket:  $BUCKET"
echo "==> Table:   $TABLE"

if ! aws s3api head-bucket --bucket "$BUCKET" 2>/dev/null; then
  echo "==> Creating S3 bucket"
  if [ "$REGION" = "us-east-1" ]; then
    aws s3api create-bucket --bucket "$BUCKET" --region "$REGION"
  else
    aws s3api create-bucket --bucket "$BUCKET" --region "$REGION" \
      --create-bucket-configuration "LocationConstraint=$REGION"
  fi
  aws s3api put-bucket-versioning --bucket "$BUCKET" --versioning-configuration Status=Enabled
  aws s3api put-bucket-encryption --bucket "$BUCKET" --server-side-encryption-configuration \
    '{"Rules":[{"ApplyServerSideEncryptionByDefault":{"SSEAlgorithm":"AES256"}}]}'
  aws s3api put-public-access-block --bucket "$BUCKET" --public-access-block-configuration \
    'BlockPublicAcls=true,IgnorePublicAcls=true,BlockPublicPolicy=true,RestrictPublicBuckets=true'
else
  echo "==> Bucket already exists"
fi

if ! aws dynamodb describe-table --table-name "$TABLE" --region "$REGION" >/dev/null 2>&1; then
  echo "==> Creating DDB lock table"
  aws dynamodb create-table \
    --table-name "$TABLE" --region "$REGION" \
    --attribute-definitions AttributeName=LockID,AttributeType=S \
    --key-schema AttributeName=LockID,KeyType=HASH \
    --billing-mode PAY_PER_REQUEST
  aws dynamodb wait table-exists --table-name "$TABLE" --region "$REGION"
else
  echo "==> Lock table already exists"
fi

echo "==> Bootstrap complete"
