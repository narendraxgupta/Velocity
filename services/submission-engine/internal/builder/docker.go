// Docker builder backend — the local/dev counterpart to the Kaniko builder.
//
// Instead of spawning a Kaniko Job in Kubernetes, it pulls the artefact tar
// (a Dockerfile build context) from the presigned MinIO URL and hands it to a
// local Docker daemon over the mounted socket. This is what powers
// `make sample-submit` inside a Codespace where there is no cluster.
package builder

import (
	"context"
	"fmt"
	"io"
	"net/http"
	"strings"
	"time"

	"go.uber.org/zap"

	"github.com/velocity/platform/services/submission-engine/internal/dockerengine"
)

// DockerConfig configures the local Docker builder.
type DockerConfig struct {
	Engine *dockerengine.Client
	Logger *zap.SugaredLogger

	// HTTPClient fetches the presigned artefact. Defaults to a 5-minute client.
	HTTPClient *http.Client
}

// NewDocker returns a Builder backed by a local Docker daemon.
func NewDocker(cfg DockerConfig) Builder {
	if cfg.HTTPClient == nil {
		cfg.HTTPClient = &http.Client{Timeout: 5 * time.Minute}
	}
	return &dockerBuilder{cfg: cfg}
}

type dockerBuilder struct {
	cfg DockerConfig
}

// dockerImageRef is the deterministic local tag for a submission. The
// repository must be lowercase; the ULID submission id lives in the tag
// portion (which permits uppercase), so the sandbox can reference it directly.
func dockerImageRef(submissionID string) string {
	return "velocity-submission:" + submissionID
}

func (b *dockerBuilder) Build(ctx context.Context, req Request) (*Result, error) {
	if b.cfg.Engine == nil {
		return nil, fmt.Errorf("docker builder not configured with an engine client")
	}
	if req.ArtefactPresign == "" {
		return nil, fmt.Errorf("docker build requires a presigned artefact URL")
	}

	// Pull the build context (a tar with a Dockerfile at its root) into memory.
	// Submission contexts are small; buffering keeps the /build request simple
	// and retry-safe.
	tarball, err := b.fetchArtefact(ctx, req.ArtefactPresign)
	if err != nil {
		return nil, fmt.Errorf("fetch artefact: %w", err)
	}

	imageRef := dockerImageRef(req.SubmissionID)
	b.logf("docker build starting submission=%s image=%s bytes=%d",
		req.SubmissionID, imageRef, len(tarball))

	onLog := func(line string) {
		// Surface build output at debug so it doesn't flood info logs but is
		// available when chasing a failed submission build.
		b.logd("[build %s] %s", req.SubmissionID, line)
	}
	if err := b.cfg.Engine.BuildImage(ctx, imageRef, tarball, onLog); err != nil {
		return nil, fmt.Errorf("docker build: %w", err)
	}

	b.logf("docker build complete submission=%s image=%s", req.SubmissionID, imageRef)
	return &Result{ImageRef: imageRef, JobName: "docker-build-" + req.SubmissionID}, nil
}

func (b *dockerBuilder) fetchArtefact(ctx context.Context, url string) ([]byte, error) {
	httpReq, err := http.NewRequestWithContext(ctx, http.MethodGet, url, nil)
	if err != nil {
		return nil, err
	}
	resp, err := b.cfg.HTTPClient.Do(httpReq)
	if err != nil {
		return nil, err
	}
	defer resp.Body.Close()
	if resp.StatusCode != http.StatusOK {
		body, _ := io.ReadAll(io.LimitReader(resp.Body, 8*1024))
		return nil, fmt.Errorf("artefact GET %s: %s", resp.Status, strings.TrimSpace(string(body)))
	}
	// Guard against a pathological artefact (256 MiB ceiling).
	return io.ReadAll(io.LimitReader(resp.Body, 256*1024*1024))
}

func (b *dockerBuilder) logf(format string, args ...any) {
	if b.cfg.Logger != nil {
		b.cfg.Logger.Infof(format, args...)
	}
}

func (b *dockerBuilder) logd(format string, args ...any) {
	if b.cfg.Logger != nil {
		b.cfg.Logger.Debugf(format, args...)
	}
}
