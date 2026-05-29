// critique-service: HTTP front-end for local-LLM strategy critiques.
//
// Environment configuration
// -------------------------
//
//	CRITIQUE_HTTP_ADDR      bind address          default ":8094"
//	CRITIQUE_REDIS_ADDR     redis dsn             default "redis:6379"
//	CRITIQUE_OLLAMA_URL     ollama base url       default "http://ollama:11434"
//	CRITIQUE_OLLAMA_MODEL   model tag             default "qwen2.5-coder:7b"
//	CRITIQUE_MAX_INFLIGHT   concurrent LLM calls  default 2
package main

import (
	"context"
	"net/http"
	"os"
	"os/signal"
	"strconv"
	"syscall"
	"time"

	"github.com/redis/go-redis/v9"
	"go.uber.org/zap"

	"github.com/velocity/platform/services/critique-service/internal/ollama"
	"github.com/velocity/platform/services/critique-service/internal/server"
	"github.com/velocity/platform/services/critique-service/internal/store"
)

func main() {
	logger, _ := zap.NewProduction()
	defer logger.Sync()
	sug := logger.Sugar()

	addr := env("CRITIQUE_HTTP_ADDR", ":8094")
	redisAddr := env("CRITIQUE_REDIS_ADDR", "redis:6379")
	ollamaURL := env("CRITIQUE_OLLAMA_URL", "http://ollama:11434")
	model := env("CRITIQUE_OLLAMA_MODEL", "qwen2.5-coder:7b")
	maxInFlight, _ := strconv.Atoi(env("CRITIQUE_MAX_INFLIGHT", "2"))

	rdb := redis.NewClient(&redis.Options{Addr: redisAddr})
	if err := rdb.Ping(context.Background()).Err(); err != nil {
		sug.Warnw("redis ping failed (continuing)", "err", err)
	}

	oc := ollama.New(ollamaURL, model)

	s := server.New(server.Config{
		Addr:        addr,
		Ollama:      oc,
		Store:       store.New(rdb),
		Logger:      sug,
		MaxInFlight: maxInFlight,
	})

	srv := &http.Server{
		Addr:              addr,
		Handler:           s.Routes(),
		ReadHeaderTimeout: 5 * time.Second,
	}
	go func() {
		sug.Infow("critique-service listening",
			"addr", addr, "ollama", ollamaURL, "model", model)
		if err := srv.ListenAndServe(); err != nil && err != http.ErrServerClosed {
			sug.Fatalw("listen failed", "err", err)
		}
	}()

	ctx, cancel := signal.NotifyContext(context.Background(),
		syscall.SIGINT, syscall.SIGTERM)
	defer cancel()
	<-ctx.Done()
	shutdownCtx, c := context.WithTimeout(context.Background(), 10*time.Second)
	defer c()
	_ = srv.Shutdown(shutdownCtx)
}

func env(k, def string) string {
	if v := os.Getenv(k); v != "" {
		return v
	}
	return def
}
