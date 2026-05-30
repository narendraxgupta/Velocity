// Docker sandbox backend — the local/dev counterpart to the gVisor (K8s) one.
//
// It runs the submission image as a plain container on the same user-defined
// Docker network the bot fleet lives on, and — crucially — gives the container
// the *exact* DNS name the bot-controller fabricates for a submission
// (`submission-<id>.sandbox.svc.cluster.local`). That lets the unmodified C++
// bot pipeline reach the sandbox by name, so `make sample-submit` works in a
// Codespace with no Kubernetes.
package sandbox

import (
	"context"
	"fmt"
	"net/http"
	"strings"
	"time"

	"go.uber.org/zap"

	"github.com/velocity/platform/services/submission-engine/internal/dockerengine"
)

// DockerConfig configures the local Docker sandbox.
type DockerConfig struct {
	Engine *dockerengine.Client
	Logger *zap.SugaredLogger

	// Network is the user-defined docker network the bot fleet is attached to
	// (e.g. "velocity-apps"). The sandbox container joins it so the bots can
	// resolve it by DNS alias.
	Network string

	// ServicePort is the port the submission listens on. The bot-controller
	// hardcodes 8080 for the fabricated endpoint, so this must match (8080).
	ServicePort uint16

	// ReadyTimeout bounds how long Launch waits for /healthz. Defaults to 90s.
	ReadyTimeout time.Duration
}

// NewDocker returns a Sandbox backed by a local Docker daemon.
func NewDocker(cfg DockerConfig) Sandbox {
	if cfg.Network == "" {
		cfg.Network = "velocity-apps"
	}
	if cfg.ServicePort == 0 {
		cfg.ServicePort = 8080
	}
	if cfg.ReadyTimeout == 0 {
		cfg.ReadyTimeout = 90 * time.Second
	}
	return &dockerSandbox{
		cfg:   cfg,
		httpc: &http.Client{Timeout: 3 * time.Second},
	}
}

type dockerSandbox struct {
	cfg   DockerConfig
	httpc *http.Client
}

// sandboxDNSName mirrors the name the bot-controller builds in
// benchmark_service.cpp: "submission-<id>.sandbox.svc.cluster.local".
func sandboxDNSName(submissionID string) string {
	return fmt.Sprintf("submission-%s.sandbox.svc.cluster.local", submissionID)
}

// containerName is the docker object name; lowercased for safety even though
// docker permits uppercase, so cleanup/inspect is predictable.
func containerName(submissionID string) string {
	return "velocity-sub-" + strings.ToLower(submissionID)
}

func (s *dockerSandbox) Launch(ctx context.Context, req LaunchRequest) (*LaunchResult, error) {
	if s.cfg.Engine == nil {
		return nil, fmt.Errorf("docker sandbox not configured with an engine client")
	}
	if req.SubmissionID == "" {
		return nil, fmt.Errorf("LaunchRequest missing SubmissionID")
	}

	name := containerName(req.SubmissionID)
	dns := sandboxDNSName(req.SubmissionID)
	port := s.cfg.ServicePort

	// Idempotency: clear any stale container from a previous run before create.
	_ = s.cfg.Engine.RemoveContainer(ctx, name)

	spec := dockerengine.CreateSpec{
		Name:  name,
		Image: req.ImageRef,
		// Tell the submission which port to bind. The bundled sample exchange
		// reads EXCHANGE_PORT; harmless for images that ignore it.
		Env:         []string{fmt.Sprintf("EXCHANGE_PORT=%d", port)},
		ServicePort: port,
		Network:     s.cfg.Network,
		// Both the k8s-style FQDN (what the bots query) and the short name.
		Aliases:     []string{dns, name},
		NanoCPUs:    int64(req.CPUCores) * 1_000_000_000,
		MemoryBytes: int64(req.MemoryMiB) * 1024 * 1024,
	}

	id, err := s.cfg.Engine.CreateContainer(ctx, spec)
	if err != nil {
		return nil, fmt.Errorf("create sandbox container: %w", err)
	}
	if err := s.cfg.Engine.StartContainer(ctx, id); err != nil {
		_ = s.cfg.Engine.RemoveContainer(ctx, name)
		return nil, fmt.Errorf("start sandbox container: %w", err)
	}

	s.logf("sandbox started submission=%s container=%s dns=%s:%d",
		req.SubmissionID, name, dns, port)

	if err := s.waitReady(ctx, name, dns, port); err != nil {
		_ = s.cfg.Engine.RemoveContainer(ctx, name)
		return nil, err
	}

	return &LaunchResult{
		Endpoint: Endpoint{
			Host: dns,
			Port: port,
			Path: "/orders",
		},
		Pod:       name,
		Namespace: "docker",
	}, nil
}

// waitReady polls the submission's /healthz (reachable because the
// submission-engine shares the network) until it answers 2xx, the container
// dies, or the deadline passes.
func (s *dockerSandbox) waitReady(ctx context.Context, name, dns string, port uint16) error {
	deadline := time.Now().Add(s.cfg.ReadyTimeout)
	probeURL := fmt.Sprintf("http://%s:%d/healthz", dns, port)

	ticker := time.NewTicker(time.Second)
	defer ticker.Stop()

	for {
		// Fail fast if the container has already exited.
		if st, err := s.cfg.Engine.InspectState(ctx, name); err == nil && !st.Running {
			reason := st.Error
			if st.OOMKilled {
				reason = "OOMKilled"
			}
			return fmt.Errorf("sandbox container %s exited (code=%d%s) before becoming ready",
				name, st.ExitCode, suffix(reason))
		}

		if s.probe(ctx, probeURL) {
			return nil
		}

		if time.Now().After(deadline) {
			return fmt.Errorf("sandbox %s never became ready at %s", name, probeURL)
		}
		select {
		case <-ctx.Done():
			return ctx.Err()
		case <-ticker.C:
		}
	}
}

func (s *dockerSandbox) probe(ctx context.Context, url string) bool {
	req, err := http.NewRequestWithContext(ctx, http.MethodGet, url, nil)
	if err != nil {
		return false
	}
	resp, err := s.httpc.Do(req)
	if err != nil {
		return false
	}
	defer resp.Body.Close()
	return resp.StatusCode >= 200 && resp.StatusCode < 300
}

func (s *dockerSandbox) Teardown(ctx context.Context, submissionID string) error {
	if s.cfg.Engine == nil {
		return fmt.Errorf("docker sandbox not configured")
	}
	if submissionID == "" {
		return fmt.Errorf("Teardown missing submissionID")
	}
	if err := s.cfg.Engine.RemoveContainer(ctx, containerName(submissionID)); err != nil {
		return fmt.Errorf("remove sandbox container: %w", err)
	}
	return nil
}

func (s *dockerSandbox) logf(format string, args ...any) {
	if s.cfg.Logger != nil {
		s.cfg.Logger.Infof(format, args...)
	}
}

func suffix(reason string) string {
	if reason == "" {
		return ""
	}
	return ", reason=" + reason
}
