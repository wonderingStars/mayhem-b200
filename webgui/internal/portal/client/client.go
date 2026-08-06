// SPDX-License-Identifier: GPL-2.0-or-later

// Part of mayhem-b200.

package client

import (
	"context"
	"encoding/json"
	"fmt"
	"io"
	"net/http"
	"net/url"
	"strings"
	"time"
)

// DefaultTimeout bounds every request this client makes, end to end
// (dial + write + read). mayhem-b200 is a local process on the same
// machine; there is no legitimate reason for any of these calls to take
// long, and a caller (the portal HTTP server, ultimately a browser tab
// waiting on a poll) must never be left hanging because the C++ process is
// wedged or simply not running.
const DefaultTimeout = 3 * time.Second

// Error is returned by every Client method on failure. It distinguishes
// "mayhem-b200 is not reachable at all" (Status == 0: dial/timeout/DNS
// failure) from "mayhem-b200 answered with an error" (Status is the HTTP
// status it sent), so callers — chiefly the portal server's handlers — can
// render an honest, specific message instead of a generic failure.
type Error struct {
	// Op names the call that failed, e.g. "apps", "launch".
	Op string
	// Status is the HTTP status mayhem-b200 responded with, or 0 if no
	// response was ever received (connection refused, timeout, DNS error).
	Status int
	// DecodeFailed is set when mayhem-b200 responded with a successful
	// status but a body this client could not parse as the expected JSON
	// shape.
	DecodeFailed bool
	// Message is a short, human-readable description safe to show in the
	// UI: either extracted from mayhem-b200's own {"error":"..."} body, or
	// a generic description of the failure.
	Message string
	// Err is the underlying error for network-level failures (Status == 0).
	// Nil when Status != 0.
	Err error
}

func (e *Error) Error() string {
	if e.Status == 0 {
		return fmt.Sprintf("mayhem-b200 %s: %s", e.Op, e.Message)
	}
	return fmt.Sprintf("mayhem-b200 %s: %s (status %d)", e.Op, e.Message, e.Status)
}

func (e *Error) Unwrap() error { return e.Err }

// Unavailable reports whether mayhem-b200 could not be reached at all, as
// opposed to reaching it and getting an error back. Callers use this to
// choose between "mayhem-b200 is not running" and a more specific message.
func (e *Error) Unavailable() bool { return e.Status == 0 }

// Client talks to mayhem-b200's app-portal HTTP API (package doc for the
// endpoint contract). It holds no other state and is safe for concurrent
// use.
type Client struct {
	baseURL    string
	httpClient *http.Client
}

// New returns a Client for the app-portal API at baseURL (e.g.
// "http://127.0.0.1:8090"). A timeout <= 0 uses DefaultTimeout.
func New(baseURL string, timeout time.Duration) *Client {
	if timeout <= 0 {
		timeout = DefaultTimeout
	}
	return &Client{
		baseURL:    strings.TrimRight(baseURL, "/"),
		httpClient: &http.Client{Timeout: timeout},
	}
}

// Apps fetches the full, unfiltered app list.
func (c *Client) Apps(ctx context.Context) ([]App, error) {
	var resp AppsResponse
	if err := c.do(ctx, "apps", http.MethodGet, "/api/apps", nil, &resp); err != nil {
		return nil, err
	}
	return resp.Apps, nil
}

// CurrentApp fetches what the navigation stack currently has on top.
func (c *Client) CurrentApp(ctx context.Context) (CurrentApp, error) {
	var resp CurrentApp
	if err := c.do(ctx, "current app", http.MethodGet, "/api/apps/current", nil, &resp); err != nil {
		return CurrentApp{}, err
	}
	return resp, nil
}

// Launch opens the app with the given registry id, mirroring a tap on its
// menu icon. Returns the resulting navigation state.
func (c *Client) Launch(ctx context.Context, id string) (CurrentApp, error) {
	path := "/api/apps/" + url.PathEscape(id) + "/launch"
	var resp CurrentApp
	if err := c.do(ctx, "launch", http.MethodPost, path, nil, &resp); err != nil {
		return CurrentApp{}, err
	}
	return resp, nil
}

// Home pops the navigation stack back to the main menu.
func (c *Client) Home(ctx context.Context) (CurrentApp, error) {
	var resp CurrentApp
	if err := c.do(ctx, "home", http.MethodPost, "/api/apps/home", nil, &resp); err != nil {
		return CurrentApp{}, err
	}
	return resp, nil
}

// Panel fetches whatever structured data the currently active app has
// published for display, if any (see Panel.HasData).
func (c *Client) Panel(ctx context.Context) (Panel, error) {
	var resp Panel
	if err := c.do(ctx, "panel", http.MethodGet, "/api/panel", nil, &resp); err != nil {
		return Panel{}, err
	}
	return resp, nil
}

// Status fetches the header-bar state (device, RX/TX activity, version).
func (c *Client) Status(ctx context.Context) (Status, error) {
	var resp Status
	if err := c.do(ctx, "status", http.MethodGet, "/api/status", nil, &resp); err != nil {
		return Status{}, err
	}
	return resp, nil
}

// do performs one request/response round trip and decodes the JSON body
// into out. It never returns a bare error from net/http or encoding/json:
// every failure is normalized into an *Error so callers can branch on
// Unavailable() without knowing this package's transport details.
func (c *Client) do(ctx context.Context, op, method, path string, body io.Reader, out any) error {
	req, err := http.NewRequestWithContext(ctx, method, c.baseURL+path, body)
	if err != nil {
		return &Error{Op: op, Message: "invalid request: " + err.Error(), Err: err}
	}
	if body != nil {
		req.Header.Set("Content-Type", "application/json")
	}
	req.Header.Set("Accept", "application/json")

	resp, err := c.httpClient.Do(req)
	if err != nil {
		// Every transport-level failure (connection refused because the
		// process isn't running, DNS failure, TLS failure, or our own
		// timeout firing) lands here. From this client's point of view they
		// all mean the same thing: mayhem-b200 could not be reached.
		return &Error{Op: op, Message: "mayhem-b200 is not running or not reachable at " + c.baseURL, Err: err}
	}
	defer resp.Body.Close()

	const maxBody = 1 << 20 // 1 MiB is generous for any of these payloads; caps a misbehaving/huge response.
	data, err := io.ReadAll(io.LimitReader(resp.Body, maxBody))
	if err != nil {
		return &Error{Op: op, Status: resp.StatusCode, Message: "reading response: " + err.Error(), Err: err}
	}

	if resp.StatusCode < 200 || resp.StatusCode >= 300 {
		return &Error{Op: op, Status: resp.StatusCode, Message: extractErrorMessage(data, resp.StatusCode)}
	}

	if out == nil || len(data) == 0 {
		return nil
	}
	if err := json.Unmarshal(data, out); err != nil {
		return &Error{
			Op:           op,
			Status:       resp.StatusCode,
			DecodeFailed: true,
			Message:      "malformed response from mayhem-b200",
			Err:          err,
		}
	}
	return nil
}

// extractErrorMessage tries to pull a message out of a JSON
// {"error":"..."} body (the shape every handler in this project uses); it
// falls back to a generic message keyed on the status code when the body
// isn't that shape (or isn't JSON at all).
func extractErrorMessage(body []byte, status int) string {
	var e struct {
		Error string `json:"error"`
	}
	if err := json.Unmarshal(body, &e); err == nil && e.Error != "" {
		return e.Error
	}
	return fmt.Sprintf("mayhem-b200 returned status %d", status)
}
