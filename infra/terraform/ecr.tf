################################################################################
# ecr.tf — one ECR repo per service image.
#
# Repos are immutable + scan-on-push. A lifecycle policy keeps the 30 newest
# tags and prunes the rest, so we don't accumulate 6 months of nightlies.
################################################################################

resource "aws_ecr_repository" "service" {
  for_each = toset(local.service_images)

  name                 = "velocity/${each.key}"
  image_tag_mutability = "IMMUTABLE"

  image_scanning_configuration {
    scan_on_push = true
  }

  encryption_configuration {
    encryption_type = "AES256"
  }

  tags = {
    "Name"    = "velocity-${each.key}"
    "Service" = each.key
  }
}

resource "aws_ecr_lifecycle_policy" "service" {
  for_each   = aws_ecr_repository.service
  repository = each.value.name

  policy = jsonencode({
    rules = [
      {
        rulePriority = 1
        description  = "Keep the 30 most recent immutable tags."
        selection = {
          tagStatus   = "any"
          countType   = "imageCountMoreThan"
          countNumber = 30
        }
        action = { type = "expire" }
      },
    ]
  })
}
