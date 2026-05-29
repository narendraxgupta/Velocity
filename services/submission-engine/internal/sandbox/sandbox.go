// Package sandbox launches submission containers in strict isolation and
// tears them down when the benchmark window ends.
//
// Production backend: Kubernetes pod with `RuntimeClass: gvisor`, which
// gives us a syscall-level user-space kernel between the submission code
// and the host. This is the same isolation Google uses for AppEngine.
//
// The pod has:
//   * No service account (cannot talk to K8s API)
//   * Read-only root filesystem
//   * No host network or host-pid
//   * Restricted seccomp profile (RuntimeDefault)
//   * CPU/memory limits enforced by cgroups
//
// See infra/k8s/base/namespace.yaml — the namespace itself denies all
// egress by default; we add a single NetworkPolicy that whitelists the
// bot-controller's traffic into the pod.
package sandbox

import (
	"context"
	"fmt"
	"time"

	corev1 "k8s.io/api/core/v1"
	apierrors "k8s.io/apimachinery/pkg/api/errors"
	"k8s.io/apimachinery/pkg/api/resource"
	metav1 "k8s.io/apimachinery/pkg/apis/meta/v1"
	"k8s.io/apimachinery/pkg/util/intstr"
	"k8s.io/client-go/kubernetes"
)

// LaunchRequest describes one sandbox creation.
type LaunchRequest struct {
	SubmissionID string
	ImageRef     string
	CPUCores     uint32
	MemoryMiB    uint32
	Lifetime     time.Duration

	// EnableProfiler attaches a `perf record` sidecar that samples the
	// submission container at 99 Hz, folds the resulting stacks, and
	// uploads the folded text + SVG to MinIO at
	//   flamegraphs/<submission_id>.{folded.txt,svg}
	// The sidecar runs in the pod's PID namespace (shareProcessNamespace=true)
	// and needs CAP_SYS_PTRACE + CAP_PERFMON. Setting this implicitly drops
	// gVisor for the submission pod, because gVisor blocks perf_event_open;
	// callers should only enable when isolation requirements are flexible.
	EnableProfiler bool
}

// Endpoint is what we hand back to the bot fleet so it knows where to send
// orders.
type Endpoint struct {
	Host string
	Port uint16
	Path string
}

// LaunchResult describes the running sandbox.
type LaunchResult struct {
	Endpoint  Endpoint
	Pod       string
	Namespace string
}

// Sandbox is the interface every backend implements.
type Sandbox interface {
	Launch(ctx context.Context, req LaunchRequest) (*LaunchResult, error)
	Teardown(ctx context.Context, submissionID string) error
}

// Config carries the K8s knobs used to instantiate sandbox pods.
type Config struct {
	K8sClient    kubernetes.Interface
	Namespace    string
	RuntimeClass string // "gvisor" in production
	ServicePort  int32  // submission container's HTTP port (default 8080)

	// ProfilerImage is the container image used by the perf-profiler sidecar
	// when LaunchRequest.EnableProfiler is true. Must bundle `perf`, the
	// `inferno-collapse-perf` and `inferno-flamegraph` binaries (Rust),
	// and an `mc` (minio-client) binary so it can upload the folded
	// output. Defaults to `ghcr.io/velocity/perf-profiler:latest`.
	ProfilerImage string

	// MinIOAddress / MinIO credentials passed to the profiler sidecar so it
	// can upload its folded.txt / svg outputs. Sourced from a secret in
	// production; falls back to env defaults when empty.
	MinIOEndpoint  string
	MinIOAccessKey string
	MinIOSecretKey string
	MinIOBucket    string
}

// NewGVisor returns a Kubernetes-backed sandbox.
func NewGVisor(cfg Config) Sandbox {
	if cfg.RuntimeClass == "" {
		cfg.RuntimeClass = "gvisor"
	}
	if cfg.ServicePort == 0 {
		cfg.ServicePort = 8080
	}
	if cfg.ProfilerImage == "" {
		cfg.ProfilerImage = "ghcr.io/velocity/perf-profiler:latest"
	}
	if cfg.MinIOBucket == "" {
		cfg.MinIOBucket = "velocity-artefacts"
	}
	return &gvisorSandbox{cfg: cfg}
}

