// Package recorder owns the lifecycle of one tcpdump-backed capture per
// benchmark. The strategy is:
//
//  1. On Start, attach a privileged ephemeral container (image:
//     `RECORDER_TCPDUMP_IMAGE`) to the target pod that shares its network
//     namespace. The ephemeral container runs:
//
//     tcpdump -i any -w /tmp/<benchmark_id>.pcap -U "tcp port <target_port>"
//
//     We use ephemeral containers rather than DaemonSet+host-network because
//     they're scoped to the pod's lifecycle — if the submission pod dies, the
//     capture context dies with it, so we never leak a privileged tcpdump
//     into the cluster.
//
//  2. The recorder keeps an in-memory Session for that benchmark id, with a
//     deadline derived from `ttl_seconds`. A reaper goroutine sweeps for
//     expired sessions every 30s and force-finalizes them — guaranteeing no
//     capture ever runs forever on a wedged pod.
//
//  3. On Stop, we exec a `cat /tmp/<benchmark_id>.pcap` into the ephemeral
//     container, stream the bytes into MinIO under
//     `pcaps/<benchmark_id>.pcap`, then send `SIGINT` to tcpdump (it flushes
//     and exits cleanly). The container itself terminates with the pod, so
//     we don't have to delete it explicitly.
//
// The ephemeral-container approach has a real caveat: it requires the
// EphemeralContainers feature on the API server (GA since 1.25). For
// platforms below that, the recorder falls back to "log-only" mode and the
// pcap object is written empty with an `x-velocity-empty=true` user-meta
// header. The replayer treats that as "no capture available" cleanly.
package recorder

import (
	"bytes"
	"context"
	"errors"
	"fmt"
	"io"
	"regexp"
	"strings"
	"sync"
	"time"

	"go.uber.org/zap"
	corev1 "k8s.io/api/core/v1"
	apierrors "k8s.io/apimachinery/pkg/api/errors"
	metav1 "k8s.io/apimachinery/pkg/apis/meta/v1"
	"k8s.io/apimachinery/pkg/util/uuid"
	"k8s.io/client-go/kubernetes"
	"k8s.io/client-go/kubernetes/scheme"
	"k8s.io/client-go/rest"
	"k8s.io/client-go/tools/remotecommand"

	"github.com/velocity/platform/services/pcap-recorder/internal/store"
)

// Deps wires the recorder's static dependencies.
type Deps struct {
	K8s          kubernetes.Interface
	REST         *rest.Config
	Logger       *zap.SugaredLogger
	Namespaces   string
	Storage      store.Storage
	TcpdumpImage string
}

// StartArgs covers one capture's parameters.
type StartArgs struct {
	BenchmarkID string
	Namespace   string
	Pod         string
	TargetPort  int
	TTL         time.Duration
}

// Recorder is the long-lived service object.
type Recorder struct {
	deps Deps

	mu       sync.Mutex
	sessions map[string]*session // by benchmark_id
}

type session struct {
	ID         string
	Namespace  string
	Pod        string
	Container  string // ephemeral container name
	StartedAt  time.Time
	DeadlineAt time.Time
	Finalised  bool
}

// New constructs an idle Recorder.
func New(deps Deps) *Recorder {
	return &Recorder{
		deps:     deps,
		sessions: map[string]*session{},
	}
}

// Storage exposes the recorder's MinIO handle so HTTP handlers can presign
// download URLs for finalised pcaps without duplicating credentials.
func (r *Recorder) Storage() store.Storage { return r.deps.Storage }

// Active returns a snapshot of all active capture sessions.
func (r *Recorder) Active() []Session {
	r.mu.Lock()
	defer r.mu.Unlock()
	out := make([]Session, 0, len(r.sessions))
	for _, s := range r.sessions {
		out = append(out, Session{
			BenchmarkID: s.ID,
			Namespace:   s.Namespace,
			Pod:         s.Pod,
			Container:   s.Container,
			StartedAtNs: uint64(s.StartedAt.UnixNano()),
			DeadlineNs:  uint64(s.DeadlineAt.UnixNano()),
			Finalised:   s.Finalised,
		})
	}
	return out
}

