// Package config centralizes the submission-engine's environment-variable
// derived configuration. Every value the binary needs lives here; nowhere
// else in the codebase should we be calling os.Getenv.
package config

import (
	"fmt"
	"os"
	"strconv"
	"strings"
)

// Config is the validated runtime configuration. Construct via Load().
type Config struct {
	// Listeners
	GRPCPort    uint16
	MetricsPort uint16

	// Image registry (push target for Kaniko-built images)
	RegistryHost string // e.g. "registry:5000"

	// Object storage
	MinIOEndpoint  string
	MinIOAccessKey string
	MinIOSecretKey string
	MinIOBucket    string
	MinIOUseTLS    bool

	// Sandbox configuration
	SandboxBackend      string // "kubernetes" (default) or "docker" (dev/Codespace)
	DockerHost          string // docker daemon socket for the "docker" backend
	SandboxNetwork      string // docker network the bot fleet shares (docker backend)
	SandboxServicePort  uint16 // submission listen port the bots target (docker backend)
	SandboxNamespace    string
	SandboxRuntimeClass string // "gvisor" in production
	DefaultCPUCores     uint32
	DefaultMemoryMiB    uint32
	DefaultLifetime     uint32 // seconds

	// Observability
	OTLPEndpoint string

	// Build-log fan-out. Optional — if set, the builder streams Kaniko stdout
	// into a Redis LIST `build:<submission_id>` so the gateway can serve it
	// to the frontend's submission detail page.
	RedisAddr string

	// Log level
	LogLevel string
}

// Load reads the environment and returns a validated Config. Returns an
// error rather than panicking so the caller controls the exit path.
func Load() (*Config, error) {
	c := &Config{}

	var err error
	if c.GRPCPort, err = requiredUint16("VELOCITY_GRPC_PORT"); err != nil {
		// 7001 is the documented default; we still surface this if the
		// caller forgot to set it explicitly. Make it optional with default.
		c.GRPCPort = 7001
	}
	c.MetricsPort = optionalUint16("VELOCITY_METRICS_PORT", 9092)
	c.RegistryHost = requiredString("VELOCITY_REGISTRY_HOST", &err)
	c.MinIOEndpoint = requiredString("VELOCITY_MINIO_ENDPOINT", &err)
	c.MinIOAccessKey = requiredString("VELOCITY_MINIO_ACCESS_KEY", &err)
	c.MinIOSecretKey = requiredString("VELOCITY_MINIO_SECRET_KEY", &err)
	c.MinIOBucket = optionalString("VELOCITY_MINIO_BUCKET", "submissions")
	c.MinIOUseTLS = optionalBool("VELOCITY_MINIO_USE_TLS", false)

	c.SandboxBackend = optionalString("VELOCITY_SANDBOX_BACKEND", "kubernetes")
	c.DockerHost = optionalString("VELOCITY_DOCKER_HOST", "unix:///var/run/docker.sock")
	c.SandboxNetwork = optionalString("VELOCITY_SANDBOX_NETWORK", "velocity-apps")
	c.SandboxServicePort = optionalUint16("VELOCITY_SANDBOX_SERVICE_PORT", 8080)
	c.SandboxNamespace = optionalString("VELOCITY_SANDBOX_NAMESPACE", "velocity-sandbox")
	c.SandboxRuntimeClass = optionalString("VELOCITY_SANDBOX_RUNTIME_CLASS", "gvisor")
	c.DefaultCPUCores = optionalUint32("VELOCITY_DEFAULT_CPU_CORES", 2)
	c.DefaultMemoryMiB = optionalUint32("VELOCITY_DEFAULT_MEMORY_MIB", 512)
	c.DefaultLifetime = optionalUint32("VELOCITY_DEFAULT_LIFETIME_SECONDS", 600)

	c.OTLPEndpoint = optionalString("VELOCITY_OTLP_ENDPOINT", "")
	c.RedisAddr    = optionalString("VELOCITY_REDIS_ADDR", "")
	c.LogLevel     = optionalString("VELOCITY_LOG_LEVEL", "info")

	if err != nil {
		return nil, err
	}
	return c, nil
}

// -----------------------------------------------------------------------------
// Small parsers.
// -----------------------------------------------------------------------------

func requiredString(key string, errOut *error) string {
	v := strings.TrimSpace(os.Getenv(key))
	if v == "" && *errOut == nil {
		*errOut = fmt.Errorf("required env var %s is unset", key)
	}
	return v
}

func optionalString(key, fallback string) string {
	if v := strings.TrimSpace(os.Getenv(key)); v != "" {
		return v
	}
	return fallback
}

func requiredUint16(key string) (uint16, error) {
	v, ok := os.LookupEnv(key)
	if !ok || strings.TrimSpace(v) == "" {
		return 0, fmt.Errorf("required env var %s is unset", key)
	}
	n, err := strconv.ParseUint(strings.TrimSpace(v), 10, 16)
	if err != nil {
		return 0, fmt.Errorf("env var %s is not a valid uint16: %w", key, err)
	}
	return uint16(n), nil
}

func optionalUint16(key string, fallback uint16) uint16 {
	if v := strings.TrimSpace(os.Getenv(key)); v != "" {
		if n, err := strconv.ParseUint(v, 10, 16); err == nil {
			return uint16(n)
		}
	}
	return fallback
}

func optionalUint32(key string, fallback uint32) uint32 {
	if v := strings.TrimSpace(os.Getenv(key)); v != "" {
		if n, err := strconv.ParseUint(v, 10, 32); err == nil {
			return uint32(n)
		}
	}
	return fallback
}

func optionalBool(key string, fallback bool) bool {
	v := strings.ToLower(strings.TrimSpace(os.Getenv(key)))
	switch v {
	case "1", "true", "yes", "y", "on":
		return true
	case "0", "false", "no", "n", "off":
		return false
	default:
		return fallback
	}
}
