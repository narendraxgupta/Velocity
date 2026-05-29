// Package tenant provides multi-tenant primitives shared by Velocity's
// Go services.
//
// What's here
//   - TenantID type — just a tagged string, but it stops you from
//     accidentally using a submission_id where a tenant_id was needed.
//   - JWT extraction from an incoming HTTP request or gRPC metadata.
//   - Key-scoping helpers for Redis, Kafka, MinIO. Same conventions
//     as services/api-gateway/include/api_gateway/tenant.h so the C++
//     and Go sides see the SAME keys for the SAME tenant.
//
// Wire formats
//
//	Redis keys :  t:<tenant_id>:<original_key>
//	Topics     :  t.<tenant_id>.<original_topic>
//	MinIO paths:  t/<tenant_id>/<original_path>
//
// All helpers are idempotent: a key already carrying its tenant prefix
// passes through unchanged. This keeps the migration safe — we can
// flip services over one at a time without double-prefixing.
package tenant

import (
	"context"
	"encoding/base64"
	"encoding/json"
	"errors"
	"fmt"
	"net/http"
	"strings"
	"time"

	"crypto/hmac"
	"crypto/sha256"

	"google.golang.org/grpc/metadata"
)

// TenantID is a tagged string so misuses surface at compile time.
type TenantID string

const (
	DefaultTenant TenantID = "default"

	HeaderAuthorization = "Authorization"
	HeaderXTenant       = "X-Velocity-Tenant"
)

// Context carries the resolved tenant information through a handler.
type Context struct {
	ID        TenantID
	Subject   string
	Role      string
	IssuedAt  time.Time
	ExpiresAt time.Time
}

// Config wires verification.
type Config struct {
	// HS256 shared secret. Empty disables JWT (fallback to header /
	// default tenant). Encoded as base64url, matching the gateway.
	HS256SecretB64 string
	// When true a request with no resolvable tenant is rejected at the
	// transport layer. The HTTP path returns 401, the gRPC path returns
	// Unauthenticated.
	RequireAuth bool
}

// FromHTTPRequest resolves a tenant from an HTTP request using the
// same priority order as the C++ gateway:
//  1. Authorization: Bearer <JWT>
//  2. X-Velocity-Tenant header
//  3. cfg.RequireAuth → error; else DefaultTenant
func FromHTTPRequest(r *http.Request, cfg Config) (*Context, error) {
	if cfg.HS256SecretB64 != "" {
		if h := r.Header.Get(HeaderAuthorization); strings.HasPrefix(h, "Bearer ") {
			ctx, err := verifyHS256(strings.TrimPrefix(h, "Bearer "), cfg.HS256SecretB64)
			if err == nil {
				return ctx, nil
			}
			// A presented-but-invalid token is terminal. We must NOT fall
			// through to the X-Velocity-Tenant header — any client can set
			// that to an arbitrary tenant, so the downgrade would let a
			// forged/expired JWT impersonate a victim (even with
			// RequireAuth=true, since the header check used to run first).
			if cfg.RequireAuth {
				return nil, fmt.Errorf("invalid bearer token: %w", err)
			}
			return &Context{ID: DefaultTenant, Role: "submitter"}, nil
		}
	}
	if t := r.Header.Get(HeaderXTenant); t != "" {
		return &Context{ID: TenantID(t), Role: "submitter"}, nil
	}
	if cfg.RequireAuth {
		return nil, errors.New("authentication required")
	}
	return &Context{ID: DefaultTenant, Role: "submitter"}, nil
}

// FromGRPCContext resolves a tenant from incoming gRPC metadata. Same
// priority order as the HTTP path; metadata keys are lowercase per
// gRPC convention.
func FromGRPCContext(ctx context.Context, cfg Config) (*Context, error) {
	md, ok := metadata.FromIncomingContext(ctx)
	if !ok {
		if cfg.RequireAuth {
			return nil, errors.New("no metadata on request")
		}
		return &Context{ID: DefaultTenant, Role: "submitter"}, nil
	}
	if cfg.HS256SecretB64 != "" {
		auth := first(md.Get("authorization"))
		if strings.HasPrefix(auth, "Bearer ") {
			t, err := verifyHS256(strings.TrimPrefix(auth, "Bearer "), cfg.HS256SecretB64)
			if err == nil {
				return t, nil
			}
			// Terminal: a presented-but-invalid token never downgrades to
			// the spoofable x-velocity-tenant metadata key.
			if cfg.RequireAuth {
				return nil, fmt.Errorf("invalid bearer token: %w", err)
			}
			return &Context{ID: DefaultTenant, Role: "submitter"}, nil
		}
	}
	if t := first(md.Get("x-velocity-tenant")); t != "" {
		return &Context{ID: TenantID(t), Role: "submitter"}, nil
	}
	if cfg.RequireAuth {
		return nil, errors.New("authentication required")
	}
	return &Context{ID: DefaultTenant, Role: "submitter"}, nil
}

