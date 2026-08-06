// SPDX-License-Identifier: GPL-2.0-or-later

// Part of mayhem-b200.

package tiles

import "testing"

// TestParsePath is the security boundary's test: everything the endpoint
// will ever be handed goes through here before a filesystem path or an
// upstream URL is built from it.
func TestParsePath(t *testing.T) {
	tests := []struct {
		name string
		path string
		want Tile
		ok   bool
	}{
		// Accepted.
		{name: "origin", path: "/api/tiles/0/0/0.png", want: Tile{0, 0, 0}, ok: true},
		{name: "typical", path: "/api/tiles/2/1/1.png", want: Tile{2, 1, 1}, ok: true},
		{name: "max zoom", path: "/api/tiles/19/524287/524287.png", want: Tile{19, 524287, 524287}, ok: true},
		{name: "uk-ish at z13", path: "/api/tiles/13/4033/2589.png", want: Tile{13, 4033, 2589}, ok: true},

		// 2^z boundaries: the last valid index and the first invalid one.
		{name: "z1 last x", path: "/api/tiles/1/1/1.png", want: Tile{1, 1, 1}, ok: true},
		{name: "z1 x == 2^z", path: "/api/tiles/1/2/0.png"},
		{name: "z1 y == 2^z", path: "/api/tiles/1/0/2.png"},
		{name: "z2 last", path: "/api/tiles/2/3/3.png", want: Tile{2, 3, 3}, ok: true},
		{name: "z2 x == 2^z", path: "/api/tiles/2/4/0.png"},
		{name: "z2 y == 2^z", path: "/api/tiles/2/0/4.png"},
		{name: "z0 x == 1", path: "/api/tiles/0/1/0.png"},
		{name: "z0 y == 1", path: "/api/tiles/0/0/1.png"},
		{name: "z19 x == 2^19", path: "/api/tiles/19/524288/0.png"},

		// Zoom range.
		{name: "zoom above max", path: "/api/tiles/20/0/0.png"},
		{name: "absurd zoom", path: "/api/tiles/99/0/0.png"},

		// Not canonical unsigned decimals.
		{name: "negative zoom", path: "/api/tiles/-1/0/0.png"},
		{name: "negative x", path: "/api/tiles/2/-1/0.png"},
		{name: "negative y", path: "/api/tiles/2/0/-1.png"},
		{name: "explicit plus", path: "/api/tiles/+2/0/0.png"},
		{name: "leading zero", path: "/api/tiles/02/0/0.png"},
		{name: "leading zero x", path: "/api/tiles/2/01/0.png"},
		{name: "leading space", path: "/api/tiles/ 2/0/0.png"},
		{name: "trailing space", path: "/api/tiles/2/0/0 .png"},
		{name: "hex", path: "/api/tiles/0x2/0/0.png"},
		{name: "float", path: "/api/tiles/2.0/0/0.png"},
		{name: "exponent", path: "/api/tiles/1e2/0/0.png"},
		{name: "letters", path: "/api/tiles/a/b/c.png"},
		{name: "empty zoom", path: "/api/tiles//0/0.png"},
		{name: "overflow-length digits", path: "/api/tiles/1/99999999999999999999/0.png"},

		// Shape.
		{name: "empty path", path: ""},
		{name: "prefix only", path: "/api/tiles/"},
		{name: "two segments", path: "/api/tiles/2/1.png"},
		{name: "four segments", path: "/api/tiles/2/1/1/1.png"},
		{name: "no extension", path: "/api/tiles/2/1/1"},
		{name: "wrong extension", path: "/api/tiles/2/1/1.jpg"},
		{name: "uppercase extension", path: "/api/tiles/2/1/1.PNG"},
		{name: "double extension", path: "/api/tiles/2/1/1.png.png"},
		{name: "trailing slash", path: "/api/tiles/2/1/1.png/"},
		{name: "wrong prefix", path: "/tiles/2/1/1.png"},
		{name: "prefix not at start", path: "/x/api/tiles/2/1/1.png"},

		// Traversal. net/url has already percent-decoded the path by the
		// time a handler sees it, so the decoded forms are what matter.
		{name: "dotdot segments", path: "/api/tiles/../../etc/passwd.png"},
		{name: "dotdot in place of y", path: "/api/tiles/2/1/...png"},
		{name: "dotdot escape", path: "/api/tiles/1/../../../secret.png"},
		{name: "dotdot after coords", path: "/api/tiles/2/1/../../secret.png"},
		{name: "absolute windows path", path: "/api/tiles/2/1/C:\\Windows\\win.ini.png"},
		{name: "backslash traversal", path: `/api/tiles/2/1/..\..\secret.png`},
		{name: "nul byte", path: "/api/tiles/2/1/1\x00.png"},
	}

	for _, tt := range tests {
		t.Run(tt.name, func(t *testing.T) {
			got, err := ParsePath(tt.path)
			if tt.ok {
				if err != nil {
					t.Fatalf("ParsePath(%q) = error %v, want %v", tt.path, err, tt.want)
				}
				if got != tt.want {
					t.Fatalf("ParsePath(%q) = %v, want %v", tt.path, got, tt.want)
				}
				return
			}
			if err == nil {
				t.Fatalf("ParsePath(%q) = %v, want an error", tt.path, got)
			}
		})
	}
}

func TestTileRelPathUsesOnlyDigits(t *testing.T) {
	got := Tile{Z: 13, X: 4033, Y: 2589}.relPath()
	want := []string{"13", "4033", "2589.png"}
	if len(got) != len(want) {
		t.Fatalf("relPath() = %v, want %v", got, want)
	}
	for i := range want {
		if got[i] != want[i] {
			t.Fatalf("relPath() = %v, want %v", got, want)
		}
	}
}

func TestParseCoord(t *testing.T) {
	tests := []struct {
		in   string
		want int
		ok   bool
	}{
		{in: "0", want: 0, ok: true},
		{in: "7", want: 7, ok: true},
		{in: "524287", want: 524287, ok: true},
		{in: "999999999", want: 999999999, ok: true},
		{in: ""},
		{in: "00"},
		{in: "0700"},
		{in: "-1"},
		{in: "+1"},
		{in: " 1"},
		{in: "1 "},
		{in: "1234567890"}, // ten digits: refused before it can overflow a 32-bit int
		{in: ".."},
		{in: "1.5"},
	}
	for _, tt := range tests {
		got, ok := parseCoord(tt.in)
		if ok != tt.ok {
			t.Errorf("parseCoord(%q) ok = %v, want %v", tt.in, ok, tt.ok)
			continue
		}
		if ok && got != tt.want {
			t.Errorf("parseCoord(%q) = %d, want %d", tt.in, got, tt.want)
		}
	}
}
