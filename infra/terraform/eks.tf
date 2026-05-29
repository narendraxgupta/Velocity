################################################################################
# eks.tf — EKS cluster.
#
# Two managed node groups (see `node-groups.tf`), KMS-encrypted secrets,
# control-plane logs to CloudWatch, and IRSA enabled so individual service
# accounts can assume IAM roles.
################################################################################

module "eks" {
  source  = "terraform-aws-modules/eks/aws"
  version = "~> 20.20"

  cluster_name    = local.name
  cluster_version = local.cluster_version

  vpc_id     = module.vpc.vpc_id
  subnet_ids = module.vpc.private_subnets
  # Sandbox subnets are intentionally NOT in the cluster's subnet list — they
  # join the cluster via the launch template in `node-groups.tf`.
  control_plane_subnet_ids = module.vpc.private_subnets

  enable_irsa = true

  # Restrict who can reach the public API server endpoint. In dev we allow
  # internet; in prod we expect this to be locked to the operator subnet.
  cluster_endpoint_public_access       = true
  cluster_endpoint_public_access_cidrs = var.environment == "prod" ? [] : ["0.0.0.0/0"]
  cluster_endpoint_private_access      = true

  cluster_enabled_log_types = [
    "api", "audit", "authenticator", "controllerManager", "scheduler",
  ]

  # Use AWS-managed encryption for K8s secrets.
  create_kms_key = true
  cluster_encryption_config = {
    resources = ["secrets"]
  }

  # Cluster add-ons. The vpc-cni / coredns / kube-proxy trio is mandatory;
  # the AWS EBS CSI driver supplies persistent volumes for QuestDB/Redis.
  cluster_addons = {
    vpc-cni = {
      most_recent = true
      configuration_values = jsonencode({
        env = {
          # Required for IRSA — gives the CNI its own IAM role.
          ENABLE_PREFIX_DELEGATION = "true"
        }
      })
    }
    coredns            = { most_recent = true }
    kube-proxy         = { most_recent = true }
    aws-ebs-csi-driver = { most_recent = true }
  }

  # Map IAM admin principals to system:masters.
  enable_cluster_creator_admin_permissions = true
  access_entries = {
    for arn in var.admin_iam_principals : replace(arn, "/[^A-Za-z0-9]/", "_") => {
      principal_arn = arn
      policy_associations = {
        admin = {
          policy_arn = "arn:${data.aws_partition.current.partition}:eks::aws:cluster-access-policy/AmazonEKSClusterAdminPolicy"
          access_scope = {
            type = "cluster"
          }
        }
      }
    }
  }

  tags = {
    "Name" = local.name
  }
}
