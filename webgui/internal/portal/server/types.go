// SPDX-License-Identifier: GPL-2.0-or-later

// Part of mayhem-b200.

package server

import (
	"mayhemb200/webgui/internal/portal/appindex"
	"mayhemb200/webgui/internal/portal/client"
)

// appsResponse is the portal's own GET /api/apps payload: the backend's
// flat app list plus this server's category grouping (internal/portal/
// appindex), so the browser never has to re-derive it. Apps is still
// included, flat, for the client-side search index.
type appsResponse struct {
	Apps   []client.App             `json:"apps"`
	Groups []appindex.CategoryGroup `json:"groups"`
}

// errorResponse is the JSON body of every non-2xx response this server
// sends. Code is a short machine-readable token (currently only set for
// "backend not running", the one case app.js branches on specifically);
// Error is always present and safe to show directly to a user.
type errorResponse struct {
	Error string `json:"error"`
	Code  string `json:"code,omitempty"`
}
