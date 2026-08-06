// SPDX-License-Identifier: GPL-2.0-or-later

// Part of mayhem-b200.

package tiles

import (
	"bytes"
	"io/fs"
	"os"
	"path/filepath"
	"strings"
	"testing"
	"time"
)

// dirSize totals every file under dir, including any this package did not
// write — the point of a size budget is what the directory actually costs.
func dirSize(t *testing.T, dir string) int64 {
	t.Helper()
	var total int64
	err := filepath.WalkDir(dir, func(path string, d fs.DirEntry, err error) error {
		if err != nil {
			return err
		}
		if d.IsDir() {
			return nil
		}
		info, err := d.Info()
		if err != nil {
			return err
		}
		total += info.Size()
		return nil
	})
	if err != nil {
		t.Fatalf("walk %s: %v", dir, err)
	}
	return total
}

func TestCacheStoreLoadRoundTrip(t *testing.T) {
	clk := newTestClock()
	c := newCache(t.TempDir(), "provider", DefaultMaxCacheBytes, clk.now)

	tile := Tile{Z: 3, X: 4, Y: 5}
	body := fakePNG(0x11)
	m := entryMeta{
		URLTemplate: "https://example.invalid/{z}/{x}/{y}.png",
		ETag:        `"abc"`,
		FetchedAt:   clk.now(),
		FreshUntil:  clk.now().Add(time.Hour),
		Size:        int64(len(body)),
	}
	if err := c.store(tile, body, m); err != nil {
		t.Fatalf("store: %v", err)
	}

	gotBody, gotMeta, ok := c.load(tile)
	if !ok {
		t.Fatal("load: not found after store")
	}
	if !bytes.Equal(gotBody, body) {
		t.Fatalf("load body = %v, want %v", gotBody, body)
	}
	if gotMeta.ETag != m.ETag || !gotMeta.FreshUntil.Equal(m.FreshUntil) {
		t.Fatalf("load meta = %+v, want ETag %q FreshUntil %v", gotMeta, m.ETag, m.FreshUntil)
	}

	// A different tile, and a tile under a different provider prefix, must
	// not resolve to those bytes.
	if _, _, ok := c.load(Tile{Z: 3, X: 4, Y: 6}); ok {
		t.Error("load of a neighbouring tile found something")
	}
	other := newCache(c.root, "other-provider", DefaultMaxCacheBytes, clk.now)
	if _, _, ok := other.load(tile); ok {
		t.Error("a second provider found the first provider's tile")
	}
}

func TestCacheLoadIgnoresCorruptSidecar(t *testing.T) {
	clk := newTestClock()
	c := newCache(t.TempDir(), "provider", DefaultMaxCacheBytes, clk.now)
	tile := Tile{Z: 1, X: 0, Y: 1}
	if err := c.store(tile, fakePNG(0x22), entryMeta{FreshUntil: clk.now().Add(time.Hour)}); err != nil {
		t.Fatalf("store: %v", err)
	}
	if err := os.WriteFile(metaPathFor(c.path(tile)), []byte("{not json"), 0o644); err != nil {
		t.Fatalf("corrupt sidecar: %v", err)
	}
	if _, _, ok := c.load(tile); ok {
		t.Fatal("load returned an entry whose sidecar is unparseable")
	}
}

