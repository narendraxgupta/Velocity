// Package server — gRPC SubmissionService implementation.
//
// The handlers compose three pure-ish dependencies (storage, builder,
// sandbox) into the lifecycle the rest of the platform expects. They are
// intentionally thin: anything non-trivial is pushed into the relevant
// package.
package server

import (
	"context"
	"crypto/sha256"
	"encoding/hex"
	"encoding/json"
	"errors"
	"fmt"
	"io"
	"strings"
	"sync"
	"time"

	"github.com/oklog/ulid/v2"
	"go.uber.org/zap"

	commonv1 "github.com/velocity/platform/proto/gen/go/common/v1"
	pb "github.com/velocity/platform/proto/gen/go/orchestrator/v1"

	"github.com/velocity/platform/services/submission-engine/internal/builder"
	"github.com/velocity/platform/services/submission-engine/internal/sandbox"
	"github.com/velocity/platform/services/submission-engine/internal/storage"
)

// maxUploadBytes caps a single artefact upload. Without this the Upload
// stream was unbounded — a client could exhaust submission-engine memory /
// MinIO disk. 1 GiB comfortably covers source tarballs and binaries while
// still being a hard DoS ceiling.
const maxUploadBytes int64 = 1 << 30 // 1 GiB

// SubmissionHandlers implements pb.SubmissionServiceServer.
type SubmissionHandlers struct {
	pb.UnimplementedSubmissionServiceServer

	storage storage.Storage
	builder builder.Builder
	sandbox sandbox.Sandbox
	logger  *zap.SugaredLogger

	// In-memory submission registry. Phase 3 swaps this for Postgres.
	mu          sync.RWMutex
	submissions map[string]*submissionRecord
	watchers    map[string][]chan *pb.SubmissionStatus
}

type submissionRecord struct {
	id          string
	displayName string
	teamName    string
	artefactKey string
	sha256      string
	kind        pb.ArtefactKind
	phase       pb.SubmissionPhase
	imageRef    string
	endpoint    *sandbox.Endpoint
	podName     string
	createdAt   time.Time
}

// NewHandlers constructs the handler struct. All deps must be non-nil.
func NewHandlers(s storage.Storage, b builder.Builder, sb sandbox.Sandbox,
	logger *zap.SugaredLogger) *SubmissionHandlers {
	return &SubmissionHandlers{
		storage:     s,
		builder:     b,
		sandbox:     sb,
		logger:      logger,
		submissions: map[string]*submissionRecord{},
		watchers:    map[string][]chan *pb.SubmissionStatus{},
	}
}

