package velocity

import (
	"context"
)

type LeaderboardEntry struct {
	Rank           int     `json:"rank"`
	SubmissionID   string  `json:"submission_id"`
	Team           string  `json:"team"`
	Display        string  `json:"display"`
	CompositeScore float64 `json:"composite_score"`
	LatencyScore   float64 `json:"latency_score"`
	ThroughputScore float64 `json:"throughput_score"`
	Profile        Profile `json:"profile"`
	UpdatedAtNs    int64   `json:"updated_at_ns"`
}

type LeaderboardResource struct{ c *Client }

func (r *LeaderboardResource) Top(ctx context.Context, limit int) ([]LeaderboardEntry, error) {
	var resp struct {
		Entries []LeaderboardEntry `json:"entries"`
	}
	if err := r.c.do(ctx, "GET",
		"/v1/leaderboard?limit="+intToString(limit), nil, &resp); err != nil {
		return nil, err
	}
	return resp.Entries, nil
}