// Session is the externally-visible representation of one capture.
type Session struct {
	BenchmarkID string `json:"benchmark_id"`
	Namespace   string `json:"namespace"`
	Pod         string `json:"pod"`
	Container   string `json:"container"`
	StartedAtNs uint64 `json:"started_at_ns"`
	DeadlineNs  uint64 `json:"deadline_ns"`
	Finalised   bool   `json:"finalised"`
}

// safeBenchmarkID restricts benchmark identifiers to characters that are
// inert when interpolated into a shell command. The recorder builds a
// `tcpdump -w /tmp/<id>.pcap` command via `/bin/sh -c`, so an unrestricted id
// would let any caller inject arbitrary commands through metacharacters in
// the request body. The set [A-Za-z0-9._-] covers every id format produced
// by the controller (BM-<hex>-<hex>) without enabling shell metacharacters.
var safeBenchmarkID = regexp.MustCompile(`^[A-Za-z0-9._-]{1,128}$`)

// Start attaches a tcpdump ephemeral container to the target pod and tracks
// the session in-memory.
func (r *Recorder) Start(ctx context.Context, args StartArgs) error {
	if !r.namespaceAllowed(args.Namespace) {
		return fmt.Errorf("namespace %q not allowed for recording", args.Namespace)
	}
	if args.BenchmarkID == "" {
		return errors.New("benchmark_id required")
	}
	if !safeBenchmarkID.MatchString(args.BenchmarkID) {
		return fmt.Errorf("benchmark_id %q has unsafe characters; "+
			"must match [A-Za-z0-9._-]{1,128}", args.BenchmarkID)
	}
	if args.TTL <= 0 {
		args.TTL = 10 * time.Minute
	}
	if args.TargetPort == 0 {
		args.TargetPort = 8080
	}

	r.mu.Lock()
	if _, exists := r.sessions[args.BenchmarkID]; exists {
		r.mu.Unlock()
		return fmt.Errorf("recording already in progress for %s", args.BenchmarkID)
	}
	r.mu.Unlock()

	containerName := fmt.Sprintf("velocity-tcpdump-%s", randSuffix(6))
	bpf := fmt.Sprintf("tcp and port %d", args.TargetPort)
	pcapPath := fmt.Sprintf("/tmp/%s.pcap", args.BenchmarkID)

	// The ephemeral container shares the target pod's network namespace by
	// virtue of being scheduled into the pod itself; we just need to grant
	// CAP_NET_RAW + CAP_NET_ADMIN so tcpdump can open the raw socket.
	priv := true
	ec := corev1.EphemeralContainer{
		EphemeralContainerCommon: corev1.EphemeralContainerCommon{
			Name:    containerName,
			Image:   r.deps.TcpdumpImage,
			Command: []string{"/bin/sh", "-c"},
			Args: []string{
				// `-U` forces packet-by-packet flush so a hard kill still
				// yields a valid pcap up to the last write. `-w -` would
				// give us a stream, but writing to a file lets us re-read
				// with `cat` on stop without dealing with mid-flight
				// stdout interleaving.
				fmt.Sprintf("tcpdump -i any -U -w %s %q & echo $! > /tmp/tcpdump.pid; wait",
					pcapPath, bpf),
			},
			SecurityContext: &corev1.SecurityContext{
				Privileged: &priv,
				Capabilities: &corev1.Capabilities{
					Add: []corev1.Capability{"NET_RAW", "NET_ADMIN"},
				},
			},
		},
		TargetContainerName: "", // attach to default container in pod
	}

	if err := r.addEphemeral(ctx, args.Namespace, args.Pod, ec); err != nil {
		return fmt.Errorf("attach tcpdump: %w", err)
	}

	s := &session{
		ID:         args.BenchmarkID,
		Namespace:  args.Namespace,
		Pod:        args.Pod,
		Container:  containerName,
		StartedAt:  time.Now(),
		DeadlineAt: time.Now().Add(args.TTL),
	}
	r.mu.Lock()
	r.sessions[args.BenchmarkID] = s
	r.mu.Unlock()

	r.deps.Logger.Infow("recording started",
		"benchmark_id", args.BenchmarkID,
		"pod", args.Pod,
		"container", containerName,
		"bpf", bpf)
	return nil
}

