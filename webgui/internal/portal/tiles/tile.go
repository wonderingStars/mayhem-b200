// SPDX-License-Identifier: GPL-2.0-or-later

// Part of mayhem-b200.

package tiles

import (
	"errors"
	"fmt"
	"strconv"
	"strings"
)

// Tile is one validated Web Mercator slippy-map tile coordinate. A Tile
// only ever exists having passed ParsePath, which is what makes it safe to
// build both a filesystem path and an upstream URL from one: every field is
// a small non-negative integer, so no amount of creativity in the request
// path can escape the cache directory or rewrite the upstream URL.
type Tile struct {
	Z, X, Y int
}

func (t Tile) String() string {
	return fmt.Sprintf("%d/%d/%d", t.Z, t.X, t.Y)
}

// key identifies a tile within one provider, for the in-flight map.
func (t Tile) key() string { return t.String() }

// errBadPath and friends are deliberately vague to the caller: the 400 body
// is user-visible, and there is nothing useful to say beyond "that is not a
// tile".
var errBadPath = errors.New("not a tile path: expected " + PathPrefix + "{z}/{x}/{y}.png")

// ParsePath validates a request path and returns the tile it names.
//
// This is the entire security boundary of this package. It runs before any
// filesystem access and before any upstream request, and it accepts only
// the canonical spelling of a tile coordinate: three unsigned decimal
// integers with no sign, no whitespace, no leading zeros, no more segments
// than that. Traversal attempts ("..", encoded or not — net/url has already
// decoded the path by this point) fail on all three counts: they are not
// numeric, they add segments, and nothing but the parsed integers is ever
// used to build a path.
func ParsePath(path string) (Tile, error) {
	rest, ok := strings.CutPrefix(path, PathPrefix)
	if !ok {
		return Tile{}, errBadPath
	}
	name, ok := strings.CutSuffix(rest, ".png")
	if !ok {
		return Tile{}, errBadPath
	}
	parts := strings.Split(name, "/")
	if len(parts) != 3 {
		return Tile{}, errBadPath
	}
	return ParseCoords(parts[0], parts[1], parts[2])
}

// ParseCoords validates the three coordinates of a tile.
func ParseCoords(zs, xs, ys string) (Tile, error) {
	z, ok := parseCoord(zs)
	if !ok {
		return Tile{}, fmt.Errorf("invalid zoom %q", zs)
	}
	if z > MaxZoom {
		return Tile{}, fmt.Errorf("zoom %d out of range (0..%d)", z, MaxZoom)
	}
	x, ok := parseCoord(xs)
	if !ok {
		return Tile{}, fmt.Errorf("invalid x %q", xs)
	}
	y, ok := parseCoord(ys)
	if !ok {
		return Tile{}, fmt.Errorf("invalid y %q", ys)
	}
	// The world is 2^z tiles square at zoom z; x and y are indices into it.
	// z <= MaxZoom (19) keeps this shift far away from overflow.
	limit := 1 << uint(z)
	if x >= limit || y >= limit {
		return Tile{}, fmt.Errorf("tile %d/%d/%d out of range at zoom %d (0..%d)", z, x, y, z, limit-1)
	}
	return Tile{Z: z, X: x, Y: y}, nil
}

// parseCoord accepts only a canonical unsigned decimal integer.
//
// strconv.Atoi is deliberately not used: it accepts "+7" and "-7" and
// "007", which would give one tile several spellings and therefore several
// cache entries (and, for the negative case, a path component starting with
// '-'). One tile, one spelling, one file.
func parseCoord(s string) (int, bool) {
	// 9 digits cannot overflow a 32-bit int, and 2^19 needs 6.
	if s == "" || len(s) > 9 {
		return 0, false
	}
	if len(s) > 1 && s[0] == '0' {
		return 0, false
	}
	n := 0
	for i := 0; i < len(s); i++ {
		c := s[i]
		if c < '0' || c > '9' {
			return 0, false
		}
		n = n*10 + int(c-'0')
	}
	return n, true
}

// relPath is the tile's location inside its provider's cache directory,
// built only from validated integers.
func (t Tile) relPath() []string {
	return []string{strconv.Itoa(t.Z), strconv.Itoa(t.X), strconv.Itoa(t.Y) + ".png"}
}
