{{/*
  velocity._helpers.tpl — shared template snippets.

  Conventions:
    velocity.fullname               instance-qualified resource name
    velocity.image                  registry/<service>:<tag> resolution
    velocity.commonLabels           label set every object gets
    velocity.selectorLabels         label set used by Deployment selectors
*/}}

{{- define "velocity.name" -}}
{{- default "velocity" .Chart.Name -}}
{{- end -}}

{{- define "velocity.fullname" -}}
{{- printf "%s-%s" .Release.Name (default "velocity" .Chart.Name) | trunc 63 | trimSuffix "-" -}}
{{- end -}}

{{/* The release-scoped name for a single service. Usage:
     {{ include "velocity.componentName" (list . "api-gateway") }} */}}
{{- define "velocity.componentName" -}}
{{- $ctx := index . 0 -}}
{{- $name := index . 1 -}}
{{- printf "%s-%s" $ctx.Release.Name $name | trunc 63 | trimSuffix "-" -}}
{{- end -}}

{{/* Resolve an image reference. Usage:
     {{ include "velocity.image" (dict "ctx" . "image" "api-gateway" "tag" .Values.global.tag) }} */}}
{{- define "velocity.image" -}}
{{- $ctx := .ctx -}}
{{- $reg := default $ctx.Values.global.registry "" -}}
{{- $tag := default $ctx.Values.global.tag .tag -}}
{{- if eq $reg "" -}}
{{- printf "%s:%s" .image $tag -}}
{{- else -}}
{{- printf "%s/%s:%s" $reg .image $tag -}}
{{- end -}}
{{- end -}}

{{- define "velocity.commonLabels" -}}
app.kubernetes.io/part-of: velocity
app.kubernetes.io/managed-by: {{ .Release.Service }}
app.kubernetes.io/instance: {{ .Release.Name }}
helm.sh/chart: {{ .Chart.Name }}-{{ .Chart.Version }}
{{- end -}}

{{- define "velocity.selectorLabels" -}}
{{- $ctx := index . 0 -}}
{{- $name := index . 1 -}}
app.kubernetes.io/name: {{ $name }}
app.kubernetes.io/instance: {{ $ctx.Release.Name }}
{{- end -}}

{{/* Redis address — internal or external. */}}
{{- define "velocity.redisAddr" -}}
{{- if eq .Values.redis.mode "external" -}}
{{ required "redis.external.addr required when mode=external" .Values.redis.external.addr }}
{{- else -}}
{{ printf "%s-redis.%s.svc.cluster.local:6379" .Release.Name (printf "%s-data" .Release.Name) }}
{{- end -}}
{{- end -}}

{{- define "velocity.kafkaBrokers" -}}
{{- if eq .Values.redpanda.mode "external" -}}
{{ required "redpanda.external.brokers required" .Values.redpanda.external.brokers }}
{{- else -}}
{{ printf "%s-redpanda.%s.svc.cluster.local:9092" .Release.Name (printf "%s-data" .Release.Name) }}
{{- end -}}
{{- end -}}