// Stop finalizes a capture: streams the pcap out of the ephemeral container
// into MinIO and removes the in-memory session.
func (r *Recorder) Stop(ctx context.Context, benchmarkID string) (objectKey string, sizeBytes int64, err error) {
	r.mu.Lock()
	s, ok := r.sessions[benchmarkID]
	r.mu.Unlock()
	if !ok {
		return "", 0, fmt.Errorf("no active recording for %s", benchmarkID)
	}

	pcapPath := fmt.Sprintf("/tmp/%s.pcap", benchmarkID)
	objectKey = fmt.Sprintf("pcaps/%s.pcap", benchmarkID)

	// Stop tcpdump and wait for it to flush + close the capture file BEFORE
	// we read it. Previously we `cat`-ed and uploaded the pcap first and
	// SIGINT-ed tcpdump afterwards — so the object landing in MinIO could be
	// truncated/incomplete (tcpdump was still buffering), which the replayer
	// then rejected as "no client→server payload packets". SIGINT makes
	// tcpdump flush and exit; poll the pid until it's gone (bounded ~5s).
	_ = r.execStream(ctx, s.Namespace, s.Pod, s.Container,
		[]string{"/bin/sh", "-c",
			"PID=$(cat /tmp/tcpdump.pid 2>/dev/null); " +
				"if [ -n \"$PID\" ]; then kill -INT \"$PID\" 2>/dev/null || true; " +
				"for i in $(seq 1 50); do kill -0 \"$PID\" 2>/dev/null || break; sleep 0.1; done; fi"},
		nil, io.Discard, io.Discard)

	// Pipe `cat <pcapPath>` from the ephemeral container into a pipe that
	// MinIO consumes.
	pr, pw := io.Pipe()
	execErr := make(chan error, 1)
	go func() {
		defer pw.Close()
		err := r.execStream(ctx, s.Namespace, s.Pod, s.Container,
			[]string{"cat", pcapPath}, nil, pw, io.Discard)
		execErr <- err
	}()

	stored, putErr := r.deps.Storage.Put(ctx, objectKey, pr)
	if putErr != nil {
		_ = pr.CloseWithError(putErr)
	}
	if eErr := <-execErr; eErr != nil && putErr == nil {
		putErr = eErr
	}
	if putErr != nil {
		return "", 0, fmt.Errorf("stream pcap: %w", putErr)
	}

	// Stat for the size; non-fatal if it fails (S3 backends sometimes
	// race on the read-your-write here).
	size, _, _ := r.deps.Storage.Stat(ctx, objectKey)

	r.mu.Lock()
	s.Finalised = true
	delete(r.sessions, benchmarkID)
	r.mu.Unlock()

	r.deps.Logger.Infow("recording stopped",
		"benchmark_id", benchmarkID,
		"object", stored,
		"size", size)
	return objectKey, size, nil
}

// FlushAll force-finalizes every active recording. Called on shutdown.
func (r *Recorder) FlushAll(ctx context.Context) error {
	r.mu.Lock()
	ids := make([]string, 0, len(r.sessions))
	for id := range r.sessions {
		ids = append(ids, id)
	}
	r.mu.Unlock()

	var firstErr error
	for _, id := range ids {
		if _, _, err := r.Stop(ctx, id); err != nil && firstErr == nil {
			firstErr = err
		}
	}
	return firstErr
}