// -----------------------------------------------------------------------------
//
//	Upload — streaming inbound artefact -> MinIO.
//
// -----------------------------------------------------------------------------
//
// Concurrency contract:
//
//   - The recv goroutine reads gRPC frames and pumps bytes into a pipe.
//   - storage.Put reads the pipe.
//   - A buffered errCh transports the recv goroutine's terminal status to
//     the main handler so we can return a meaningful gRPC status.
//
// We MUST ensure that, on every exit path, the goroutine has terminated
// before this function returns — otherwise it would keep calling
// stream.Recv() on a stream gRPC is about to close, leaking memory and
// potentially blocking storage layers behind it. We achieve that by
// always closing the pipe writer (which unblocks the storage.Put reader)
// and always reading errCh, regardless of which side fails first.
func (h *SubmissionHandlers) Upload(stream pb.SubmissionService_UploadServer) error {
	ctx := stream.Context()

	first, err := stream.Recv()
	if err != nil {
		return fmt.Errorf("recv header: %w", err)
	}
	hdr := first.GetHeader()
	if hdr == nil {
		return errors.New("first upload message must be header")
	}
	if hdr.Filename == "" {
		return errors.New("upload header missing filename")
	}

	submissionID := ulid.Make().String()
	cleanName := sanitizeFilename(hdr.Filename)
	objectKey := fmt.Sprintf("submissions/%s/%s", submissionID, cleanName)

	pr, pw := io.Pipe()
	hasher := sha256.New()
	var received int64

	errCh := make(chan error, 1)
	go func() {
		var finalErr error
		defer func() {
			// Always close the write end so storage.Put unblocks. If we
			// hit a recv error, propagate it through the pipe so the
			// reader sees a real failure rather than a clean EOF.
			if finalErr != nil {
				_ = pw.CloseWithError(finalErr)
			} else {
				_ = pw.Close()
			}
			errCh <- finalErr
			close(errCh)
		}()
		for {
			msg, recvErr := stream.Recv()
			if errors.Is(recvErr, io.EOF) {
				return
			}
			if recvErr != nil {
				finalErr = recvErr
				return
			}
			chunk := msg.GetChunk()
			if chunk == nil {
				continue
			}
			data := chunk.GetData()
			if received+int64(len(data)) > maxUploadBytes {
				finalErr = fmt.Errorf("artefact exceeds max upload size of %d bytes", maxUploadBytes)
				return
			}
			if _, werr := pw.Write(data); werr != nil {
				finalErr = werr
				return
			}
			hasher.Write(data)
			received += int64(len(data))
		}
	}()

	// minio.Put with size = -1 indicates streaming put. If Put fails,
	// closing pr unblocks the goroutine's next pw.Write so it can exit.
	_, putErr := h.storage.Put(ctx, objectKey, pr, -1, storage.PutOptions{
		ContentType: "application/octet-stream",
		UserMeta: map[string]string{
			"submission-id": submissionID,
			"team":          hdr.TeamName,
			"display":       hdr.DisplayName,
		},
	})
	if putErr != nil {
		// Force the goroutine to exit by closing the reader side.
		_ = pr.CloseWithError(putErr)
	}

	// Always drain errCh so the goroutine has fully terminated.
	recvErr := <-errCh

	if putErr != nil {
		return fmt.Errorf("storage.Put: %w", putErr)
	}
	if recvErr != nil {
		return fmt.Errorf("stream recv: %w", recvErr)
	}

	sum := hex.EncodeToString(hasher.Sum(nil))
	h.logger.Infow("artefact uploaded",
		"submission_id", submissionID,
		"key", objectKey,
		"bytes", received,
		"sha256", sum)

	return stream.SendAndClose(&pb.UploadResponse{
		ArtefactObjectKey: objectKey,
		Sha256:            sum,
		ReceivedBytes:     uint64(received),
	})
}

// sanitizeFilename strips path separators and other characters that
// would make the object key ambiguous or escape the submission's
// MinIO prefix.
func sanitizeFilename(name string) string {
	if i := strings.LastIndexAny(name, "/\\"); i >= 0 {
		name = name[i+1:]
	}
	name = strings.TrimSpace(name)
	if name == "" || name == "." || name == ".." {
		return "artefact.bin"
	}
	// Replace anything not in [A-Za-z0-9._-] with '_'.
	out := make([]byte, 0, len(name))
	for i := 0; i < len(name); i++ {
		c := name[i]
		switch {
		case c >= 'A' && c <= 'Z',
			c >= 'a' && c <= 'z',
			c >= '0' && c <= '9',
			c == '.' || c == '_' || c == '-':
			out = append(out, c)
		default:
			out = append(out, '_')
		}
	}
	return string(out)
}

// -----------------------------------------------------------------------------
//
//	Register
//
// -----------------------------------------------------------------------------
func (h *SubmissionHandlers) Register(_ context.Context, req *pb.RegisterRequest) (
	*pb.RegisterResponse, error) {

	// Derive a submission_id from the artefact key (which already contains
	// the ULID minted at upload time) so Upload→Register is idempotent.
	id := ""
	if strings.HasPrefix(req.GetArtefactObjectKey(), "submissions/") {
		parts := strings.SplitN(strings.TrimPrefix(req.GetArtefactObjectKey(),
			"submissions/"), "/", 2)
		if len(parts) > 0 {
			id = parts[0]
		}
	}
	if id == "" {
		id = ulid.Make().String()
	}

	rec := &submissionRecord{
		id:          id,
		displayName: req.GetDisplayName(),
		teamName:    req.GetTeamName(),
		artefactKey: req.GetArtefactObjectKey(),
		sha256:      req.GetArtefactSha256(),
		kind:        req.GetKind(),
		phase:       pb.SubmissionPhase_SUBMISSION_PHASE_REGISTERED,
		createdAt:   time.Now(),
	}

	h.mu.Lock()
	h.submissions[id] = rec
	h.mu.Unlock()
	h.broadcast(id, pb.SubmissionPhase_SUBMISSION_PHASE_REGISTERED, "registered")

	h.logger.Infow("submission registered", "id", id, "team", rec.teamName)
	return &pb.RegisterResponse{
		SubmissionId: &commonv1.SubmissionId{Value: id},
	}, nil
}