type gvisorSandbox struct {
	cfg Config
}

func (s *gvisorSandbox) Launch(ctx context.Context, req LaunchRequest) (*LaunchResult, error) {
	if s.cfg.K8sClient == nil {
		return nil, fmt.Errorf("sandbox not configured with K8s client")
	}
	if req.SubmissionID == "" {
		return nil, fmt.Errorf("LaunchRequest missing SubmissionID")
	}
	// Sensible defaults so callers that omit limits still get a sandbox
	// inside the namespace's ResourceQuota envelope.
	if req.CPUCores == 0 {
		req.CPUCores = 2
	}
	if req.MemoryMiB == 0 {
		req.MemoryMiB = 1024
	}
	if req.Lifetime <= 0 {
		req.Lifetime = 30 * time.Minute
	}

	podName := podNameFor(req.SubmissionID)
	cpuQty := resource.MustParse(fmt.Sprintf("%dm", req.CPUCores*1000))
	memQty := resource.MustParse(fmt.Sprintf("%dMi", req.MemoryMiB))
	activeDeadline := int64(req.Lifetime / time.Second)

	runAsNonRoot := true
	allowPriv := false
	readOnly := true
	dropAll := corev1.SecurityContext{
		AllowPrivilegeEscalation: &allowPriv,
		ReadOnlyRootFilesystem:   &readOnly,
		Capabilities: &corev1.Capabilities{
			Drop: []corev1.Capability{"ALL"},
		},
		SeccompProfile: &corev1.SeccompProfile{
			Type: corev1.SeccompProfileTypeRuntimeDefault,
		},
		RunAsNonRoot: &runAsNonRoot,
	}

	containers := []corev1.Container{
		{
			Name:            "submission",
			Image:           req.ImageRef,
			ImagePullPolicy: corev1.PullIfNotPresent,
			Ports: []corev1.ContainerPort{
				{ContainerPort: s.cfg.ServicePort, Protocol: corev1.ProtocolTCP},
			},
			Resources: corev1.ResourceRequirements{
				Requests: corev1.ResourceList{
					corev1.ResourceCPU:    cpuQty,
					corev1.ResourceMemory: memQty,
				},
				Limits: corev1.ResourceList{
					corev1.ResourceCPU:    cpuQty,
					corev1.ResourceMemory: memQty,
				},
			},
			ReadinessProbe: &corev1.Probe{
				ProbeHandler: corev1.ProbeHandler{
					HTTPGet: &corev1.HTTPGetAction{
						Path: "/healthz",
						Port: intstr.FromInt32(s.cfg.ServicePort),
					},
				},
				InitialDelaySeconds: 2,
				PeriodSeconds:       2,
				TimeoutSeconds:      1,
				FailureThreshold:    5,
			},
			SecurityContext: &dropAll,
		},
	}

	// Choose the runtime class. perf_event_open is forbidden under gVisor,
	// so if the caller asked for profiling we hand control back to runc by
	// leaving RuntimeClassName nil. The sidecar's PID-namespace and ptrace
	// requirements also forbid the default seccomp profile.
	runtimeClass := &s.cfg.RuntimeClass
	shareProcessNamespace := false
	var volumes []corev1.Volume

	if req.EnableProfiler {
		runtimeClass = nil
		shareProcessNamespace = true

		// Shared scratch volume for the sidecar to write folded.txt/svg.
		volumes = append(volumes, corev1.Volume{
			Name: "profiler-out",
			VolumeSource: corev1.VolumeSource{
				EmptyDir: &corev1.EmptyDirVolumeSource{
					SizeLimit: ptrQuantity("256Mi"),
				},
			},
		})
		// Mount the volume on the submission container so the sidecar can
		// reach the engine's `/tmp` if it wants to inspect generated files.
		containers[0].VolumeMounts = []corev1.VolumeMount{
			{Name: "profiler-out", MountPath: "/var/velocity/profile"},
		}

		// Sidecar — runs perf record for the full lifetime, then folds &
		// uploads on SIGTERM (or pod teardown). All wrapped in a single
		// shell script for clarity; the perf-profiler image's entrypoint
		// expects $VELOCITY_SUBMISSION_ID and $VELOCITY_MINIO_ENDPOINT in
		// the env.
		profilerPriv := true   // perf_event_open requires this; gVisor is off
		profilerReadOnly := false
		containers = append(containers, corev1.Container{
			Name:            "perf-profiler",
			Image:           s.cfg.ProfilerImage,
			ImagePullPolicy: corev1.PullIfNotPresent,
			Env: []corev1.EnvVar{
				{Name: "VELOCITY_SUBMISSION_ID", Value: req.SubmissionID},
				{Name: "VELOCITY_MINIO_ENDPOINT", Value: s.cfg.MinIOEndpoint},
				{Name: "VELOCITY_MINIO_ACCESS", Value: s.cfg.MinIOAccessKey},
				{Name: "VELOCITY_MINIO_SECRET", Value: s.cfg.MinIOSecretKey},
				{Name: "VELOCITY_MINIO_BUCKET", Value: s.cfg.MinIOBucket},
				{Name: "VELOCITY_PROFILE_SAMPLE_HZ", Value: "99"},
			},
			VolumeMounts: []corev1.VolumeMount{
				{Name: "profiler-out", MountPath: "/var/velocity/profile"},
			},
			Resources: corev1.ResourceRequirements{
				Requests: corev1.ResourceList{
					corev1.ResourceCPU:    resource.MustParse("100m"),
					corev1.ResourceMemory: resource.MustParse("128Mi"),
				},
				Limits: corev1.ResourceList{
					corev1.ResourceCPU:    resource.MustParse("500m"),
					corev1.ResourceMemory: resource.MustParse("512Mi"),
				},
			},
			SecurityContext: &corev1.SecurityContext{
				Privileged:               &profilerPriv,
				ReadOnlyRootFilesystem:   &profilerReadOnly,
				AllowPrivilegeEscalation: &profilerPriv,
				Capabilities: &corev1.Capabilities{
					Add: []corev1.Capability{
						"SYS_PTRACE",        // attach to engine PID
						"PERFMON",           // perf_event_open
						"SYS_ADMIN",         // perf_event_paranoid=2 hosts need this
					},
				},
			},
		})
	}

	pod := &corev1.Pod{
		ObjectMeta: metav1.ObjectMeta{
			Name:      podName,
			Namespace: s.cfg.Namespace,
			Labels: map[string]string{
				"app.kubernetes.io/part-of":   "velocity",
				"app.kubernetes.io/component": "submission",
				"velocity.io/submission-id":   req.SubmissionID,
				"velocity.io/profiler":        boolLabel(req.EnableProfiler),
			},
		},
		Spec: corev1.PodSpec{
			RuntimeClassName:             runtimeClass,
			ShareProcessNamespace:        &shareProcessNamespace,
			AutomountServiceAccountToken: &allowPriv, // i.e. false
			EnableServiceLinks:           &allowPriv,
			HostNetwork:                  false,
			HostPID:                      false,
			HostIPC:                      false,
			RestartPolicy:                corev1.RestartPolicyNever,
			// kubelet kills the pod after this many seconds regardless of
			// whether the orchestrator remembered to call Teardown. This
			// is our last-line-of-defence against zombie sandboxes that
			// outlive a benchmark window.
			ActiveDeadlineSeconds: &activeDeadline,
			Volumes:               volumes,
			Containers:            containers,
		},
	}

	pods := s.cfg.K8sClient.CoreV1().Pods(s.cfg.Namespace)
	if _, err := pods.Create(ctx, pod, metav1.CreateOptions{}); err != nil {
		if !apierrors.IsAlreadyExists(err) {
			return nil, fmt.Errorf("create pod: %w", err)
		}
	}

	// Wait for Ready. If anything fails between here and a successful
	// return, we must delete the pod we just created or it will outlive
	// the controller's awareness of it (zombie sandbox).
	ip, waitErr := s.waitForReady(ctx, pods, podName)
	if waitErr != nil {
		// Best-effort cleanup with a fresh context — the caller's ctx
		// might be cancelled, but we still want to reap the pod.
		cleanCtx, cancel := context.WithTimeout(context.Background(), 10*time.Second)
		_ = pods.Delete(cleanCtx, podName, metav1.DeleteOptions{})
		cancel()
		return nil, waitErr
	}

	return &LaunchResult{
		Endpoint: Endpoint{
			Host: ip,
			Port: uint16(s.cfg.ServicePort),
			Path: "/orders",
		},
		Pod:       podName,
		Namespace: s.cfg.Namespace,
	}, nil
}

