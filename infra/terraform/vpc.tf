################################################################################
# vpc.tf — VPC, subnets, NAT, route tables.
#
# Layout (per /16 VPC, split across 3 AZs):
#   - private  3 × /20 (blocks 0,1,2)    — control-plane workloads.
#   - public   3 × /24 (blocks 48,49,50) — NAT + load balancers.
#   - sandbox  3 × /20 (blocks 4,5,6)    — gVisor pods (intra-VPC only).
#
# CRITICAL: subnets are DERIVED from var.vpc_cidr via cidrsubnet() rather than
# hardcoded. Hardcoded 10.40.x ranges silently broke prod, whose vpc_cidr is
# 10.60.0.0/16 — AWS rejects subnets that fall outside the VPC CIDR, so
# `terraform apply` failed before creating anything. Deriving them keeps the
# exact same dev layout (10.40.x) while staying valid for any /16 CIDR.
#
# We use the official `terraform-aws-modules/vpc` module — battle tested and
# avoids the dozen-resource trap of writing this longhand.
################################################################################

locals {
  # All three tiers are carved out of var.vpc_cidr so they are guaranteed to
  # sit inside the VPC for any environment.
  vpc_private_subnets = [for i in [0, 1, 2] : cidrsubnet(var.vpc_cidr, 4, i)]
  vpc_public_subnets  = [for i in [48, 49, 50] : cidrsubnet(var.vpc_cidr, 8, i)]
  vpc_intra_subnets   = [for i in [4, 5, 6] : cidrsubnet(var.vpc_cidr, 4, i)]
}

module "vpc" {
  source  = "terraform-aws-modules/vpc/aws"
  version = "~> 5.13"

  name = local.name
  cidr = var.vpc_cidr

  azs = local.azs

  private_subnets = local.vpc_private_subnets
  public_subnets  = local.vpc_public_subnets
  intra_subnets   = local.vpc_intra_subnets

  enable_nat_gateway     = true
  single_nat_gateway     = var.environment != "prod"
  one_nat_gateway_per_az = var.environment == "prod"
  enable_dns_hostnames   = true
  enable_dns_support     = true

  # VPC flow logs — invaluable when judging a "where did my benchmark go"
  # incident. Cheap to store, free to query via Athena.
  enable_flow_log                      = true
  create_flow_log_cloudwatch_iam_role  = true
  create_flow_log_cloudwatch_log_group = true
  flow_log_max_aggregation_interval    = 60

  # Subnet tags required by AWS Load Balancer Controller to discover where
  # to provision NLBs/ALBs from the in-cluster ingress.
  public_subnet_tags = {
    "kubernetes.io/role/elb" = "1"
  }
  private_subnet_tags = {
    "kubernetes.io/role/internal-elb" = "1"
    "karpenter.sh/discovery"          = local.name
  }

  tags = {
    "Name" = "${local.name}-vpc"
  }
}

# `intra_subnets` are a reserved, NAT-free tier. NOTE: the sandbox EKS node
# group does NOT currently run here — see node-groups.tf, where it uses the
# (NAT-routed) private subnets. That is deliberate: the node bootstrap fetches
# the gVisor `runsc` release over the internet and pulls images from ECR, both
# of which need egress. Moving the nodes here would require pre-baking runsc
# into a custom AMI plus PrivateLink endpoints for ECR/S3/STS.
#
# The "untrusted submission cannot reach the internet" guarantee is therefore
# NOT a subnet/NAT property — it is enforced at the POD layer by the
# default-deny-all NetworkPolicy in the velocity-sandbox namespace
# (infra/kubernetes/base/networkpolicy-sandbox.yaml), which drops ALL pod
# egress (including DNS). Keep that policy intact; it is the real control.
