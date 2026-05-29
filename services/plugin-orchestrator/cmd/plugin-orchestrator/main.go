// =============================================================================
//  plugin-orchestrator — control loop.
//
//  Two flavours:
//    - --watch: polls a Git checkout / mounted directory for manifest
//               files and reconciles continuously. Production mode.
//    - --once <path>: applies one manifest and exits. Useful in CI and
//               in ad-hoc operator commands.
//
//  HTTP surface:
//    GET  /healthz    — liveness
//    GET  /v1/plugins/{tenant}  — debug: dump current registry
//    POST /v1/plugins/{tenant}/reconcile  — kick a reconcile NOW
//
//  The HTTP server is admin-only at the gateway level (RBAC table puts
//  /v1/plugins at ADMIN).
// =============================================================================

package main

import (
	"context"
	"errors"
	"flag"
	"fmt"
	"io"
	"net/http"
	"os"
	"os/signal"
	"path/filepath"
	"strings"
	"syscall"
	"time"

	"github.com/redis/go-redis/v9"
	"go.uber.org/zap"
	"gopkg.in/yaml.v3"
	"k8s.io/client-go/kubernetes"
	"k8s.io/client-go/rest"
	"k8s.io/client-go/tools/clientcmd"

	"github.com/velocity/platform/services/plugin-orchestrator/internal/deployer"
	"github.com/velocity/platform/services/plugin-orchestrator/internal/discovery"
	"github.com/velocity/platform/services/plugin-orchestrator/internal/registry"
)

func main() {
	var (
		watchDir  = flag.String("watch", "", "Directory of *.yaml plugin registries to reconcile in a loop.")
		oncePath  = flag.String("once", "", "Apply a single manifest and exit.")
		redisAddr = flag.String("redis", envDefault("REDIS_ADDR", "redis.velocity-data.svc.cluster.local:6379"), "Redis address.")
		httpAddr  = flag.String("http", envDefault("HTTP_ADDR", ":8082"), "HTTP listen address.")
		interval  = flag.Duration("interval", 30*time.Second, "Reconcile interval in --watch mode.")
	)
	flag.Parse()

	logger, _ := zap.NewProduction()
	defer func() { _ = logger.Sync() }()
	sugar := logger.Sugar()

	k8s, err := buildKubeClient()
	if err != nil {
		sugar.Fatalw("kube client init failed", "err", err)
	}
	rdb := redis.NewClient(&redis.Options{Addr: *redisAddr})
	defer rdb.Close()

	dep := deployer.New(k8s)
	pub := discovery.New(rdb)

	if *oncePath != "" {
		ctx, cancel := context.WithTimeout(context.Background(), 60*time.Second)
		defer cancel()
		if err := reconcileOne(ctx, *oncePath, dep, pub, sugar); err != nil {
			sugar.Fatalw("reconcile failed", "err", err)
		}
		return
	}

	ctx, cancel := signal.NotifyContext(context.Background(),
		os.Interrupt, syscall.SIGTERM)
	defer cancel()

	go startHTTP(*httpAddr, rdb, dep, pub, sugar)

	if *watchDir == "" {
		sugar.Info("no --watch directory set; running HTTP server only")
		<-ctx.Done()
		return
	}
	ticker := time.NewTicker(*interval)
	defer ticker.Stop()
	for {
		entries, err := os.ReadDir(*watchDir)
		if err != nil {
			sugar.Warnw("watch dir read failed", "err", err)
		}
		for _, e := range entries {
			if e.IsDir() || !strings.HasSuffix(e.Name(), ".yaml") {
				continue
			}
			if err := reconcileOne(ctx, filepath.Join(*watchDir, e.Name()),
				dep, pub, sugar); err != nil {
				sugar.Warnw("reconcile error", "file", e.Name(), "err", err)
			}
		}
		select {
		case <-ctx.Done():
			return
		case <-ticker.C:
		}
	}
}

func reconcileOne(ctx context.Context, path string, dep *deployer.Deployer,
	pub *discovery.Publisher, log *zap.SugaredLogger) error {
	m, err := registry.Load(path)
	if err != nil {
		return err
	}
	if err := dep.Apply(ctx, m); err != nil {
		return fmt.Errorf("deploy: %w", err)
	}
	if err := pub.Publish(ctx, m); err != nil {
		return fmt.Errorf("publish: %w", err)
	}
	log.Infow("reconciled tenant", "tenant", m.Tenant,
		"plugins", len(m.Plugins))
	return nil
}

// parsePluginsPath extracts the tenant id and the (optional) verb from
// the path under `/v1/plugins/`. Examples:
//
//	/v1/plugins/acme            → tenant="acme",  verb=""
//	/v1/plugins/acme/           → tenant="acme",  verb=""
//	/v1/plugins/acme/reconcile  → tenant="acme",  verb="reconcile"
//
// Anything deeper (e.g. /v1/plugins/acme/foo/bar) is rejected — we
// keep the surface small on purpose.
func parsePluginsPath(p string) (tenant, verb string, ok bool) {
	rest := strings.TrimPrefix(p, "/v1/plugins/")
	rest = strings.Trim(rest, "/")
	if rest == "" {
		return "", "", false
	}
	parts := strings.Split(rest, "/")
	switch len(parts) {
	case 1:
		return parts[0], "", true
	case 2:
		return parts[0], parts[1], true
	default:
		return "", "", false
	}
}