// -----------------------------------------------------------------------------
//
//	Build
//
// -----------------------------------------------------------------------------
func (h *SubmissionHandlers) Build(ctx context.Context, req *pb.BuildRequest) (
	*pb.BuildResponse, error) {
	id := req.GetSubmissionId().GetValue()
	rec := h.get(id)
	if rec == nil {
		return nil, fmt.Errorf("unknown submission %q", id)
	}

	// PCAP_REPLAY submissions have no container artefact — there's nothing
	// to build. Mark them BUILT immediately so the gateway's standard
	// Upload→Register→Build→Deploy→Run dance still works end-to-end.
	// The "image_ref" we return is a sentinel that Deploy will recognise
	// and route to the pcap-replayer service instead of the sandbox.
	if rec.kind == pb.ArtefactKind_ARTEFACT_KIND_PCAP_REPLAY {
		sentinel := "pcap-replay://" + rec.artefactKey
		h.mu.Lock()
		rec.imageRef = sentinel
		rec.phase = pb.SubmissionPhase_SUBMISSION_PHASE_BUILT
		h.mu.Unlock()
		h.broadcast(id, pb.SubmissionPhase_SUBMISSION_PHASE_BUILT,
			"pcap replay — no image build required")
		return &pb.BuildResponse{ImageRef: sentinel}, nil
	}

	// Presign the artefact so Kaniko can curl it.
	presigned, err := h.storage.PresignGet(ctx, rec.artefactKey, 30*60)
	if err != nil {
		return nil, fmt.Errorf("presign: %w", err)
	}

	h.broadcast(id, pb.SubmissionPhase_SUBMISSION_PHASE_BUILDING, "kaniko started")
	res, err := h.builder.Build(ctx, builder.Request{
		SubmissionID:    id,
		ArtefactKey:     rec.artefactKey,
		ArtefactSHA:     rec.sha256,
		ArtefactPresign: presigned,
		BuildKind:       kindOf(rec.kind),
	})
	if err != nil {
		h.broadcast(id, pb.SubmissionPhase_SUBMISSION_PHASE_BUILD_FAILED, err.Error())
		return nil, err
	}

	h.mu.Lock()
	rec.imageRef = res.ImageRef
	rec.phase = pb.SubmissionPhase_SUBMISSION_PHASE_BUILT
	h.mu.Unlock()
	h.broadcast(id, pb.SubmissionPhase_SUBMISSION_PHASE_BUILT, res.ImageRef)

	return &pb.BuildResponse{ImageRef: res.ImageRef}, nil
}

