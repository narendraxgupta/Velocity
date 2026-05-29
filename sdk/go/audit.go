package velocity

import (
	"context"
	"net/url"
	"strconv"
	"time"
)

type AuditEvent struct {
	EventID      string         `json:"event_id"`
	TenantID     string         `json:"tenant_id"`
	Subject      string         `json:"subject"`
	Role         string         `json:"role"`
	Source       string         `json:"source"`
	Action       string         `json:"action"`
	ResourceType string         `json:"resource_type"`
	ResourceID   string         `json:"resource_id"`
	Outcome      string         `json:"outcome"`
	StatusCode   int            `json:"status_code"`
	OccurredAtNs int64          `json:"occurred_at_ns"`
	RemoteIP     string         `json:"remote_ip"`
	RequestID    string         `json:"request_id"`
	ChainHash    string         `json:"chain_hash"`
	Meta         map[string]any `json:"meta,omitempty"`
}

type AuditQuery struct {
	TenantID string
	Action   string
	Since    time.Time
	Until    time.Time
	Limit    int
}

type AuditResource struct{ c *Client }

func (r *AuditResource) Query(ctx context.Context, q *AuditQuery) ([]AuditEvent, error) {
	v := url.Values{}
	if q.TenantID != "" {
		v.Set("tenant", q.TenantID)
	}
	if q.Action != "" {
		v.Set("action", q.Action)
	}
	if !q.Since.IsZero() {
		v.Set("since", q.Since.Format(time.RFC3339))
	}
	if !q.Until.IsZero() {
		v.Set("until", q.Until.Format(time.RFC3339))
	}
	if q.Limit > 0 {
		v.Set("limit", strconv.Itoa(q.Limit))
	}
	var resp struct {
		Events []AuditEvent `json:"events"`
		Count  int          `json:"count"`
	}
	if err := r.c.do(ctx, "GET", "/v1/audit?"+v.Encode(), nil, &resp); err != nil {
		return nil, err
	}
	return resp.Events, nil
}
