// Package dockerengine is a tiny, dependency-free client for the subset of
// the Docker Engine HTTP API the submission-engine's local (Codespace / dev)
// backend needs: build an image from a tar context, create/start/remove a
// container, and inspect it.
//
// We deliberately avoid github.com/docker/docker (the official SDK) — it drags
// in a very large dependency tree. The Engine API is a plain HTTP/JSON service
// exposed over a unix socket, so the standard library covers everything here.
package dockerengine

import (
	"bytes"
	"context"
	"encoding/json"
	"fmt"
	"io"
	"net"
	"net/http"
	"net/url"
	"strings"
	"time"
)

// Client talks to the Docker daemon over its unix socket.
type Client struct {
	httpc *http.Client
	base  string // dummy host; the unix dialer ignores it
}

// New constructs a Client. host accepts the docker-conventional
// "unix:///var/run/docker.sock" form (the only transport we support — the dev
// backend always speaks to a mounted socket).
func New(host string) (*Client, error) {
	socket := host
	switch {
	case strings.HasPrefix(host, "unix://"):
		socket = strings.TrimPrefix(host, "unix://")
	case host == "":
		socket = "/var/run/docker.sock"
	default:
		return nil, fmt.Errorf("dockerengine: unsupported DOCKER_HOST %q (only unix:// is supported)", host)
	}

	tr := &http.Transport{
		DialContext: func(ctx context.Context, _, _ string) (net.Conn, error) {
			var d net.Dialer
			return d.DialContext(ctx, "unix", socket)
		},
		DisableCompression: true,
		MaxIdleConns:       4,
		IdleConnTimeout:    30 * time.Second,
	}
	return &Client{
		httpc: &http.Client{Transport: tr},
		base:  "http://docker",
	}, nil
}

// do issues a request to the daemon. The caller owns resp.Body on success.
func (c *Client) do(ctx context.Context, method, path string, query url.Values,
	contentType string, body io.Reader) (*http.Response, error) {
	u := c.base + path
	if len(query) > 0 {
		u += "?" + query.Encode()
	}
	req, err := http.NewRequestWithContext(ctx, method, u, body)
	if err != nil {
		return nil, err
	}
	if contentType != "" {
		req.Header.Set("Content-Type", contentType)
	}
	return c.httpc.Do(req)
}

// apiError reads a daemon error body ({"message":"..."}) and renders it.
func apiError(resp *http.Response) error {
	b, _ := io.ReadAll(io.LimitReader(resp.Body, 64*1024))
	var m struct {
		Message string `json:"message"`
	}
	if json.Unmarshal(b, &m) == nil && m.Message != "" {
		return fmt.Errorf("docker api %s: %s", resp.Status, m.Message)
	}
	return fmt.Errorf("docker api %s: %s", resp.Status, strings.TrimSpace(string(b)))
}

// -----------------------------------------------------------------------------
//  Build
// -----------------------------------------------------------------------------

// BuildImage builds `tag` from a tar build context (Dockerfile at its root).
// The daemon returns HTTP 200 even for build failures — the actual outcome is
// streamed as JSON lines, so we scan for an error object.
func (c *Client) BuildImage(ctx context.Context, tag string, contextTar []byte,
	onLog func(string)) error {
	q := url.Values{}
	q.Set("t", tag)
	q.Set("rm", "1")
	q.Set("forcerm", "1")
	q.Set("pull", "0")

	resp, err := c.do(ctx, http.MethodPost, "/build", q,
		"application/x-tar", bytes.NewReader(contextTar))
	if err != nil {
		return fmt.Errorf("build request: %w", err)
	}
	defer resp.Body.Close()
	if resp.StatusCode != http.StatusOK {
		return apiError(resp)
	}

	dec := json.NewDecoder(resp.Body)
	for {
		var line struct {
			Stream      string `json:"stream"`
			Status      string `json:"status"`
			Error       string `json:"error"`
			ErrorDetail struct {
				Message string `json:"message"`
			} `json:"errorDetail"`
		}
		if err := dec.Decode(&line); err != nil {
			if err == io.EOF {
				break
			}
			return fmt.Errorf("decode build stream: %w", err)
		}
		if line.Error != "" {
			return fmt.Errorf("image build failed: %s", line.Error)
		}
		if onLog != nil {
			if s := strings.TrimRight(line.Stream, "\n"); s != "" {
				onLog(s)
			}
		}
	}
	return nil
}

// -----------------------------------------------------------------------------
//  Containers
// -----------------------------------------------------------------------------

// CreateSpec is the narrow set of container knobs the dev sandbox needs.
type CreateSpec struct {
	Name        string
	Image       string
	Env         []string
	Cmd         []string
	ServicePort uint16   // container port exposed for the bot fleet
	Network     string   // user-defined network to attach to (e.g. velocity-apps)
	Aliases     []string // network aliases (DNS names) on that network
	NanoCPUs    int64    // 0 == unlimited
	MemoryBytes int64    // 0 == unlimited
}

