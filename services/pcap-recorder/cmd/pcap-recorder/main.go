// Command pcap-recorder is a deterministic-replay enabler for the Velocity
// platform. It captures the bot-fleet → submission-pod TCP byte stream during
// every benchmark and persists it to MinIO under
//
//	pcaps/<benchmark_id>.pcap
//
// Why a separate service (vs. a sidecar in every submission pod)?
//
//   - We need raw-packet capture privileges (CAP_NET_RAW, CAP_NET_ADMIN). The
//     submission pods run under gVisor with all capabilities dropped; granting
//     them packet capture would defeat the security model.
//
//   - The recorder is operator-owned. Submissions never have to opt in or
//     even know capture is happening — the contest record is immutable from
//     their POV.
//
//   - Capture state has a non-trivial lifecycle (start → buffer → rotate →
//     finalize → upload). Pulling it into one binary makes failure modes
//     auditable in one log stream.
//
// The control surface mirrors the chaos-orchestrator on purpose: small,
// HTTP-shaped, no protobuf required at the gateway boundary. The full
// replay path (PcapService gRPC) lives in the sibling pcap-replayer.
//
// HTTP API:
//
//	POST /v1/recorder/start
//	     { benchmark_id, namespace, pod, target_port, ttl_seconds }
//	     → starts tcpdump in a privileged ephemeral container attached to
//	       the target pod, BPF-filtered to traffic on `target_port`.
//
//	POST /v1/recorder/stop
//	     { benchmark_id }
//	     → asks tcpdump to flush, then streams the pcap out via
//	       pods/exec and uploads to MinIO. Returns the object key.
//
//	GET  /v1/recorder/state
//	     → enumerates active recordings (benchmark_id, pod, started_at,
//	       deadline_at).
//
//	GET  /healthz
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

	"github.com/velocity/platform/services/pcap-recorder/internal/recorder"
	"github.com/velocity/platform/services/pcap-recorder/internal/server"
	"github.com/velocity/platform/services/pcap-recorder/internal/store"
)

func main() {
	if err := run(); err != nil {
		fmt.Fprintf(os.Stderr, "pcap-recorder: %v\n", err)
		os.Exit(1)
	}
}

func run() error {
	logger, err := zap.NewProduction()
	if err != nil {
		return fmt.Errorf("zap: %w", err)
	}
	sugar := logger.Sugar()

	httpAddr := envOr("RECORDER_HTTP_ADDR", "0.0.0.0:8091")
	allowedNs := envOr("RECORDER_NAMESPACES", "velocity-sandbox,velocity-load")
	bucket := envOr("MINIO_BUCKET", "velocity-artefacts")
	minioEndpoint := envOr("MINIO_ENDPOINT", "minio.velocity-control.svc.cluster.local:9000")
	minioAccess := envOr("MINIO_ACCESS_KEY", "minio")
	minioSecret := envOr("MINIO_SECRET_KEY", "minio123")
	tcpdumpImage := envOr("RECORDER_TCPDUMP_IMAGE", "nicolaka/netshoot:latest")

	clientset, err := newK8sClient()
	if err != nil {
		return fmt.Errorf("k8s client: %w", err)
	}

	restCfg, err := newRESTConfig()
	if err != nil {
		return fmt.Errorf("rest config: %w", err)
	}

	st, err := store.NewMinIO(minioEndpoint, minioAccess, minioSecret, bucket, false)
	if err != nil {
		return fmt.Errorf("minio: %w", err)
	}

	rec := recorder.New(recorder.Deps{
		K8s:           clientset,
		REST:          restCfg,
		Logger:        sugar,
		Namespaces:    allowedNs,
		Storage:       st,
		TcpdumpImage:  tcpdumpImage,
	})

	srv := server.New(server.Config{
		Addr:       httpAddr,
		Logger:     sugar,
		Recorder:   rec,
		Namespaces: allowedNs,
	})

	ctx, cancel := signal.NotifyContext(context.Background(),
		syscall.SIGINT, syscall.SIGTERM)
	defer cancel()

	// On boot, list any leftover capture sessions and either resume their
	// deadlines or force-finalize them. A crashed recorder must not leave
	// tcpdump pinned forever to a submission pod's lifecycle.
	go rec.RunReaper(ctx, 30*time.Second)

	errCh := make(chan error, 1)
	go func() {
		sugar.Infow("HTTP listening",
			"addr", httpAddr,
			"namespaces", allowedNs,
			"bucket", bucket)
		errCh <- srv.Listen()
	}()

	select {
	case <-ctx.Done():
		sugar.Infow("shutdown requested")
		shutdownCtx, shutdownCancel := context.WithTimeout(
			context.Background(), 30*time.Second)
		defer shutdownCancel()
		// Best-effort: cleanly flush any in-flight recordings so the
		// MinIO objects are consistent before we die.
		if drainErr := rec.FlushAll(shutdownCtx); drainErr != nil {
			sugar.Warnw("flush-on-shutdown failed", "err", drainErr)
		}
		return srv.Shutdown(shutdownCtx)
	case err := <-errCh:
		if err != nil && !errors.Is(err, http.ErrServerClosed) {
			return fmt.Errorf("http server: %w", err)
		}
		return nil
	}
}

func newK8sClient() (kubernetes.Interface, error) {
	cfg, err := newRESTConfig()
	if err != nil {
		return nil, err
	}
	return kubernetes.NewForConfig(cfg)
}

func newRESTConfig() (*rest.Config, error) {
	if cfg, err := rest.InClusterConfig(); err == nil {
		return cfg, nil
	}
	kubeconfig := envOr("KUBECONFIG", clientcmd.RecommendedHomeFile)
	return clientcmd.BuildConfigFromFlags("", kubeconfig)
}

func envOr(key, def string) string {
	if v := os.Getenv(key); v != "" {
		return v
	}
	return def
}
