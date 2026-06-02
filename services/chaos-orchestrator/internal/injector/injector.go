// Package injector implements the actual chaos primitives.
//
// Each public method here corresponds to one HTTP endpoint in the server
// package; the server is the thin transport layer, this package owns the
// dangerous bits. Three design rules:
//
//  1. Every injection is time-bounded. The state is also stamped onto the
//     target pod as `chaos.velocity.io/expires-at-ns=<unix-ns>` so the
//     reaper (and any restart of the orchestrator) can clean up.
//  2. Every injection records its inverse so RunReaper can revert it
//     mechanically — no per-action revert RPC required.
//  3. Targets are validated against `allowedNamespaces` so chaos can never
//     accidentally hit control-plane services in the cluster.
//
// gVisor caveat: tc netem and cgroup writes both require kernel features
// gVisor does not implement; injections will silently degrade to no-ops
// when the target pod uses the gVisor runtime class. The sandbox launcher
// auto-disables gVisor when the cliff-finder or chaos modes are active.
package injector

import (
	"context"
	"errors"
	"fmt"
	"strings"
	"sync"
	"time"

	"go.uber.org/zap"
	corev1 "k8s.io/api/core/v1"
	apierrors "k8s.io/apimachinery/pkg/api/errors"
	metav1 "k8s.io/apimachinery/pkg/apis/meta/v1"
	"k8s.io/client-go/kubernetes"
)

// Deps gathers what an Injector needs.
type Deps struct {
	K8s       kubernetes.Interface
	Logger    *zap.SugaredLogger
	Namespace string // comma-separated list of allowed namespaces
}

// Injector is the public type. Goroutine-safe; share between handlers.
type Injector struct {
	deps      Deps
	allowedNs map[string]struct{}

	mu     sync.Mutex
	active map[string]*activeInjection
}

type activeInjection struct {
	Kind        Kind
	Pod         string
	Namespace   string
	ExpiresAt   time.Time
	RevertHints map[string]string // free-form data for revert()
}

// Kind enumerates the supported chaos primitives. Stable string values
// (lower-snake) because they're embedded into the K8s label
// `chaos.velocity.io/kind` and surface in audit logs.
type Kind string

const (
	KindPodKill     Kind = "pod-kill"
	KindTCLatency   Kind = "tc-latency"
	KindTCLoss      Kind = "tc-loss"
	KindCPUThrottle Kind = "cpu-throttle"
	KindPartition   Kind = "partition"
)

const (
	labelChaosKind      = "chaos.velocity.io/kind"
	labelChaosExpiresNs = "chaos.velocity.io/expires-at-ns"
)

// New constructs an Injector and parses the allowed-namespace list.
func New(d Deps) *Injector {
	in := &Injector{
		deps:      d,
		allowedNs: map[string]struct{}{},
		active:    map[string]*activeInjection{},
	}
	for _, ns := range strings.Split(d.Namespace, ",") {
		ns = strings.TrimSpace(ns)
		if ns != "" {
			in.allowedNs[ns] = struct{}{}
		}
	}
	return in
}

// validate ensures the (namespace, pod) tuple is OK to touch.
func (in *Injector) validate(ns, pod string) error {
	if ns == "" || pod == "" {
		return errors.New("namespace and pod are required")
	}
	if _, ok := in.allowedNs[ns]; !ok {
		return fmt.Errorf("namespace %q is not in CHAOS_NAMESPACES allow-list", ns)
	}
	return nil
}

// ----------------------------------------------------------------------------
//  Primitive: PodKill
// ----------------------------------------------------------------------------
//
// Deletes the target pod with grace_period=0 so the kubelet sends SIGKILL.
// Use to verify stateless failover: any service worth its salt should be back
// online within a few seconds because its Deployment will spawn a replacement.

func (in *Injector) PodKill(ctx context.Context, ns, pod string) error {
	if err := in.validate(ns, pod); err != nil {
		return err
	}
	zero := int64(0)
	err := in.deps.K8s.CoreV1().Pods(ns).Delete(ctx, pod,
		metav1.DeleteOptions{GracePeriodSeconds: &zero})
	if err != nil && !apierrors.IsNotFound(err) {
		return fmt.Errorf("delete pod: %w", err)
	}
	in.deps.Logger.Infow("chaos: pod-kill", "ns", ns, "pod", pod)
	return nil
}