// CreateContainer creates (but does not start) a container, returning its id.
func (c *Client) CreateContainer(ctx context.Context, spec CreateSpec) (string, error) {
	portKey := fmt.Sprintf("%d/tcp", spec.ServicePort)

	hostCfg := map[string]any{
		"NetworkMode":   spec.Network,
		"RestartPolicy": map[string]any{"Name": "no"},
	}
	if spec.NanoCPUs > 0 {
		hostCfg["NanoCpus"] = spec.NanoCPUs
	}
	if spec.MemoryBytes > 0 {
		hostCfg["Memory"] = spec.MemoryBytes
	}

	payload := map[string]any{
		"Image":        spec.Image,
		"Env":          spec.Env,
		"ExposedPorts": map[string]any{portKey: map[string]any{}},
		"HostConfig":   hostCfg,
	}
	if len(spec.Cmd) > 0 {
		payload["Cmd"] = spec.Cmd
	}
	if spec.Network != "" {
		payload["NetworkingConfig"] = map[string]any{
			"EndpointsConfig": map[string]any{
				spec.Network: map[string]any{"Aliases": spec.Aliases},
			},
		}
	}

	buf, err := json.Marshal(payload)
	if err != nil {
		return "", err
	}
	q := url.Values{}
	if spec.Name != "" {
		q.Set("name", spec.Name)
	}
	resp, err := c.do(ctx, http.MethodPost, "/containers/create", q,
		"application/json", bytes.NewReader(buf))
	if err != nil {
		return "", fmt.Errorf("create request: %w", err)
	}
	defer resp.Body.Close()
	if resp.StatusCode != http.StatusCreated {
		return "", apiError(resp)
	}
	var out struct {
		ID string `json:"Id"`
	}
	if err := json.NewDecoder(resp.Body).Decode(&out); err != nil {
		return "", fmt.Errorf("decode create: %w", err)
	}
	return out.ID, nil
}

// StartContainer starts a previously-created container.
func (c *Client) StartContainer(ctx context.Context, id string) error {
	resp, err := c.do(ctx, http.MethodPost, "/containers/"+id+"/start", nil, "", nil)
	if err != nil {
		return fmt.Errorf("start request: %w", err)
	}
	defer resp.Body.Close()
	// 204 == started, 304 == already started.
	if resp.StatusCode != http.StatusNoContent && resp.StatusCode != http.StatusNotModified {
		return apiError(resp)
	}
	return nil
}

// RemoveContainer force-removes a container (and its anonymous volumes). A
// missing container is treated as success so callers can use it for cleanup.
func (c *Client) RemoveContainer(ctx context.Context, idOrName string) error {
	q := url.Values{}
	q.Set("force", "1")
	q.Set("v", "1")
	resp, err := c.do(ctx, http.MethodDelete, "/containers/"+idOrName, q, "", nil)
	if err != nil {
		return fmt.Errorf("remove request: %w", err)
	}
	defer resp.Body.Close()
	if resp.StatusCode == http.StatusNoContent || resp.StatusCode == http.StatusNotFound {
		return nil
	}
	return apiError(resp)
}

// ContainerState is the slice of `docker inspect` we care about.
type ContainerState struct {
	Running   bool
	ExitCode  int
	Error     string
	OOMKilled bool
}

// InspectState returns the container's current state.
func (c *Client) InspectState(ctx context.Context, idOrName string) (*ContainerState, error) {
	resp, err := c.do(ctx, http.MethodGet, "/containers/"+idOrName+"/json", nil, "", nil)
	if err != nil {
		return nil, fmt.Errorf("inspect request: %w", err)
	}
	defer resp.Body.Close()
	if resp.StatusCode != http.StatusOK {
		return nil, apiError(resp)
	}
	var out struct {
		State struct {
			Running   bool   `json:"Running"`
			ExitCode  int    `json:"ExitCode"`
			Error     string `json:"Error"`
			OOMKilled bool   `json:"OOMKilled"`
		} `json:"State"`
	}
	if err := json.NewDecoder(resp.Body).Decode(&out); err != nil {
		return nil, fmt.Errorf("decode inspect: %w", err)
	}
	return &ContainerState{
		Running:   out.State.Running,
		ExitCode:  out.State.ExitCode,
		Error:     out.State.Error,
		OOMKilled: out.State.OOMKilled,
	}, nil
}

// Ping verifies the daemon is reachable.
func (c *Client) Ping(ctx context.Context) error {
	resp, err := c.do(ctx, http.MethodGet, "/_ping", nil, "", nil)
	if err != nil {
		return err
	}
	defer resp.Body.Close()
	if resp.StatusCode != http.StatusOK {
		return apiError(resp)
	}
	return nil
}
