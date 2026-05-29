################################################################################
# backend.tf — remote state.
#
# We use S3 for state storage and DynamoDB for state locking. The bucket
# and table are created out-of-band (chicken-and-egg) by `bootstrap.sh`.
#
# Per-environment values are passed at `terraform init` time via the
# `-backend-config` flag, e.g.:
#
#   terraform init \
#     -backend-config=env/dev.backend.tfvars
################################################################################

terraform {
  backend "s3" {
    # Static defaults; override per env via -backend-config.
    bucket         = "velocity-tfstate"
    key            = "velocity/terraform.tfstate"
    region         = "us-east-1"
    encrypt        = true
    dynamodb_table = "velocity-tfstate-lock"
  }
}
