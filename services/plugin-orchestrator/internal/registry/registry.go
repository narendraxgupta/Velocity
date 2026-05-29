// Package registry parses the user-supplied plugin manifest.
//
// The manifest is a small YAML document committed alongside each
// tenant's deployment config:
//
//   apiVersion: velocity/v1
//   kind: PluginRegistry
//   tenant: acme
//   plugins:
//     - id:      acme/stp-strict/1.2.0
//       image:   ghcr.io/acme/velocity-plugin-stp:1.2.0
//       enabled: true
//       cpu:     "500m"
//       memory:  "256Mi"
//       env:
//         STP_LEVEL: STRICT
//       events:
//         - ORDER
//         - FILL
//
// We deliberately keep the shape narrow — anything that doesn't fit in
// a 10-line declaration probably shouldn't be a plugin yet.
package registry

import (
	"errors"
	"fmt"
	"os"
	"regexp"

	"gopkg.in/yaml.v3"
)

// Manifest is the on-disk shape.
type Manifest struct {
	APIVersion string   `yaml:"apiVersion"`
	Kind       string   `yaml:"kind"`
	Tenant     string   `yaml:"tenant"`
	Plugins    []Plugin `yaml:"plugins"`
}

type Plugin struct {
	ID       string            `yaml:"id"`
	Image    string            `yaml:"image"`
	Enabled  bool              `yaml:"enabled"`
	CPU      string            `yaml:"cpu"`
	Memory   string            `yaml:"memory"`
	Env      map[string]string `yaml:"env,omitempty"`
	Events   []string          `yaml:"events,omitempty"`
	// Port the plugin's gRPC server listens on. Defaults to 50061.
	Port int32 `yaml:"port,omitempty"`
}

var (
	idRegex     = regexp.MustCompile(`^[a-z0-9][a-z0-9./-]{2,127}$`)
	tenantRegex = regexp.MustCompile(`^[a-z][a-z0-9-]{1,30}[a-z0-9]$`)
)

// Load reads + validates a manifest file.
func Load(path string) (*Manifest, error) {
	raw, err := os.ReadFile(path)
	if err != nil {
		return nil, fmt.Errorf("read manifest: %w", err)
	}
	var m Manifest
	if err := yaml.Unmarshal(raw, &m); err != nil {
		return nil, fmt.Errorf("parse manifest: %w", err)
	}
	if err := m.Validate(); err != nil {
		return nil, fmt.Errorf("manifest invalid: %w", err)
	}
	return &m, nil
}

// Validate enforces the basic shape constraints.
func (m *Manifest) Validate() error {
	if m.APIVersion != "velocity/v1" {
		return fmt.Errorf("unsupported apiVersion %q", m.APIVersion)
	}
	if m.Kind != "PluginRegistry" {
		return fmt.Errorf("unsupported kind %q", m.Kind)
	}
	if !tenantRegex.MatchString(m.Tenant) {
		return errors.New("tenant fails [a-z][a-z0-9-]{1,30}[a-z0-9]")
	}
	if len(m.Plugins) == 0 {
		return errors.New("plugins: must be non-empty")
	}
	seen := make(map[string]struct{}, len(m.Plugins))
	for i, p := range m.Plugins {
		if !idRegex.MatchString(p.ID) {
			return fmt.Errorf("plugins[%d].id %q is malformed", i, p.ID)
		}
		if _, dup := seen[p.ID]; dup {
			return fmt.Errorf("plugins[%d].id %q duplicated", i, p.ID)
		}
		seen[p.ID] = struct{}{}
		if p.Image == "" {
			return fmt.Errorf("plugins[%d].image required", i)
		}
		if p.Port == 0 {
			m.Plugins[i].Port = 50061
		}
	}
	return nil
}

// ResourceName returns the Kubernetes object name a plugin maps to.
// It hashes-and-truncates the (tenant, id) tuple so the names fit in
// the 63-char DNS-1123 limit no matter how long the plugin id is.
func ResourceName(tenant, id string) string {
	// Replace forbidden characters with '-' and truncate. Collisions
	// in the truncated form are very unlikely given the id regex, but
	// the daemon also appends a 6-char sha256 prefix in deployer.go.
	out := make([]byte, 0, 32)
	for _, c := range fmt.Sprintf("%s-%s", tenant, id) {
		switch {
		case c >= 'a' && c <= 'z', c >= '0' && c <= '9':
			out = append(out, byte(c))
		case c == '-' || c == '.' || c == '/':
			out = append(out, '-')
		}
	}
	if len(out) > 48 {
		out = out[:48]
	}
	return string(out)
}