// waitForReady blocks until the pod reports a PodIP and every container
// is in Ready=true, the caller's context fires, or 60 seconds pass.
func (s *gvisorSandbox) waitForReady(ctx context.Context,
	pods podClient, podName string) (string, error) {
	deadline := time.Now().Add(60 * time.Second)
	poll := time.NewTicker(time.Second)
	defer poll.Stop()
	for {
		if time.Now().After(deadline) {
			return "", fmt.Errorf("sandbox pod %s never became ready", podName)
		}
		select {
		case <-ctx.Done():
			return "", ctx.Err()
		case <-poll.C:
		}
		got, err := pods.Get(ctx, podName, metav1.GetOptions{})
		if err != nil {
			// Transient API server errors should retry until the deadline;
			// only abort on context cancellation.
			if ctx.Err() != nil {
				return "", ctx.Err()
			}
			continue
		}
		// Surface terminal failures so the caller doesn't sit waiting on
		// a pod we already know is dead.
		switch got.Status.Phase {
		case corev1.PodFailed, corev1.PodSucceeded:
			return "", fmt.Errorf("sandbox pod %s terminated before ready: %s",
				podName, got.Status.Reason)
		}
		if got.Status.PodIP == "" {
			continue
		}
		ready := true
		for _, cs := range got.Status.ContainerStatuses {
			if !cs.Ready {
				ready = false
				break
			}
		}
		if ready {
			return got.Status.PodIP, nil
		}
	}
}