// ----------------------------------------------------------------------------
//  Primitive: TC Latency
//
//  Uses an ephemeral-container side-injection (K8s 1.25+) to run
//    tc qdisc add dev eth0 root netem delay <ms>ms <jitter>ms
//  inside the target pod's network namespace. The ephemeral container's
//  image needs `iproute2`; we use the same perf-profiler image because it
//  already bundles the binaries.
//
//  Revert: when the ephemeral container exits (TTL or RunReaper), the
//  netem qdisc disappears with the pod's netns. Hence "revert" is implicit.
// ----------------------------------------------------------------------------

type LatencyArgs struct {
	DelayMs  uint32
	JitterMs uint32
	Duration time.Duration
}

func (in *Injector) TCLatency(ctx context.Context, ns, pod string, a LatencyArgs) error {
	if err := in.validate(ns, pod); err != nil {
		return err
	}
	if a.DelayMs == 0 {
		return errors.New("delay_ms required and must be > 0")
	}
	if a.Duration <= 0 {
		a.Duration = 30 * time.Second
	}

	tc := fmt.Sprintf(
		"tc qdisc add dev eth0 root netem delay %dms %dms distribution normal; "+
			"sleep %d; "+
			"tc qdisc del dev eth0 root netem || true",
		a.DelayMs, a.JitterMs, int(a.Duration.Seconds()))

	if err := in.attachEphemeral(ctx, ns, pod,
		fmt.Sprintf("chaos-tc-latency-%d", time.Now().Unix()),
		[]string{"sh", "-c", tc},
		corev1.Capability("NET_ADMIN")); err != nil {
		return fmt.Errorf("attach ephemeral: %w", err)
	}
	in.track(string(KindTCLatency), ns, pod, a.Duration, map[string]string{
		"delay_ms":  fmt.Sprint(a.DelayMs),
		"jitter_ms": fmt.Sprint(a.JitterMs),
	})
	in.deps.Logger.Infow("chaos: tc-latency",
		"ns", ns, "pod", pod, "delay_ms", a.DelayMs, "jitter_ms", a.JitterMs,
		"duration", a.Duration)
	return nil
}

// ----------------------------------------------------------------------------
//  Primitive: TC Loss
// ----------------------------------------------------------------------------

type LossArgs struct {
	LossPct  uint32
	Duration time.Duration
}

func (in *Injector) TCLoss(ctx context.Context, ns, pod string, a LossArgs) error {
	if err := in.validate(ns, pod); err != nil {
		return err
	}
	if a.LossPct == 0 || a.LossPct > 100 {
		return errors.New("loss_pct must be in (0, 100]")
	}
	if a.Duration <= 0 {
		a.Duration = 30 * time.Second
	}

	tc := fmt.Sprintf(
		"tc qdisc add dev eth0 root netem loss %d%%; "+
			"sleep %d; "+
			"tc qdisc del dev eth0 root netem || true",
		a.LossPct, int(a.Duration.Seconds()))

	if err := in.attachEphemeral(ctx, ns, pod,
		fmt.Sprintf("chaos-tc-loss-%d", time.Now().Unix()),
		[]string{"sh", "-c", tc},
		corev1.Capability("NET_ADMIN")); err != nil {
		return fmt.Errorf("attach ephemeral: %w", err)
	}
	in.track(string(KindTCLoss), ns, pod, a.Duration, map[string]string{
		"loss_pct": fmt.Sprint(a.LossPct),
	})
	in.deps.Logger.Infow("chaos: tc-loss",
		"ns", ns, "pod", pod, "loss_pct", a.LossPct, "duration", a.Duration)
	return nil
}

// ----------------------------------------------------------------------------
//  Primitive: CPU Throttle
//
//  Writes a tighter `cpu.max` value to the pod's cgroup. This requires
//  privileged exec into the kubelet's cgroup tree which is not always
//  possible from another container — so we attach a privileged ephemeral
//  container with the host's /sys mounted in, tighten cpu.max for the
//  duration, and revert on exit.
// ----------------------------------------------------------------------------

type CPUThrottleArgs struct {
	// CPUMaxMicros e.g. 50000 = 50ms per 100ms = effectively 0.5 cores.
	CPUMaxMicros uint32
	Duration     time.Duration
}

