// SPDX-License-Identifier: GPL-2.0-or-later

// Part of mayhem-b200.

package server

import (
	"embed"
	"io/fs"
)

//go:embed static/*
var staticFS embed.FS

// Assets returns the embedded frontend, rooted so that "index.html",
// "app.js" etc. are top-level paths (rather than "static/index.html").
func Assets() fs.FS {
	sub, err := fs.Sub(staticFS, "static")
	if err != nil {
		// static/ is embedded at build time by the go:embed directive above;
		// fs.Sub can only fail if that directory doesn't exist, which would
		// be a build-time failure, not a runtime one.
		panic("portal/server: embedded static/ missing: " + err.Error())
	}
	return sub
}