// podClient narrows the kubernetes client interface to just the methods
// waitForReady consumes; lets us mock it in tests without pulling in
// fake.NewSimpleClientset.
type podClient interface {
	Get(ctx context.Context, name string, opts metav1.GetOptions) (*corev1.Pod, error)
}

// podNameFor returns the deterministic pod name we use for a submission.
// Centralised so Launch and Teardown agree.
func podNameFor(submissionID string) string {
	return fmt.Sprintf("velocity-submission-%s", submissionID)
}

// ptrQuantity returns a pointer to a parsed resource.Quantity. EmptyDir's
// SizeLimit takes a *resource.Quantity, hence the indirection.
func ptrQuantity(v string) *resource.Quantity {
	q := resource.MustParse(v)
	return &q
}

// boolLabel renders a Go bool as the Kubernetes-canonical "true"/"false"
// string used in label values (where it's matched against selectors).
func boolLabel(b bool) string {
	if b {
		return "true"
	}
	return "false"
}

func (s *gvisorSandbox) Teardown(ctx context.Context, submissionID string) error {
	if s.cfg.K8sClient == nil {
		return fmt.Errorf("sandbox not configured")
	}
	if submissionID == "" {
		return fmt.Errorf("Teardown missing submissionID")
	}
	podName := podNameFor(submissionID)
	gracePeriod := int64(5)
	err := s.cfg.K8sClient.CoreV1().Pods(s.cfg.Namespace).Delete(ctx, podName,
		metav1.DeleteOptions{GracePeriodSeconds: &gracePeriod})
	if err != nil && !apierrors.IsNotFound(err) {
		return fmt.Errorf("delete pod %s: %w", podName, err)
	}
	return nil
}