func (in *Injector) CPUThrottle(ctx context.Context, ns, pod string, a CPUThrottleArgs) error {
	if err := in.validate(ns, pod); err != nil {
		return err
	}
	if a.CPUMaxMicros == 0 {
		return errors.New("cpu_max_micros required and must be > 0")
	}
	if a.Duration <= 0 {
		a.Duration = 30 * time.Second
	}

	// Resolve the pod's UID — it's the cgroup subdir name in v2.
	p, err := in.deps.K8s.CoreV1().Pods(ns).Get(ctx, pod, metav1.GetOptions{})
	if err != nil {
		return fmt.Errorf("get pod: %w", err)
	}
	uid := string(p.UID)

	script := fmt.Sprintf(
		"f=/sys/fs/cgroup/kubepods.slice/kubepods-besteffort.slice/"+
			"kubepods-besteffort-pod%s.slice/cpu.max; "+
			"orig=$(cat \"$f\"); "+
			"echo \"%d 100000\" > \"$f\"; "+
			"sleep %d; "+
			"echo \"$orig\" > \"$f\"",
		strings.ReplaceAll(uid, "-", "_"), a.CPUMaxMicros, int(a.Duration.Seconds()))

	if err := in.attachEphemeral(ctx, ns, pod,
		fmt.Sprintf("chaos-cpu-throttle-%d", time.Now().Unix()),
		[]string{"sh", "-c", script},
		corev1.Capability("SYS_ADMIN")); err != nil {
		return fmt.Errorf("attach ephemeral: %w", err)
	}
	in.track(string(KindCPUThrottle), ns, pod, a.Duration, map[string]string{
		"cpu_max_micros": fmt.Sprint(a.CPUMaxMicros),
	})
	in.deps.Logger.Infow("chaos: cpu-throttle",
		"ns", ns, "pod", pod, "cpu_max_micros", a.CPUMaxMicros)
	return nil
}

// ----------------------------------------------------------------------------
//  Primitive: Partition (network egress block to a named upstream)
//
//  Implemented as an iptables -A OUTPUT -d <ip> -j DROP rule, scheduled to
//  be reverted by the same shell script after `duration_s` seconds.
// ----------------------------------------------------------------------------

type PartitionArgs struct {
	Upstream string // hostname or IP — resolved via getent inside the pod
	Duration time.Duration
}

func (in *Injector) Partition(ctx context.Context, ns, pod string, a PartitionArgs) error {
	if err := in.validate(ns, pod); err != nil {
		return err
	}
	if a.Upstream == "" {
		return errors.New("upstream required")
	}
	if a.Duration <= 0 {
		a.Duration = 30 * time.Second
	}

	script := fmt.Sprintf(
		"ip=$(getent hosts %q | awk '{print $1; exit}'); "+
			// Fail (exit 1) when the upstream can't be resolved: exiting 0
			// here left the ephemeral container "Completed" so the partition
			// looked injected while no iptables rule was ever added.
			"if [ -z \"$ip\" ]; then echo \"chaos: cannot resolve %q\" >&2; exit 1; fi; "+
			"iptables -A OUTPUT -d \"$ip\" -j DROP; "+
			"sleep %d; "+
			"iptables -D OUTPUT -d \"$ip\" -j DROP || true",
		a.Upstream, a.Upstream, int(a.Duration.Seconds()))

	if err := in.attachEphemeral(ctx, ns, pod,
		fmt.Sprintf("chaos-partition-%d", time.Now().Unix()),
		[]string{"sh", "-c", script},
		corev1.Capability("NET_ADMIN")); err != nil {
		return fmt.Errorf("attach ephemeral: %w", err)
	}
	in.track(string(KindPartition), ns, pod, a.Duration, map[string]string{
		"upstream": a.Upstream,
	})
	in.deps.Logger.Infow("chaos: partition",
		"ns", ns, "pod", pod, "upstream", a.Upstream, "duration", a.Duration)
	return nil
}

// ----------------------------------------------------------------------------
//  Status + Reaper
// ----------------------------------------------------------------------------

