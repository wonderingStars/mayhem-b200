// SPDX-License-Identifier: GPL-2.0-or-later

// Part of mayhem-b200.

package tiles

import (
	"encoding/json"
	"fmt"
	"io/fs"
	"os"
	"path/filepath"
	"sort"
	"strings"
	"sync"
	"time"
)

// metaSuffix names the sidecar file holding a cached tile's validators and
// freshness window. A sidecar rather than a header prepended to the image
// keeps the cache directory full of ordinary .png files that can be opened
// and eyeballed when something looks wrong.
const metaSuffix = ".meta.json"

// entryMeta is the sidecar's content.
type entryMeta struct {
	// URLTemplate records which provider these bytes came from. The cache
	// path already separates providers by hash; this is the belt-and-braces
	// check that survives a hash collision or a hand-edited cache.
	URLTemplate  string    `json:"url_template"`
	ETag         string    `json:"etag,omitempty"`
	LastModified string    `json:"last_modified,omitempty"`
	FetchedAt    time.Time `json:"fetched_at"`
	FreshUntil   time.Time `json:"fresh_until"`
	Size         int64     `json:"size"`
}

// cache is the on-disk tile store: root/<template hash>/z/x/y.png plus a
// sidecar. It is safe for concurrent use; concurrent writes to one tile are
// prevented a level up by the Proxy's in-flight coalescing, and writes are
// atomic (temp file + rename) so a crash mid-write cannot leave a truncated
// PNG behind to be served for a week.
type cache struct {
	root     string // configured cache directory; nothing outside it is ever touched
	prefix   string // per-provider subdirectory (see templateHash)
	maxBytes int64
	now      func() time.Time

	mu    sync.Mutex
	bytes int64 // running total of root's size, valid once sized
	sized bool
}

func newCache(root, prefix string, maxBytes int64, now func() time.Time) *cache {
	return &cache{root: root, prefix: prefix, maxBytes: maxBytes, now: now}
}

// path is the tile's file. Every component after root comes from validated
// integers (see ParsePath), so this cannot escape root.
func (c *cache) path(t Tile) string {
	return filepath.Join(append([]string{c.root, c.prefix}, t.relPath()...)...)
}

func metaPathFor(tilePath string) string {
	return strings.TrimSuffix(tilePath, ".png") + metaSuffix
}

// load returns the cached bytes and metadata for a tile. A tile whose
// sidecar is missing, corrupt, or from a different provider counts as
// absent: it will simply be fetched again.
func (c *cache) load(t Tile) ([]byte, entryMeta, bool) {
	p := c.path(t)
	raw, err := os.ReadFile(metaPathFor(p))
	if err != nil {
		return nil, entryMeta{}, false
	}
	var m entryMeta
	if err := json.Unmarshal(raw, &m); err != nil {
		return nil, entryMeta{}, false
	}
	body, err := os.ReadFile(p)
	if err != nil {
		return nil, entryMeta{}, false
	}
	return body, m, true
}

// store writes a freshly downloaded tile and its sidecar, then charges it
// against the size budget.
func (c *cache) store(t Tile, body []byte, m entryMeta) error {
	p := c.path(t)
	if err := os.MkdirAll(filepath.Dir(p), 0o755); err != nil {
		return err
	}
	if err := writeAtomic(p, body); err != nil {
		return err
	}
	n, err := c.writeMeta(p, m)
	if err != nil {
		return err
	}
	c.touch(t)
	c.charge(int64(len(body)) + n)
	return nil
}

// storeMeta rewrites only the sidecar, after a 304 re-armed the freshness
// window. The image bytes on disk are already the current ones — that is
// what the 304 said.
func (c *cache) storeMeta(t Tile, m entryMeta) {
	p := c.path(t)
	if _, err := c.writeMeta(p, m); err != nil {
		return
	}
	c.touch(t)
}

func (c *cache) writeMeta(tilePath string, m entryMeta) (int64, error) {
	raw, err := json.Marshal(m)
	if err != nil {
		return 0, err
	}
	if err := writeAtomic(metaPathFor(tilePath), raw); err != nil {
		return 0, err
	}
	return int64(len(raw)), nil
}

// touch stamps the sidecar with the current time. That timestamp is this
// cache's LRU key: it is updated on every serve, including cache hits that
// never write anything else, so eviction discards the squares nobody has
// looked at rather than the ones that merely happen to be old.
func (c *cache) touch(t Tile) {
	now := c.now()
	// Failure here is not worth reporting: the worst case is that this
	// entry looks older than it is and gets evicted early, and it is
	// demand-driven cache content that refills itself.
	_ = os.Chtimes(metaPathFor(c.path(t)), now, now)
}