// -----------------------------------------------------------------------------
//
//	Deploy
//
// -----------------------------------------------------------------------------
func (h *SubmissionHandlers) Deploy(ctx context.Context, req *pb.DeployRequest) (
	*pb.DeployResponse, error) {
	id := req.GetSubmissionId().GetValue()
	rec := h.get(id)
	if rec == nil {
		return nil, fmt.Errorf("unknown submission %q", id)
	}
	if rec.imageRef == "" {
		return nil, fmt.Errorf("submission %s has not been built", id)
	}

	// PCAP_REPLAY submissions don't deploy a sandbox — they re-fire bytes
	// at an existing engine. Surface a placeholder endpoint so the
	// gateway can wire `/v1/pcaps/replay` against this submission_id;
	// the actual target endpoint is the *other* submission the operator
	// picked at replay time, passed through on the StartReplay body.
	if rec.kind == pb.ArtefactKind_ARTEFACT_KIND_PCAP_REPLAY {
		h.mu.Lock()
		rec.endpoint = &sandbox.Endpoint{Host: "pcap-replayer", Port: 8092, Path: "/v1/pcaps/replay"}
		rec.podName = "pcap-replay-" + id
		rec.phase = pb.SubmissionPhase_SUBMISSION_PHASE_HEALTHY
		h.mu.Unlock()
		h.broadcast(id, pb.SubmissionPhase_SUBMISSION_PHASE_HEALTHY,
			"pcap replay submission ready")
		return &pb.DeployResponse{
			Endpoint: &commonv1.Endpoint{
				Host:     rec.endpoint.Host,
				Port:     uint32(rec.endpoint.Port),
				Protocol: commonv1.WireProtocol_WIRE_PROTOCOL_REST,
				Path:     rec.endpoint.Path,
			},
			PodName:   rec.podName,
			Namespace: "velocity-control",
		}, nil
	}

	h.broadcast(id, pb.SubmissionPhase_SUBMISSION_PHASE_DEPLOYING, "")
	lim := req.GetLimits()
	out, err := h.sandbox.Launch(ctx, sandbox.LaunchRequest{
		SubmissionID: id,
		ImageRef:     rec.imageRef,
		CPUCores:     lim.GetCpuCores(),
		MemoryMiB:    uint32(lim.GetMemBytes() / 1024 / 1024),
		Lifetime:     time.Duration(lim.GetLifetimeSeconds()) * time.Second,
	})
	if err != nil {
		h.broadcast(id, pb.SubmissionPhase_SUBMISSION_PHASE_UNHEALTHY, err.Error())
		return nil, err
	}

	h.mu.Lock()
	rec.endpoint = &out.Endpoint
	rec.podName = out.Pod
	rec.phase = pb.SubmissionPhase_SUBMISSION_PHASE_HEALTHY
	h.mu.Unlock()
	h.broadcast(id, pb.SubmissionPhase_SUBMISSION_PHASE_HEALTHY, out.Pod)

	return &pb.DeployResponse{
		Endpoint: &commonv1.Endpoint{
			Host:     out.Endpoint.Host,
			Port:     uint32(out.Endpoint.Port),
			Protocol: commonv1.WireProtocol_WIRE_PROTOCOL_REST,
			Path:     out.Endpoint.Path,
		},
		PodName:   out.Pod,
		Namespace: out.Namespace,
	}, nil
}

// -----------------------------------------------------------------------------
//
//	Teardown
//
// -----------------------------------------------------------------------------
func (h *SubmissionHandlers) Teardown(ctx context.Context, req *pb.TeardownRequest) (
	*pb.TeardownResponse, error) {
	id := req.GetSubmissionId().GetValue()
	if err := h.sandbox.Teardown(ctx, id); err != nil {
		return nil, err
	}
	if rec := h.get(id); rec != nil {
		h.mu.Lock()
		rec.phase = pb.SubmissionPhase_SUBMISSION_PHASE_TERMINATED
		h.mu.Unlock()
	}
	h.broadcast(id, pb.SubmissionPhase_SUBMISSION_PHASE_TERMINATED, "")
	return &pb.TeardownResponse{TeardownComplete: true}, nil
}

// -----------------------------------------------------------------------------
//
//	WatchSubmission — stream of SubmissionStatus
//
// -----------------------------------------------------------------------------
func (h *SubmissionHandlers) WatchSubmission(req *pb.WatchSubmissionRequest,
	stream pb.SubmissionService_WatchSubmissionServer) error {
	id := req.GetSubmissionId().GetValue()
	rec := h.get(id)

	// Emit the current snapshot immediately so the caller sees state even
	// if no further events ever fire.
	if rec != nil {
		_ = stream.Send(&pb.SubmissionStatus{
			SubmissionId: &commonv1.SubmissionId{Value: id},
			Phase:        rec.phase,
			Detail:       "current",
			TsNs:         uint64(time.Now().UnixNano()),
		})
	}

	ch := make(chan *pb.SubmissionStatus, 32)
	h.mu.Lock()
	h.watchers[id] = append(h.watchers[id], ch)
	h.mu.Unlock()

	defer func() {
		h.mu.Lock()
		watchers := h.watchers[id]
		for i, w := range watchers {
			if w == ch {
				h.watchers[id] = append(watchers[:i], watchers[i+1:]...)
				break
			}
		}
		h.mu.Unlock()
		close(ch)
	}()

	for {
		select {
		case <-stream.Context().Done():
			return stream.Context().Err()
		case st, ok := <-ch:
			if !ok {
				return nil
			}
			if err := stream.Send(st); err != nil {
				return err
			}
		}
	}
}