// alreadyScoped checks for the *exact* `<sigil><tenant><sep>` prefix.
// A bare sigil match is not enough: skipping the prefix on a key that
// already begins with `t:` would let `ScopedKey("acme", "t:other:foo")`
// leak the `other` tenant's key out to `acme` callers. We have to
// fail closed.
func alreadyScoped(value string, t TenantID, sigil string, sep byte) bool {
	prefix := sigil + string(t)
	if !strings.HasPrefix(value, prefix) {
		return false
	}
	if len(value) <= len(prefix) {
		return false
	}
	return value[len(prefix)] == sep
}

// ScopedKey is the canonical Redis key helper.
func ScopedKey(t TenantID, raw string) string {
	if alreadyScoped(raw, t, "t:", ':') {
		return raw
	}
	return "t:" + string(t) + ":" + raw
}

// ScopedTopic is the canonical Kafka/Redpanda topic helper.
func ScopedTopic(t TenantID, raw string) string {
	if alreadyScoped(raw, t, "t.", '.') {
		return raw
	}
	return "t." + string(t) + "." + raw
}

// ScopedObject is the canonical MinIO object key / prefix helper.
func ScopedObject(t TenantID, raw string) string {
	if alreadyScoped(raw, t, "t/", '/') {
		return raw
	}
	return "t/" + string(t) + "/" + raw
}

// ----------------------------------------------------------------------------
//  Internal: HS256 JWT parse + verify.
// ----------------------------------------------------------------------------

func verifyHS256(token, secretB64 string) (*Context, error) {
	parts := strings.Split(token, ".")
	if len(parts) != 3 {
		return nil, errors.New("malformed JWT")
	}
	headerJSON, err := base64.RawURLEncoding.DecodeString(parts[0])
	if err != nil {
		return nil, fmt.Errorf("header decode: %w", err)
	}
	var hdr struct {
		Alg string `json:"alg"`
		Typ string `json:"typ"`
	}
	if err := json.Unmarshal(headerJSON, &hdr); err != nil {
		return nil, err
	}
	if hdr.Alg != "HS256" {
		return nil, errors.New("unsupported alg")
	}
	sig, err := base64.RawURLEncoding.DecodeString(parts[2])
	if err != nil {
		return nil, fmt.Errorf("sig decode: %w", err)
	}
	key, err := base64.RawURLEncoding.DecodeString(secretB64)
	if err != nil {
		return nil, fmt.Errorf("secret decode: %w", err)
	}
	signed := parts[0] + "." + parts[1]
	mac := hmac.New(sha256.New, key)
	mac.Write([]byte(signed))
	expected := mac.Sum(nil)
	if !hmac.Equal(expected, sig) {
		return nil, errors.New("bad signature")
	}
	payloadJSON, err := base64.RawURLEncoding.DecodeString(parts[1])
	if err != nil {
		return nil, fmt.Errorf("payload decode: %w", err)
	}
	var claims struct {
		Tid  string `json:"tid"`
		Sub  string `json:"sub"`
		Role string `json:"role"`
		Iat  int64  `json:"iat"`
		Exp  int64  `json:"exp"`
	}
	if err := json.Unmarshal(payloadJSON, &claims); err != nil {
		return nil, err
	}
	if claims.Exp > 0 && time.Now().Unix() >= claims.Exp {
		return nil, errors.New("token expired")
	}
	tid := TenantID(claims.Tid)
	if tid == "" {
		tid = DefaultTenant
	}
	return &Context{
		ID:        tid,
		Subject:   claims.Sub,
		Role:      claims.Role,
		IssuedAt:  time.Unix(claims.Iat, 0),
		ExpiresAt: time.Unix(claims.Exp, 0),
	}, nil
}

func first(vs []string) string {
	if len(vs) == 0 {
		return ""
	}
	return vs[0]
}
