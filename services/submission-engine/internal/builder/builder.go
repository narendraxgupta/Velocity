// Package builder turns a submission artefact stored in MinIO into an OCI
// image pushed to our private registry.
//
// Implementation: we spawn a Kubernetes Job whose pod runs the Kaniko
// executor image. Kaniko handles rootless image builds inside an unprivileged
// pod — exactly the security posture we want for untrusted code.
package builder

import (
	"bufio"
	"context"
	"fmt"
	"io"
	"sync"
	"time"

	"github.com/redis/go-redis/v9"
	batchv1 "k8s.io/api/batch/v1"
	corev1 "k8s.io/api/core/v1"
	apierrors "k8s.io/apimachinery/pkg/api/errors"
	"k8s.io/apimachinery/pkg/api/resource"
	metav1 "k8s.io/apimachinery/pkg/apis/meta/v1"
	"k8s.io/client-go/kubernetes"
)

// Request describes one build job.
type Request struct {
	SubmissionID    string
	ArtefactKey     string // object key in MinIO
	ArtefactSHA     string // hex sha-256 we expect the artefact to hash to
	ArtefactPresign string // presigned URL Kaniko can curl
	BuildKind       Kind
}

// Kind enumerates what Builder needs to do with the artefact.
type Kind int

const (
	KindUnspecified Kind = iota
	KindDockerfile
	KindSourceTar
	KindBinary
	KindOCIImage
)

// Result is the build output — primarily the canonical image ref.
type Result struct {
	ImageRef string
	JobName  string
}

// Builder is the interface every concrete implementation satisfies.
type Builder interface {
	Build(ctx context.Context, req Request) (*Result, error)
}

// Config holds the immutable configuration the Kaniko builder needs.
type Config struct {
	K8sClient     kubernetes.Interface
	Namespace     string
	RegistryHost  string
	KanikoImage   string // default "gcr.io/kaniko-project/executor:latest"
	JobTTLSeconds int32  // delete completed Jobs after this many seconds

	// RegistryInsecure disables registry TLS verification for the Kaniko
	// push. Secure-by-default (false) so a MITM can't inject layers into a
	// submission image in production; enable it only for a plain-HTTP dev
	// registry (local k3d/compose).
	RegistryInsecure bool

	// Optional Redis client for build-log fan-out. When nil the builder
	// runs identically — logs are simply not surfaced to the gateway.
	Redis *redis.Client

	// BuildLogMaxLines caps the per-submission ring buffer in Redis.
	// Defaults to 2000.
	BuildLogMaxLines int64
}

// NewKaniko returns a Builder that runs Kaniko in a K8s Job.
func NewKaniko(cfg Config) Builder {
	if cfg.KanikoImage == "" {
		cfg.KanikoImage = "gcr.io/kaniko-project/executor:latest"
	}
	if cfg.JobTTLSeconds == 0 {
		cfg.JobTTLSeconds = 3600
	}
	return &kanikoBuilder{cfg: cfg}
}

type kanikoBuilder struct {
	cfg Config
}

