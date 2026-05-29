package velocity

import (
	"bytes"
	"context"
	"encoding/json"
	"io"
)

type SubmissionKind string

const (
	SubmissionKindMatchingEngine SubmissionKind = "matching_engine"
	SubmissionKindMarketMaker    SubmissionKind = "market_maker"
	SubmissionKindPcapReplay     SubmissionKind = "pcap_replay"
)

type Submission struct {
	ID        string         `json:"id"`
	Team      string         `json:"team"`
	Display   string         `json:"display"`
	Kind      SubmissionKind `json:"kind"`
	Status    string         `json:"status"`
	CreatedAt int64          `json:"created_at_ns"`
}

type CreateSubmissionRequest struct {
	Team        string         `json:"team"`
	Display     string         `json:"display"`
	Kind        SubmissionKind `json:"kind"`
	SourceTarGz io.Reader      `json:"-"`
}

// SubmissionsResource bundles all /v1/submissions endpoints.
type SubmissionsResource struct{ c *Client }

// Create uploads source and registers a new submission. The artefact
// upload is a separate hop: the gateway first allocates an ID + signed
// upload URL, the SDK PUTs the source to MinIO, then the gateway is
// notified. We hide all that here so the caller just hands us a reader.
func (r *SubmissionsResource) Create(ctx context.Context, req *CreateSubmissionRequest) (*Submission, error) {
	body, err := json.Marshal(map[string]any{
		"team":    req.Team,
		"display": req.Display,
		"kind":    req.Kind,
	})
	if err != nil {
		return nil, err
	}
	var resp struct {
		Submission Submission `json:"submission"`
		UploadURL  string     `json:"upload_url"`
	}
	if err := r.c.do(ctx, "POST", "/v1/submissions",
		bytes.NewReader(body), &resp); err != nil {
		return nil, err
	}
	if req.SourceTarGz != nil && resp.UploadURL != "" {
		if err := r.upload(ctx, resp.UploadURL, req.SourceTarGz); err != nil {
			return nil, err
		}
		// Notify the gateway that upload completed. The route is a
		// no-op when the gateway is configured against an inline
		// MinIO-event hook, but explicitly notifying makes the SDK
		// portable across deployments.
		notifyBody, _ := json.Marshal(map[string]any{"id": resp.Submission.ID})
		if err := r.c.do(ctx, "POST",
			"/v1/submissions/"+resp.Submission.ID+"/uploaded",
			bytes.NewReader(notifyBody), nil); err != nil {
			return nil, err
		}
	}
	return &resp.Submission, nil
}

// Get fetches a submission by id.
func (r *SubmissionsResource) Get(ctx context.Context, id string) (*Submission, error) {
	var resp struct {
		Submission Submission `json:"submission"`
	}
	if err := r.c.do(ctx, "GET", "/v1/submissions/"+id, nil, &resp); err != nil {
		return nil, err
	}
	return &resp.Submission, nil
}

// List returns up to `limit` recent submissions for the current tenant.
func (r *SubmissionsResource) List(ctx context.Context, limit int) ([]Submission, error) {
	var resp struct {
		Submissions []Submission `json:"submissions"`
	}
	if err := r.c.do(ctx, "GET",
		"/v1/submissions?limit="+intToString(limit), nil, &resp); err != nil {
		return nil, err
	}
	return resp.Submissions, nil
}

func (r *SubmissionsResource) upload(ctx context.Context, url string, src io.Reader) error {
	// Direct PUT to the signed URL. We deliberately skip the SDK's
	// retry helper here — MinIO signed URLs are time-limited and a
	// long retry tail does more harm than good.
	body, err := io.ReadAll(src)
	if err != nil {
		return err
	}
	req, err := newPlainRequest(ctx, "PUT", url, body)
	if err != nil {
		return err
	}
	resp, err := r.c.http.Do(req)
	if err != nil {
		return err
	}
	defer resp.Body.Close()
	if resp.StatusCode >= 400 {
		return &APIError{StatusCode: resp.StatusCode, Message: "upload failed"}
	}
	return nil
}