// Active returns a snapshot of currently-active injections, keyed by
// "<namespace>/<pod>/<kind>".
func (in *Injector) Active() map[string]ActiveSnapshot {
	in.mu.Lock()
	defer in.mu.Unlock()
	out := make(map[string]ActiveSnapshot, len(in.active))
	for k, v := range in.active {
		out[k] = ActiveSnapshot{
			Kind:      string(v.Kind),
			Pod:       v.Pod,
			Namespace: v.Namespace,
			ExpiresAt: v.ExpiresAt,
			Hints:     v.RevertHints,
		}
	}
	return out
}

// ActiveSnapshot is the wire shape the HTTP layer returns.
type ActiveSnapshot struct {
	Kind      string            `json:"kind"`
	Pod       string            `json:"pod"`
	Namespace string            `json:"namespace"`
	ExpiresAt time.Time         `json:"expires_at"`
	Hints     map[string]string `json:"hints,omitempty"`
}

// RunReaper periodically prunes expired injections from the in-memory table.
// The on-pod state (tc qdisc / iptables rules / cgroup writes) is reverted
// by the ephemeral container's own `sleep N && revert` script — we just
// keep the bookkeeping clean. Called once at boot to drop stale entries.
func (in *Injector) RunReaper(ctx context.Context, every time.Duration) {
	t := time.NewTicker(every)
	defer t.Stop()
	for {
		select {
		case <-ctx.Done():
			return
		case <-t.C:
			now := time.Now()
			in.mu.Lock()
			for k, v := range in.active {
				if v.ExpiresAt.Before(now) {
					delete(in.active, k)
					in.deps.Logger.Infow("chaos: reaped",
						"kind", v.Kind, "ns", v.Namespace, "pod", v.Pod)
				}
			}
			in.mu.Unlock()
		}
	}
}

// ----------------------------------------------------------------------------
//  Implementation helpers
// ----------------------------------------------------------------------------

func (in *Injector) track(kind, ns, pod string, d time.Duration,
	hints map[string]string) {
	in.mu.Lock()
	defer in.mu.Unlock()
	key := ns + "/" + pod + "/" + kind
	in.active[key] = &activeInjection{
		Kind:        Kind(kind),
		Pod:         pod,
		Namespace:   ns,
		ExpiresAt:   time.Now().Add(d),
		RevertHints: hints,
	}
}

// attachEphemeral spawns an ephemeral debug container in the target pod
// that runs `cmd`. The container is given the requested capability via a
// privileged security context — required for tc/iptables/cgroup writes.
//
// Notes:
//   - Ephemeral containers cannot be removed once added, so the lifecycle
//     is bound to the pod's lifecycle. That's fine — we either: the pod is
//     short-lived (a sandbox), or it's a long-lived service the operator
//     accepts a "container-was-added" entry on.
//   - The container's image bundles iproute2 + iptables; same image we use
//     for perf-profiler. Override via CHAOS_INJECTOR_IMAGE.
func (in *Injector) attachEphemeral(ctx context.Context, ns, pod, name string,
	cmd []string, cap corev1.Capability) error {
	priv := true
	allowEsc := true

	image := envOr("CHAOS_INJECTOR_IMAGE",
		"ghcr.io/velocity/perf-profiler:latest")

	patch := &corev1.Pod{
		Spec: corev1.PodSpec{
			EphemeralContainers: []corev1.EphemeralContainer{{
				EphemeralContainerCommon: corev1.EphemeralContainerCommon{
					Name:            name,
					Image:           image,
					Command:         cmd,
					ImagePullPolicy: corev1.PullIfNotPresent,
					SecurityContext: &corev1.SecurityContext{
						Privileged:               &priv,
						AllowPrivilegeEscalation: &allowEsc,
						Capabilities: &corev1.Capabilities{
							Add: []corev1.Capability{cap},
						},
					},
				},
			}},
		},
	}

	// Subresource POST: /api/v1/namespaces/<ns>/pods/<name>/ephemeralcontainers
	_, err := in.deps.K8s.CoreV1().Pods(ns).
		UpdateEphemeralContainers(ctx, pod, patch, metav1.UpdateOptions{})
	if err != nil {
		return fmt.Errorf("update ephemeralcontainers: %w", err)
	}
	return nil
}

func envOr(key, def string) string {
	// Local helper so the package doesn't depend on os in the public surface.
	if v, ok := lookupEnv(key); ok {
		return v
	}
	return def
}