// charge adds n bytes to the running total and evicts if that puts the
// cache over budget.
func (c *cache) charge(n int64) {
	c.mu.Lock()
	if !c.sized {
		// One scan on the first write of the process, so a cache inherited
		// from a previous run is accounted for before it can grow past the
		// budget unnoticed. The scan already includes the write that
		// prompted this call, so n must not be added on top of it.
		total, _, err := c.scan()
		if err == nil {
			c.bytes = total
			c.sized = true
		} else {
			c.bytes += n
		}
	} else {
		c.bytes += n
	}
	over := c.bytes > c.maxBytes
	c.mu.Unlock()

	if over {
		c.evict()
	}
}

// evict deletes least-recently-used entries until the cache is comfortably
// under budget.
//
// It evicts down to 90% rather than exactly to the budget so that a full
// cache does not re-scan on every single tile; and it is a plain LRU, with
// no minimum-age exemption, because the alternative when every entry is
// younger than the policy's seven days would be to grow past the budget
// forever. The policy's concern is that tiles are not re-downloaded
// needlessly, which the freshness window handles; keeping a bounded working
// set on disk is orthogonal to it.
func (c *cache) evict() {
	c.mu.Lock()
	defer c.mu.Unlock()

	total, entries, err := c.scan()
	if err != nil {
		return
	}
	target := c.maxBytes - c.maxBytes/10
	sort.Slice(entries, func(i, j int) bool { return entries[i].used.Before(entries[j].used) })
	for _, e := range entries {
		if total <= target {
			break
		}
		if err := c.removeInside(e.base + ".png"); err != nil {
			continue
		}
		_ = c.removeInside(e.base + metaSuffix)
		c.pruneEmpty(filepath.Dir(e.base))
		total -= e.size
	}
	c.bytes = total
	c.sized = true
}

// cacheEntry is one tile's worth of files, as seen by a scan.
type cacheEntry struct {
	base string    // full path without the .png / .meta.json suffix
	size int64     // both files together
	used time.Time // sidecar mtime: see touch
}

// scan walks the cache directory. It reports only files this package owns:
// anything else found under root is counted for neither size nor eviction
// and is never deleted.
func (c *cache) scan() (int64, []cacheEntry, error) {
	byBase := make(map[string]*cacheEntry)
	var total int64
	err := filepath.WalkDir(c.root, func(path string, d fs.DirEntry, err error) error {
		if err != nil {
			// An unreadable subtree should not abort the whole scan; the
			// worst case is that the estimate is low.
			return nil //nolint:nilerr // deliberate: skip, don't abort
		}
		if d.IsDir() {
			return nil
		}
		var base string
		var isMeta bool
		switch {
		case strings.HasSuffix(path, metaSuffix):
			base, isMeta = strings.TrimSuffix(path, metaSuffix), true
		case strings.HasSuffix(path, ".png"):
			base = strings.TrimSuffix(path, ".png")
		default:
			return nil
		}
		info, err := d.Info()
		if err != nil {
			return nil //nolint:nilerr // deliberate: skip, don't abort
		}
		e := byBase[base]
		if e == nil {
			e = &cacheEntry{base: base}
			byBase[base] = e
		}
		e.size += info.Size()
		total += info.Size()
		if isMeta || e.used.IsZero() {
			e.used = info.ModTime()
		}
		return nil
	})
	if err != nil {
		return 0, nil, err
	}
	entries := make([]cacheEntry, 0, len(byBase))
	for _, e := range byBase {
		entries = append(entries, *e)
	}
	return total, entries, nil
}

// removeInside deletes a file, refusing anything that is not underneath the
// configured cache directory. Paths only ever come from a walk rooted at
// c.root, so this can only fire on a programming error — which is exactly
// when a delete-by-path helper is most dangerous.
func (c *cache) removeInside(path string) error {
	rel, err := filepath.Rel(c.root, path)
	if err != nil {
		return err
	}
	if rel == "." || rel == ".." || strings.HasPrefix(rel, ".."+string(filepath.Separator)) || filepath.IsAbs(rel) {
		return fmt.Errorf("tiles: refusing to delete %s: outside cache directory %s", path, c.root)
	}
	return os.Remove(path)
}

// pruneEmpty removes now-empty z/ and x/ directories left behind by
// eviction, walking up to (but never including) the cache root. os.Remove
// fails harmlessly on a directory that still has entries, which is the
// whole emptiness test.
func (c *cache) pruneEmpty(dir string) {
	for i := 0; i < 3; i++ {
		if c.removeInside(dir) != nil {
			return
		}
		dir = filepath.Dir(dir)
	}
}

// writeAtomic writes data to path via a temporary file in the same
// directory, so a reader never sees a half-written tile.
func writeAtomic(path string, data []byte) error {
	dir := filepath.Dir(path)
	f, err := os.CreateTemp(dir, ".tile-*")
	if err != nil {
		return err
	}
	tmp := f.Name()
	if _, err := f.Write(data); err != nil {
		f.Close()
		os.Remove(tmp)
		return err
	}
	if err := f.Close(); err != nil {
		os.Remove(tmp)
		return err
	}
	if err := os.Rename(tmp, path); err != nil {
		os.Remove(tmp)
		return err
	}
	return nil
}
