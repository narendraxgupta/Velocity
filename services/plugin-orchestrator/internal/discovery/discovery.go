// Package discovery publishes the active plugin set to Redis so the
// correctness-validator can pick it up without watching Kubernetes.
//
// Wire format
// -----------
//
//   Key: plugins:registry:<tenant>     (Redis Hash)
//   Field:  <plugin_id>
//   Value:  JSON {
//     "endpoint": "tenant-acme-stp-strict-1-2-0-7fa2c1.velocity-tenant-acme.svc.cluster.local:50061",
//     "enabled":  true,
//     "events":   ["EVENT_KIND_ORDER", "EVENT_KIND_FILL"]
//   }
//
// The correctness-validator polls this key once per submission with
// HGETALL. We don't bother with PubSub: the polling cost is tiny
// (handful of plugins per tenant, polled at submission start) and
// keeps the validator restart-resilient.
package discovery

import (
	"context"
	"crypto/sha256"
	"encoding/hex"
	"encoding/json"
	"fmt"
	"time"

	"github.com/redis/go-redis/v9"

	"github.com/velocity/platform/services/plugin-orchestrator/internal/registry"
)

type Publisher struct {
	rdb *redis.Client
}

func New(rdb *redis.Client) *Publisher { return &Publisher{rdb: rdb} }

type Entry struct {
	Endpoint string   `json:"endpoint"`
	Enabled  bool     `json:"enabled"`
	Events   []string `json:"events,omitempty"`
}

func (p *Publisher) Publish(ctx context.Context, m *registry.Manifest) error {
	key := fmt.Sprintf("plugins:registry:%s", m.Tenant)
	values := make(map[string]any, len(m.Plugins))
	for _, plugin := range m.Plugins {
		name := suffixedDNSName(m.Tenant, plugin.ID)
		entry := Entry{
			Endpoint: fmt.Sprintf("%s.velocity-tenant-%s.svc.cluster.local:%d",
				name, m.Tenant, plugin.Port),
			Enabled: plugin.Enabled,
			Events:  plugin.Events,
		}
		body, err := json.Marshal(entry)
		if err != nil {
			return err
		}
		values[plugin.ID] = body
	}
	// Atomic refresh: write the new set, drop fields no longer present.
	pipe := p.rdb.TxPipeline()
	pipe.Del(ctx, key)
	if len(values) > 0 {
		pipe.HSet(ctx, key, values)
	}
	pipe.Expire(ctx, key, 24*time.Hour)
	_, err := pipe.Exec(ctx)
	return err
}

// Match the resource name shape produced by deployer.suffixHashed,
// so the endpoint we publish resolves to the actual Service.
// The 6-char hex prefix is sha256(id) — see deployer.suffixHashed.
func suffixedDNSName(tenant, id string) string {
	base := registry.ResourceName(tenant, id)
	h := sha256.Sum256([]byte(id))
	return fmt.Sprintf("%s-%s", base, hex.EncodeToString(h[:3]))
}