// TestCacheEvictsLeastRecentlyUsed pins down both halves of eviction: it
// gets back under budget, and it discards by last use rather than by age.
func TestCacheEvictsLeastRecentlyUsed(t *testing.T) {
	root := t.TempDir()
	clk := newTestClock()

	// A decoy immediately outside the cache root: eviction must never walk
	// out of its own directory.
	decoy := filepath.Join(filepath.Dir(root), "decoy-not-ours.png")
	if err := os.WriteFile(decoy, fakePNG(0xEE), 0o644); err != nil {
		t.Fatalf("write decoy: %v", err)
	}
	defer os.Remove(decoy)

	// Also a foreign file inside the cache root. This package only ever
	// deletes its own .png/.meta.json pairs.
	foreign := filepath.Join(root, "README-from-the-user.txt")
	if err := os.WriteFile(foreign, []byte("do not delete me"), 0o644); err != nil {
		t.Fatalf("write foreign file: %v", err)
	}

	body := bytes.Repeat([]byte{0}, 900)
	copy(body, pngSignature)
	// Budget for roughly two entries (900 bytes of image plus a ~150-byte
	// sidecar each), so storing the third has to evict.
	const budget = 2400
	c := newCache(root, "provider", budget, clk.now)

	tiles := []Tile{{Z: 4, X: 1, Y: 1}, {Z: 4, X: 2, Y: 2}, {Z: 4, X: 3, Y: 3}}
	for _, tile := range tiles[:2] {
		if err := c.store(tile, body, entryMeta{FreshUntil: clk.now().Add(time.Hour)}); err != nil {
			t.Fatalf("store %v: %v", tile, err)
		}
		clk.advance(time.Minute)
	}

	// Re-serving the first tile makes the SECOND one the least recently
	// used, which is the whole difference between LRU and "oldest".
	c.touch(tiles[0])
	clk.advance(time.Minute)

	if err := c.store(tiles[2], body, entryMeta{FreshUntil: clk.now().Add(time.Hour)}); err != nil {
		t.Fatalf("store %v: %v", tiles[2], err)
	}

	if got := dirSize(t, root); got > budget {
		t.Errorf("cache size after eviction = %d bytes, want <= %d", got, budget)
	}
	if _, _, ok := c.load(tiles[1]); ok {
		t.Error("least recently used tile survived eviction")
	}
	for _, tile := range []Tile{tiles[0], tiles[2]} {
		if _, _, ok := c.load(tile); !ok {
			t.Errorf("recently used tile %v was evicted", tile)
		}
	}
	if _, err := os.Stat(decoy); err != nil {
		t.Errorf("file outside the cache directory was removed: %v", err)
	}
	if _, err := os.Stat(foreign); err != nil {
		t.Errorf("foreign file inside the cache directory was removed: %v", err)
	}
}

func TestCacheRemoveInsideRefusesOutsideRoot(t *testing.T) {
	root := filepath.Join(t.TempDir(), "cache")
	if err := os.MkdirAll(root, 0o755); err != nil {
		t.Fatalf("mkdir: %v", err)
	}
	c := newCache(root, "provider", DefaultMaxCacheBytes, newTestClock().now)

	outside := filepath.Join(filepath.Dir(root), "outside.png")
	if err := os.WriteFile(outside, fakePNG(0x33), 0o644); err != nil {
		t.Fatalf("write: %v", err)
	}

	for _, path := range []string{
		outside,
		filepath.Join(root, "..", "outside.png"),
		filepath.Join(root, "provider", "..", "..", "outside.png"),
		root,
	} {
		if err := c.removeInside(path); err == nil {
			t.Errorf("removeInside(%q) = nil, want a refusal", path)
		} else if !strings.Contains(err.Error(), "refusing to delete") {
			t.Errorf("removeInside(%q) = %v, want a refusal", path, err)
		}
	}
	if _, err := os.Stat(outside); err != nil {
		t.Fatalf("file outside the cache root was deleted: %v", err)
	}
	if _, err := os.Stat(root); err != nil {
		t.Fatalf("the cache root itself was deleted: %v", err)
	}
}

func TestCacheTouchUpdatesLastUse(t *testing.T) {
	clk := newTestClock()
	c := newCache(t.TempDir(), "provider", DefaultMaxCacheBytes, clk.now)
	tile := Tile{Z: 2, X: 1, Y: 2}
	if err := c.store(tile, fakePNG(0x44), entryMeta{}); err != nil {
		t.Fatalf("store: %v", err)
	}

	_, entries, err := c.scan()
	if err != nil {
		t.Fatalf("scan: %v", err)
	}
	if len(entries) != 1 {
		t.Fatalf("scan found %d entries, want 1", len(entries))
	}
	stored := entries[0].used

	clk.advance(2 * time.Hour)
	c.touch(tile)

	_, entries, err = c.scan()
	if err != nil {
		t.Fatalf("scan: %v", err)
	}
	if !entries[0].used.After(stored) {
		t.Fatalf("last use after touch = %v, want later than %v", entries[0].used, stored)
	}
}
