// Command chaos-orchestrator is a small HTTP control-plane that injects
// production-grade chaos into the Velocity platform.
//
// Why a separate process?
//
//   - It needs to talk to the Kubernetes API with elevated permissions
//     (delete pods, exec into pods, write to ephemeral containers); the
//     submission-engine intentionally runs without those privileges so
//     a compromised engine cannot tear down its peers.
//   - Chaos actions are inherently destructive; isolating them in a
//     dedicated deployment makes audit trails trivial (every action goes
//     through one binary) and lets us roll out chaos changes without
//     redeploying the whole platform.
//
// The API surface is intentionally small. Five injectors, mapped 1:1 to the
// roadmap (`docs/architecture.md` §"Chaos engineering harness"):
//
//   POST /v1/chaos/pod-kill         delete the target pod
//   POST /v1/chaos/tc-latency       inject `tc netem` delay + jitter
//   POST /v1/chaos/tc-loss          inject random packet loss via tc netem
//   POST /v1/chaos/cpu-throttle     tighten the pod's cpu.max cgroup
//   POST /v1/chaos/partition        block egress to a named upstream
//
// All actions are *time-bounded* — every injection accepts a `duration_s`
// field, and the orchestrator tracks the deadline and reverts the
// injection automatically. Crashes therefore can't leave a pod stuck in
// the broken state forever; on boot, we sweep for outstanding "chaos="
// labels and revert anything older than its declared lifetime.
package main

import (
	"context"
	"errors"
	"fmt"
	"net/http"
	"os"
	"os/signal"
	"syscall"
	"time"

	"go.uber.org/zap"
	"k8s.io/client-go/kubernetes"
	"k8s.io/client-go/rest"
	"k8s.io/client-go/tools/clientcmd"

	"github.com/velocity/platform/services/chaos-orchestrator/internal/injector"
	"github.com/velocity/platform/services/chaos-orchestrator/internal/server"
)

func main() {
	if err := run(); err != nil {
		fmt.Fprintf(os.Stderr, "chaos-orchestrator: %v\n", err)
		os.Exit(1)
	}
}

func run() error {
	logger, err := zap.NewProduction()
	if err != nil {
		return fmt.Errorf("zap: %w", err)
	}
	sugar := logger.Sugar()

	httpAddr := envOr("CHAOS_HTTP_ADDR", "0.0.0.0:8090")
	allowedNs := envOr("CHAOS_NAMESPACES", "velocity-sandbox,velocity-load")

	clientset, err := newK8sClient()
	if err != nil {
		return fmt.Errorf("k8s client: %w", err)
	}

	inj := injector.New(injector.Deps{
		K8s:       clientset,
		Logger:    sugar,
		Namespace: allowedNs,
	})

	srv := server.New(server.Config{
		Addr:       httpAddr,
		Logger:     sugar,
		Injector:   inj,
		Namespaces: allowedNs,
	})

	ctx, cancel := signal.NotifyContext(context.Background(),
		syscall.SIGINT, syscall.SIGTERM)
	defer cancel()

	// Reaper: every 30s, walk every namespace and revert any injection whose
	// deadline has passed. Bootstrap call sweeps any leftover state from a
	// previous instance that crashed mid-injection.
	go inj.RunReaper(ctx, 30*time.Second)

	errCh := make(chan error, 1)
	go func() {
		sugar.Infow("HTTP listening", "addr", httpAddr, "namespaces", allowedNs)
		errCh <- srv.Listen()
	}()

	select {
	case <-ctx.Done():
		sugar.Infow("shutdown requested")
		shutdownCtx, shutdownCancel := context.WithTimeout(
			context.Background(), 10*time.Second)
		defer shutdownCancel()
		return srv.Shutdown(shutdownCtx)
	case err := <-errCh:
		if err != nil && !errors.Is(err, http.ErrServerClosed) {
			return fmt.Errorf("http server: %w", err)
		}
		return nil
	}
}

func newK8sClient() (kubernetes.Interface, error) {
	// In-cluster config takes priority — that's the production path.
	if cfg, err := rest.InClusterConfig(); err == nil {
		return kubernetes.NewForConfig(cfg)
	}
	// Fall back to ~/.kube/config for local development.
	kubeconfig := envOr("KUBECONFIG", clientcmd.RecommendedHomeFile)
	cfg, err := clientcmd.BuildConfigFromFlags("", kubeconfig)
	if err != nil {
		return nil, err
	}
	return kubernetes.NewForConfig(cfg)
}

func envOr(key, def string) string {
	if v := os.Getenv(key); v != "" {
		return v
	}
	return def
}