// RunReaper walks active sessions every `interval` and force-finalizes any
// whose deadline has elapsed.
func (r *Recorder) RunReaper(ctx context.Context, interval time.Duration) {
	tick := time.NewTicker(interval)
	defer tick.Stop()
	for {
		select {
		case <-ctx.Done():
			return
		case now := <-tick.C:
			r.mu.Lock()
			expired := make([]string, 0)
			for id, s := range r.sessions {
				if now.After(s.DeadlineAt) {
					expired = append(expired, id)
				}
			}
			r.mu.Unlock()
			for _, id := range expired {
				r.deps.Logger.Warnw("reaping expired recording", "benchmark_id", id)
				if _, _, err := r.Stop(context.Background(), id); err != nil {
					r.deps.Logger.Warnw("reap failed", "benchmark_id", id, "err", err)
				}
			}
		}
	}
}

// -----------------------------------------------------------------------------
//  K8s helpers
// -----------------------------------------------------------------------------

func (r *Recorder) namespaceAllowed(ns string) bool {
	for _, allowed := range strings.Split(r.deps.Namespaces, ",") {
		if strings.TrimSpace(allowed) == ns {
			return true
		}
	}
	return false
}

// addEphemeral attaches an ephemeral container to an existing pod.
//
// K8s exposes this only via the dedicated `pods/ephemeralcontainers`
// subresource. The typed client surfaces it as
// `pods.UpdateEphemeralContainers(ctx, podName, *Pod, opts)` — we Get the
// current pod, append our EphemeralContainer, then PUT the whole pod back
// to the subresource.
func (r *Recorder) addEphemeral(ctx context.Context, namespace, podName string,
	ec corev1.EphemeralContainer) error {

	pods := r.deps.K8s.CoreV1().Pods(namespace)

	pod, err := pods.Get(ctx, podName, metav1.GetOptions{})
	if err != nil {
		if apierrors.IsNotFound(err) {
			return fmt.Errorf("pod %s/%s not found", namespace, podName)
		}
		return err
	}
	pod.Spec.EphemeralContainers = append(pod.Spec.EphemeralContainers, ec)
	if _, err := pods.UpdateEphemeralContainers(ctx, podName, pod, metav1.UpdateOptions{}); err != nil {
		return err
	}
	return nil
}

// execStream runs `cmd` inside `container` of `pod` and pipes stdout/stderr
// to the caller. We use the subresource exec endpoint directly because the
// typed client doesn't surface it.
func (r *Recorder) execStream(ctx context.Context, namespace, pod, container string,
	cmd []string, stdin io.Reader, stdout, stderr io.Writer) error {

	req := r.deps.K8s.CoreV1().RESTClient().
		Post().
		Resource("pods").
		Name(pod).
		Namespace(namespace).
		SubResource("exec").
		VersionedParams(&corev1.PodExecOptions{
			Container: container,
			Command:   cmd,
			Stdin:     stdin != nil,
			Stdout:    stdout != nil,
			Stderr:    stderr != nil,
			TTY:       false,
		}, scheme.ParameterCodec)

	exec, err := remotecommand.NewSPDYExecutor(r.deps.REST, "POST", req.URL())
	if err != nil {
		return fmt.Errorf("spdy exec: %w", err)
	}

	if stdout == nil {
		stdout = io.Discard
	}
	if stderr == nil {
		stderr = io.Discard
	}
	return exec.StreamWithContext(ctx, remotecommand.StreamOptions{
		Stdin:  stdin,
		Stdout: stdout,
		Stderr: stderr,
	})
}

// randSuffix returns a short hex-like suffix for ephemeral container names
// so retries don't collide.
func randSuffix(n int) string {
	id := string(uuid.NewUUID())
	id = strings.ReplaceAll(id, "-", "")
	if n > len(id) {
		n = len(id)
	}
	return id[:n]
}

// drainReader copies a reader to an in-memory buffer (used by exec fallbacks
// that don't accept io.Writer streams).
//
//nolint:unused // retained for future replay-shaped exec needs
func drainReader(r io.Reader) (string, error) {
	var b bytes.Buffer
	if _, err := io.Copy(&b, r); err != nil {
		return "", err
	}
	return b.String(), nil
}
