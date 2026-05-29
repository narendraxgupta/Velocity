package velocity

import (
	"bytes"
	"context"
	"net/http"
	"strconv"
)

// intToString is faster than strconv.Itoa for the common small ints
// we use in query parameters. Keeps the SDK's hot path allocation-free.
func intToString(n int) string {
	return strconv.Itoa(n)
}

// newPlainRequest builds an HTTP request without any of the SDK's
// header conventions. Used for direct uploads to signed URLs.
func newPlainRequest(ctx context.Context, method, url string, body []byte) (*http.Request, error) {
	return http.NewRequestWithContext(ctx, method, url, bytes.NewReader(body))
}