func (b *kanikoBuilder) Build(ctx context.Context, req Request) (*Result, error) {
	if req.ArtefactPresign == "" {
		return nil, fmt.Errorf("kaniko build requires presigned artefact URL")
	}
	if b.cfg.K8sClient == nil {
		return nil, fmt.Errorf("kaniko builder not configured with K8s client")
	}

	imageRef := fmt.Sprintf("%s/velocity/%s:latest", b.cfg.RegistryHost, req.SubmissionID)
	jobName := fmt.Sprintf("velocity-build-%s", req.SubmissionID)

	ttl := b.cfg.JobTTLSeconds
	backoff := int32(0)
	activeDeadline := int64(20 * 60)

	// Kaniko args. We point it at a tarball URL; the artefact must be a tar
	// containing a Dockerfile at the root.
	args := []string{
		"--context", "tar://" + req.ArtefactPresign,
		"--destination", imageRef,
		"--snapshot-mode=redo",
		"--cache=true",
		"--cache-ttl=24h",
	}
	// Disable registry TLS verification only when explicitly allowed (the
	// dev registry is plain HTTP). Defaults on for dev compat; prod sets
	// VELOCITY_REGISTRY_INSECURE=false to require a verified TLS push.
	if b.cfg.RegistryInsecure {
		args = append(args, "--insecure", "--skip-tls-verify")
	}

	job := &batchv1.Job{
		ObjectMeta: metav1.ObjectMeta{
			Name:      jobName,
			Namespace: b.cfg.Namespace,
			Labels: map[string]string{
				"app.kubernetes.io/part-of":   "velocity",
				"app.kubernetes.io/component": "build",
				"velocity.io/submission-id":   req.SubmissionID,
			},
		},
		Spec: batchv1.JobSpec{
			BackoffLimit:            &backoff,
			ActiveDeadlineSeconds:   &activeDeadline,
			TTLSecondsAfterFinished: &ttl,
			Template: corev1.PodTemplateSpec{
				ObjectMeta: metav1.ObjectMeta{
					Labels: map[string]string{
						"velocity.io/submission-id": req.SubmissionID,
					},
				},
				Spec: corev1.PodSpec{
					RestartPolicy: corev1.RestartPolicyNever,
					Containers: []corev1.Container{
						{
							Name:  "kaniko",
							Image: b.cfg.KanikoImage,
							Args:  args,
							Resources: corev1.ResourceRequirements{
								Requests: corev1.ResourceList{
									corev1.ResourceCPU:    resource.MustParse("500m"),
									corev1.ResourceMemory: resource.MustParse("1Gi"),
								},
								Limits: corev1.ResourceList{
									corev1.ResourceCPU:    resource.MustParse("2"),
									corev1.ResourceMemory: resource.MustParse("4Gi"),
								},
							},
						},
					},
				},
			},
		},
	}

	jobs := b.cfg.K8sClient.BatchV1().Jobs(b.cfg.Namespace)
	if _, err := jobs.Create(ctx, job, metav1.CreateOptions{}); err != nil {
		if !apierrors.IsAlreadyExists(err) {
			return nil, fmt.Errorf("create job %s: %w", jobName, err)
		}
	}

	// Kick off the log shipper. It blocks waiting for the pod to exist,
	// then streams stdout into Redis. We don't propagate its errors —
	// build correctness must not depend on log delivery.
	if b.cfg.Redis != nil {
		go b.shipLogs(ctx, req.SubmissionID, jobName)
	}

	// Wait for completion. Poll every 2s; bail on context cancellation.
	deadline := time.Now().Add(20 * time.Minute)
	poll := time.NewTicker(2 * time.Second)
	defer poll.Stop()
	for {
		if time.Now().After(deadline) {
			return nil, fmt.Errorf("build %s timed out", jobName)
		}
		select {
		case <-ctx.Done():
			return nil, ctx.Err()
		case <-poll.C:
		}
		got, err := jobs.Get(ctx, jobName, metav1.GetOptions{})
		if err != nil {
			return nil, fmt.Errorf("get job: %w", err)
		}
		if got.Status.Succeeded > 0 {
			return &Result{ImageRef: imageRef, JobName: jobName}, nil
		}
		if got.Status.Failed > 0 {
			return nil, fmt.Errorf("kaniko build failed (see job %s)", jobName)
		}
	}
}

// shipLogs streams the build pod's stdout into Redis. Best-effort —
// build correctness must not depend on log delivery.
func (b *kanikoBuilder) shipLogs(ctx context.Context, submissionID, jobName string) {
	maxLines := b.cfg.BuildLogMaxLines
	if maxLines <= 0 {
		maxLines = 2000
	}
	key := "build:" + submissionID

	// Reset whatever was there from a prior build.
	_ = b.cfg.Redis.Del(ctx, key).Err()

	// Find the pod once it exists. K8s Job pods bear the job-name label.
	pods := b.cfg.K8sClient.CoreV1().Pods(b.cfg.Namespace)
	var podName string
	wait := time.NewTicker(time.Second)
	defer wait.Stop()
	for i := 0; i < 60 && podName == ""; i++ {
		select {
		case <-ctx.Done():
			return
		case <-wait.C:
		}
		list, err := pods.List(ctx, metav1.ListOptions{LabelSelector: "job-name=" + jobName})
		if err == nil {
			for _, p := range list.Items {
				if p.Status.Phase == corev1.PodPending {
					continue
				}
				podName = p.Name
				break
			}
		}
	}
	if podName == "" {
		return
	}

	stream, err := pods.GetLogs(podName, &corev1.PodLogOptions{Follow: true}).Stream(ctx)
	if err != nil {
		return
	}
	defer func() { _ = stream.Close() }()

	// Buffered scan so we ship one redis op per log line. Cap each line at
	// 1 MiB to defend against runaway output.
	scanner := bufio.NewScanner(stream)
	scanner.Buffer(make([]byte, 64*1024), 1024*1024)

	var mu sync.Mutex
	push := func(line string) {
		mu.Lock()
		defer mu.Unlock()
		pipe := b.cfg.Redis.TxPipeline()
		pipe.RPush(ctx, key, line)
		pipe.LTrim(ctx, key, -maxLines, -1)
		pipe.Expire(ctx, key, 6*time.Hour)
		_, _ = pipe.Exec(ctx)
	}

	for scanner.Scan() {
		push(scanner.Text())
	}
	// If the scanner closed because EOF (or no error), flush a sentinel
	// line so the frontend can hide the spinner.
	if err := scanner.Err(); err == nil || err == io.EOF {
		push("[velocity] build pod log stream closed")
	}
}