// -----------------------------------------------------------------------------
//
//	GetFlamegraph — read perf-profiler sidecar output from MinIO.
//
// -----------------------------------------------------------------------------
//
// The perf-profiler sidecar (see sandbox.go) writes folded-stack format to
//
//	flamegraphs/<submission_id>.folded.txt
//
// alongside a meta-JSON sibling carrying recording metadata. We stream the
// folded payload back to the gateway as a single GetFlamegraphResponse; the
// frontend uses d3-flamegraph to render an interactive SVG.
//
// Returns codes.NotFound when no flamegraph object exists for this submission
// (e.g. the profiler sidecar was disabled, the recording is still in
// progress, or this submission has never been deployed).
func (h *SubmissionHandlers) GetFlamegraph(ctx context.Context,
	req *pb.GetFlamegraphRequest) (*pb.GetFlamegraphResponse, error) {
	id := req.GetSubmissionId().GetValue()
	if id == "" {
		return nil, fmt.Errorf("GetFlamegraph: missing submission_id")
	}

	key := fmt.Sprintf("flamegraphs/%s.folded.txt", id)
	rd, err := h.storage.Get(ctx, key)
	if err != nil {
		return nil, fmt.Errorf("flamegraph object %s not found: %w", key, err)
	}
	defer rd.Close()

	// Cap reads at 16 MiB to keep a malicious or pathological recording from
	// OOM-ing the engine. A flamegraph this large is unreadable anyway.
	const maxBytes = 16 * 1024 * 1024
	buf, err := io.ReadAll(io.LimitReader(rd, maxBytes))
	if err != nil {
		return nil, fmt.Errorf("read flamegraph: %w", err)
	}

	// The sidecar also writes a sibling meta JSON. We do a best-effort read;
	// missing meta is non-fatal — the response just gets zero values.
	var recordedNs uint64
	var sampleHz uint32 = 99 // sidecar default
	var durSecs uint64
	if metaRd, mErr := h.storage.Get(ctx,
		fmt.Sprintf("flamegraphs/%s.meta.json", id)); mErr == nil {
		defer metaRd.Close()
		// Parse only the three fields we care about — keep the dep light.
		var meta struct {
			RecordedAtNs uint64 `json:"recorded_at_ns"`
			SampleFreqHz uint32 `json:"sample_freq_hz"`
			DurationSecs uint64 `json:"duration_seconds"`
		}
		if dec := json.NewDecoder(metaRd); dec != nil {
			_ = dec.Decode(&meta)
			recordedNs = meta.RecordedAtNs
			if meta.SampleFreqHz > 0 {
				sampleHz = meta.SampleFreqHz
			}
			durSecs = meta.DurationSecs
		}
	}

	return &pb.GetFlamegraphResponse{
		Folded:          string(buf),
		RecordedAtNs:    recordedNs,
		SampleFreqHz:    sampleHz,
		DurationSeconds: durSecs,
	}, nil
}

// -----------------------------------------------------------------------------
//
//	Helpers
//
// -----------------------------------------------------------------------------
func (h *SubmissionHandlers) get(id string) *submissionRecord {
	h.mu.RLock()
	defer h.mu.RUnlock()
	return h.submissions[id]
}

func (h *SubmissionHandlers) broadcast(id string, phase pb.SubmissionPhase, detail string) {
	msg := &pb.SubmissionStatus{
		SubmissionId: &commonv1.SubmissionId{Value: id},
		Phase:        phase,
		Detail:       detail,
		TsNs:         uint64(time.Now().UnixNano()),
	}
	h.mu.RLock()
	defer h.mu.RUnlock()
	for _, ch := range h.watchers[id] {
		select {
		case ch <- msg:
		default:
			// drop on full — watcher is too slow
		}
	}
}

func kindOf(k pb.ArtefactKind) builder.Kind {
	switch k {
	case pb.ArtefactKind_ARTEFACT_KIND_DOCKERFILE:
		return builder.KindDockerfile
	case pb.ArtefactKind_ARTEFACT_KIND_SOURCE_TAR:
		return builder.KindSourceTar
	case pb.ArtefactKind_ARTEFACT_KIND_BINARY:
		return builder.KindBinary
	case pb.ArtefactKind_ARTEFACT_KIND_OCI_IMAGE:
		return builder.KindOCIImage
	case pb.ArtefactKind_ARTEFACT_KIND_PCAP_REPLAY:
		// Never reaches a real builder — Build() short-circuits to
		// SUBMISSION_PHASE_BUILT before this lookup happens. Mapped to
		// Unspecified so a future caller's switch is total.
		return builder.KindUnspecified
	default:
		return builder.KindUnspecified
	}
}
