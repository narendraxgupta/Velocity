// =============================================================================
// velocity-submission-engine — entrypoint
//
// Boots the gRPC server, wires in the Docker/K8s sandboxers and the MinIO
// artefact store, and blocks until SIGTERM.
//
// Configuration is via environment variables (same convention as the C++
// services). Required vars are documented in internal/config/config.go.
// =============================================================================
package main

import (
	"context"
	"errors"
	"os/signal"
	"strings"
	"syscall"
	"time"

	"github.com/redis/go-redis/v9"
	"k8s.io/client-go/kubernetes"
	"k8s.io/client-go/rest"
	"k8s.io/client-go/tools/clientcmd"

	"github.com/velocity/platform/services/submission-engine/internal/builder"
	"github.com/velocity/platform/services/submission-engine/internal/config"
	"github.com/velocity/platform/services/submission-engine/internal/dockerengine"
	"github.com/velocity/platform/services/submission-engine/internal/log"
	"github.com/velocity/platform/services/submission-engine/internal/sandbox"
	"github.com/velocity/platform/services/submission-engine/internal/server"
	"github.com/velocity/platform/services/submission-engine/internal/storage"
)

func main() {
	logger := log.New("submission-engine")
	defer func() { _ = logger.Sync() }()

	cfg, err := config.Load()
	if err != nil {
		logger.Fatalw("invalid configuration", "err", err)
	}

	logger.Infow(
		"velocity-submission-engine starting",
		"grpc_port", cfg.GRPCPort,
		"registry", cfg.RegistryHost,
		"minio", cfg.MinIOEndpoint,
	)

	// Cancel on SIGTERM / SIGINT.
	ctx, cancel := signal.NotifyContext(context.Background(), syscall.SIGTERM, syscall.SIGINT)
	defer cancel()

	// External deps.
	store, err := storage.NewMinIO(cfg.MinIOEndpoint, cfg.MinIOAccessKey,
		cfg.MinIOSecretKey, cfg.MinIOBucket, cfg.MinIOUseTLS)
	if err != nil {
		logger.Fatalw("storage init", "err", err)
	}

	var rdb *redis.Client
	if cfg.RedisAddr != "" {
		opts, perr := redis.ParseURL(cfg.RedisAddr)
		if perr != nil {
			// Fall back to host:port form.
			opts = &redis.Options{Addr: cfg.RedisAddr}
		}
		rdb = redis.NewClient(opts)
		if perr := rdb.Ping(ctx).Err(); perr != nil {
			logger.Warnw("redis unavailable; build logs will not be streamed",
				"addr", cfg.RedisAddr, "err", perr)
			rdb = nil
		}
	}

	// Select the build/sandbox backend. "docker" targets a local daemon over
	// the mounted socket (dev / Codespace, no cluster); "kubernetes" (default)
	// uses Kaniko Jobs + gVisor pods. Only the k8s path loads a kubeconfig, so
	// the engine no longer fatals on a clusterless host.
	var (
		bld builder.Builder
		sbx sandbox.Sandbox
	)
	switch strings.ToLower(cfg.SandboxBackend) {
	case "docker", "local":
		eng, derr := dockerengine.New(cfg.DockerHost)
		if derr != nil {
			logger.Fatalw("docker client", "err", derr)
		}
		if perr := eng.Ping(ctx); perr != nil {
			logger.Fatalw("docker daemon unreachable", "host", cfg.DockerHost, "err", perr)
		}
		logger.Infow("sandbox backend: docker",
			"network", cfg.SandboxNetwork, "service_port", cfg.SandboxServicePort)
		bld = builder.NewDocker(builder.DockerConfig{Engine: eng, Logger: logger})
		sbx = sandbox.NewDocker(sandbox.DockerConfig{
			Engine:      eng,
			Logger:      logger,
			Network:     cfg.SandboxNetwork,
			ServicePort: cfg.SandboxServicePort,
		})
	default:
		kcfg, kerr := loadK8sConfig()
		if kerr != nil {
			logger.Fatalw("k8s config", "err", kerr)
		}
		kcli, kerr := kubernetes.NewForConfig(kcfg)
		if kerr != nil {
			logger.Fatalw("k8s client", "err", kerr)
		}
		logger.Infow("sandbox backend: kubernetes", "namespace", cfg.SandboxNamespace)
		bld = builder.NewKaniko(builder.Config{
			K8sClient:    kcli,
			Namespace:    cfg.SandboxNamespace,
			RegistryHost: cfg.RegistryHost,
			Redis:        rdb,
		})
		sbx = sandbox.NewGVisor(sandbox.Config{
			K8sClient:    kcli,
			Namespace:    cfg.SandboxNamespace,
			RuntimeClass: cfg.SandboxRuntimeClass,
		})
	}

	srv, err := server.New(cfg, server.Deps{
		Storage: store,
		Builder: bld,
		Sandbox: sbx,
	}, logger)
	if err != nil {
		logger.Fatalw("server construction failed", "err", err)
	}

	if err := srv.Serve(ctx); err != nil && !errors.Is(err, context.Canceled) {
		logger.Errorw("server terminated with error", "err", err)
	}

	logger.Infow("shutdown initiated; draining")
	drainCtx, drainCancel := context.WithTimeout(context.Background(), 5*time.Second)
	defer drainCancel()
	if err := srv.Stop(drainCtx); err != nil {
		logger.Warnw("graceful shutdown error", "err", err)
	}

	logger.Infow("shutdown complete")
}

// loadK8sConfig returns an in-cluster REST config when available, falling
// back to ~/.kube/config for local development.
func loadK8sConfig() (*rest.Config, error) {
	if cfg, err := rest.InClusterConfig(); err == nil {
		return cfg, nil
	}
	loader := clientcmd.NewDefaultClientConfigLoadingRules()
	return clientcmd.NewNonInteractiveDeferredLoadingClientConfig(
		loader, &clientcmd.ConfigOverrides{}).ClientConfig()
}
