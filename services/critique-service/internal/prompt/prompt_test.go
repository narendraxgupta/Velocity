package prompt

import (
	"strings"
	"testing"
)

func TestUserPromptTemplate_EmbedsReportAndSource(t *testing.T) {
	ctx := SubmissionContext{
		SubmissionID:  "01H8X",
		Language:      "cpp",
		Kind:          "BINARY",
		Score:         87.4,
		ThroughputRps: 18432,
		P99LatencyNs:  312_000,
		Source:        "int main() {}\n",
	}
	out, err := UserPromptTemplate(ctx)
	if err != nil {
		t.Fatal(err)
	}
	if !strings.Contains(out, `"submission_id"`) {
		t.Fatalf("envelope missing submission_id field: %s", out)
	}
	if !strings.Contains(out, "int main()") {
		t.Fatalf("source not embedded")
	}
	if !strings.Contains(out, "Output ONLY the JSON object") {
		t.Fatalf("constraints missing")
	}
}

func TestUserPromptTemplate_TruncatesSource(t *testing.T) {
	huge := strings.Repeat("a", 200_000)
	out, err := UserPromptTemplate(SubmissionContext{
		SubmissionID: "S",
		Language:     "cpp",
		Source:       huge,
	})
	if err != nil {
		t.Fatal(err)
	}
	if !strings.Contains(out, "truncated") {
		t.Fatalf("expected truncation marker for 200kB source")
	}
}

func TestParseAndValidate_Happy(t *testing.T) {
	raw := "```json\n" + `{
		"summary": "ok",
		"strengths": ["a"],
		"risks": ["b"],
		"suggestions": [{"priority": "high", "title": "T", "rationale": "R"}],
		"microstructure": [],
		"latency": [],
		"correctness": []
	}` + "\n```\nthe model added some prose"
	c, _, err := ParseAndValidate(raw)
	if err != nil {
		t.Fatalf("parse: %v", err)
	}
	if c.Summary != "ok" {
		t.Fatalf("summary mismatch: %q", c.Summary)
	}
	if len(c.Suggestions) != 1 || c.Suggestions[0].Priority != "high" {
		t.Fatalf("suggestions parse failed: %+v", c.Suggestions)
	}
}

func TestParseAndValidate_RejectsBadPriority(t *testing.T) {
	raw := `{
		"summary": "ok",
		"strengths": [],
		"risks": [],
		"suggestions": [{"priority": "ULTRA", "title": "T", "rationale": "R"}],
		"microstructure": [],
		"latency": [],
		"correctness": []
	}`
	if _, _, err := ParseAndValidate(raw); err == nil {
		t.Fatal("expected invalid-priority error")
	}
}

func TestParseAndValidate_RejectsMissingSummary(t *testing.T) {
	raw := `{"strengths": [], "risks": [], "suggestions": [], "microstructure": [], "latency": [], "correctness": []}`
	if _, _, err := ParseAndValidate(raw); err == nil {
		t.Fatal("expected missing-summary error")
	}
}
