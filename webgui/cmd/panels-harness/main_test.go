// SPDX-License-Identifier: GPL-2.0-or-later

// Part of mayhem-b200.

package main

import (
	"encoding/json"
	"net/http"
	"net/http/httptest"
	"os"
	"path/filepath"
	"testing"
)

// TestBackendTableFixtureIsTheFlatCppShape pins testdata/table_backend.json to
// the shape src/remote/app_data.cpp's to_json(TableData) actually emits --
// column labels as bare strings and rows as positional arrays, with no per-row
// key and no updated_at_ms. That is NOT the shape PANELS.md documents for the
// table panel, and the two went unreconciled long enough for every table panel
// the C++ backend published (AIS, BLE, EPIRB, ERT, NRF, Search, SubCar,
// 2-Tone, Weather, APRS) to render with blank headers and empty cells in the
// portal. panels/table.js now normalizes it; this fixture is what that path is
// exercised against, so "modernizing" it into the documented shape would
// silently delete the only coverage of the shape the backend really sends.
func TestBackendTableFixtureIsTheFlatCppShape(t *testing.T) {
	root, err := defaultPortalDir()
	if err != nil {
		t.Fatalf("defaultPortalDir: %v", err)
	}
	raw, err := os.ReadFile(filepath.Join(root, "testdata", "table_backend.json"))
	if err != nil {
		t.Fatalf("read table_backend.json: %v", err)
	}

	var flat struct {
		Columns []string   `json:"columns"`
		Rows    [][]string `json:"rows"`
	}
	if err := json.Unmarshal(raw, &flat); err != nil {
		t.Fatalf("table_backend.json is not the flat backend shape: %v", err)
	}
	if len(flat.Columns) == 0 {
		t.Fatal("table_backend.json has no columns")
	}
	if len(flat.Rows) == 0 {
		t.Fatal("table_backend.json has no rows")
	}
	for i, row := range flat.Rows {
		if len(row) != len(flat.Columns) {
			t.Errorf("row %d has %d cells, want %d (one per column)", i, len(row), len(flat.Columns))
		}
	}

	// An absent value must stay absent: the AIS app leaves Name/Call blank for
	// a vessel that has broadcast neither, and the fixture has to keep an
	// example of that rather than inventing a name for it.
	blank := false
	for _, row := range flat.Rows {
		for _, cell := range row {
			if cell == "" {
				blank = true
			}
		}
	}
	if !blank {
		t.Error("table_backend.json has no empty cell; it no longer covers absent data")
	}
}

func TestNewMux_ServesHarnessAndAssets(t *testing.T) {
	root, err := defaultPortalDir()
	if err != nil {
		t.Fatalf("defaultPortalDir: %v", err)
	}

	mux, err := newMux(root)
	if err != nil {
		t.Fatalf("newMux(%q): %v", root, err)
	}
	ts := httptest.NewServer(mux)
	defer ts.Close()

	paths := []string{
		"/harness.html",
		"/server/static/panels/registry.js",
		"/server/static/panels/panels.css",
		"/testdata/table.json",
	}
	for _, path := range paths {
		resp, err := http.Get(ts.URL + path)
		if err != nil {
			t.Fatalf("GET %s: %v", path, err)
		}
		resp.Body.Close()
		if resp.StatusCode != http.StatusOK {
			t.Errorf("GET %s: status = %d, want 200", path, resp.StatusCode)
		}
	}
}

// TestFixturesAreServedAsIs pins down that every testdata/*.json fixture
// PANELS.md and harness.html rely on is actually present and served, rather
// than only existing at write time.
func TestFixturesAreServedAsIs(t *testing.T) {
	root, err := defaultPortalDir()
	if err != nil {
		t.Fatalf("defaultPortalDir: %v", err)
	}
	mux, err := newMux(root)
	if err != nil {
		t.Fatalf("newMux(%q): %v", root, err)
	}
	ts := httptest.NewServer(mux)
	defer ts.Close()

	fixtures := []string{
		"table.json", "table_backend.json", "spectrum_active.json", "spectrum_idle.json",
		"receiver.json", "console.json", "map.json", "screen.json",
	}
	for _, name := range fixtures {
		resp, err := http.Get(ts.URL + "/testdata/" + name)
		if err != nil {
			t.Fatalf("GET testdata/%s: %v", name, err)
		}
		resp.Body.Close()
		if resp.StatusCode != http.StatusOK {
			t.Errorf("GET testdata/%s: status = %d, want 200", name, resp.StatusCode)
		}
	}
}

func TestNewMux_MissingHarness(t *testing.T) {
	dir := t.TempDir()
	if _, err := newMux(dir); err == nil {
		t.Fatalf("newMux(%q) with no harness.html: want error, got nil", dir)
	}
}

func TestDefaultPortalDir_WalksUpToGoMod(t *testing.T) {
	root := t.TempDir()
	if err := os.WriteFile(filepath.Join(root, "go.mod"), []byte("module test\n"), 0o644); err != nil {
		t.Fatalf("write go.mod: %v", err)
	}
	nested := filepath.Join(root, "cmd", "somewhere")
	if err := os.MkdirAll(nested, 0o755); err != nil {
		t.Fatalf("mkdir: %v", err)
	}

	oldWd, err := os.Getwd()
	if err != nil {
		t.Fatalf("Getwd: %v", err)
	}
	defer func() {
		if err := os.Chdir(oldWd); err != nil {
			t.Fatalf("restore cwd: %v", err)
		}
	}()
	if err := os.Chdir(nested); err != nil {
		t.Fatalf("Chdir: %v", err)
	}

	got, err := defaultPortalDir()
	if err != nil {
		t.Fatalf("defaultPortalDir: %v", err)
	}
	want := filepath.Join(root, "internal", "portal")
	if got != want {
		t.Fatalf("defaultPortalDir() = %q, want %q", got, want)
	}
}