func startHTTP(addr string, rdb *redis.Client, dep *deployer.Deployer,
	pub *discovery.Publisher, log *zap.SugaredLogger) {
	mux := http.NewServeMux()
	mux.HandleFunc("/healthz", func(w http.ResponseWriter, _ *http.Request) {
		_, _ = w.Write([]byte("ok\n"))
	})
	mux.HandleFunc("/v1/plugins/", func(w http.ResponseWriter, r *http.Request) {
		tenant, verb, ok := parsePluginsPath(r.URL.Path)
		if !ok {
			http.Error(w, "bad path", http.StatusBadRequest)
			return
		}

		switch {
		case verb == "" && r.Method == http.MethodGet:
			handlePluginsList(w, r, rdb, tenant)
		case verb == "reconcile" && r.Method == http.MethodPost:
			handlePluginsReconcile(w, r, dep, pub, tenant, log)
		default:
			http.Error(w, "method/verb not supported", http.StatusMethodNotAllowed)
		}
	})

	srv := &http.Server{Addr: addr, Handler: mux,
		ReadHeaderTimeout: 5 * time.Second}
	if err := srv.ListenAndServe(); err != nil && !errors.Is(err, http.ErrServerClosed) {
		log.Errorw("http server stopped", "err", err)
	}
}

func handlePluginsList(w http.ResponseWriter, r *http.Request,
	rdb *redis.Client, tenant string) {
	key := fmt.Sprintf("plugins:registry:%s", tenant)
	vals, err := rdb.HGetAll(r.Context(), key).Result()
	if err != nil {
		http.Error(w, err.Error(), http.StatusInternalServerError)
		return
	}
	w.Header().Set("Content-Type", "application/json")
	_, _ = w.Write([]byte("{"))
	first := true
	for k, v := range vals {
		if !first {
			_, _ = w.Write([]byte(","))
		}
		_, _ = fmt.Fprintf(w, `%q:%s`, k, v)
		first = false
	}
	_, _ = w.Write([]byte("}"))
}

// handlePluginsReconcile accepts a manifest body (application/yaml),
// validates it, applies the deployment, and publishes the discovery
// entries. The {tenant} URL segment MUST match the manifest's tenant
// field — we reject mismatches rather than silently coercing, because
// a tenant-cross-mint mistake is the kind of bug we don't want.
func handlePluginsReconcile(w http.ResponseWriter, r *http.Request,
	dep *deployer.Deployer, pub *discovery.Publisher, tenant string,
	log *zap.SugaredLogger) {
	const maxBody = 256 * 1024
	body, err := io.ReadAll(io.LimitReader(r.Body, maxBody+1))
	if err != nil {
		http.Error(w, "body read failed", http.StatusBadRequest)
		return
	}
	if len(body) > maxBody {
		http.Error(w, "body too large", http.StatusRequestEntityTooLarge)
		return
	}
	var m registry.Manifest
	if err := yaml.Unmarshal(body, &m); err != nil {
		http.Error(w, "yaml parse: "+err.Error(), http.StatusBadRequest)
		return
	}
	if m.Tenant == "" {
		m.Tenant = tenant
	} else if m.Tenant != tenant {
		http.Error(w, "manifest tenant does not match URL tenant",
			http.StatusBadRequest)
		return
	}
	if err := m.Validate(); err != nil {
		http.Error(w, "validate: "+err.Error(), http.StatusBadRequest)
		return
	}
	ctx, cancel := context.WithTimeout(r.Context(), 30*time.Second)
	defer cancel()
	if err := dep.Apply(ctx, &m); err != nil {
		log.Errorw("reconcile deploy failed", "tenant", tenant, "err", err)
		http.Error(w, "deploy: "+err.Error(), http.StatusBadGateway)
		return
	}
	if err := pub.Publish(ctx, &m); err != nil {
		log.Errorw("reconcile publish failed", "tenant", tenant, "err", err)
		http.Error(w, "publish: "+err.Error(), http.StatusBadGateway)
		return
	}
	w.Header().Set("Content-Type", "application/json")
	_, _ = fmt.Fprintf(w, `{"tenant":%q,"plugins":%d,"status":"reconciled"}`,
		tenant, len(m.Plugins))
}

func buildKubeClient() (kubernetes.Interface, error) {
	// Try in-cluster first; fall back to KUBECONFIG (handy in dev).
	if cfg, err := rest.InClusterConfig(); err == nil {
		return kubernetes.NewForConfig(cfg)
	}
	loader := clientcmd.NewDefaultClientConfigLoadingRules()
	cfg, err := clientcmd.NewNonInteractiveDeferredLoadingClientConfig(
		loader, &clientcmd.ConfigOverrides{}).ClientConfig()
	if err != nil {
		return nil, err
	}
	return kubernetes.NewForConfig(cfg)
}

func envDefault(k, def string) string {
	if v := os.Getenv(k); v != "" {
		return v
	}
	return def
}
