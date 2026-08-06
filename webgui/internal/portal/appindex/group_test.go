// SPDX-License-Identifier: GPL-2.0-or-later

// Part of mayhem-b200.

package appindex

import (
	"testing"

	"mayhemb200/webgui/internal/portal/client"
)

func names(g CategoryGroup) []string {
	out := make([]string, len(g.Apps))
	for i, a := range g.Apps {
		out[i] = a.DisplayName
	}
	return out
}

func categories(groups []CategoryGroup) []string {
	out := make([]string, len(groups))
	for i, g := range groups {
		out[i] = g.Category
	}
	return out
}

func TestGroup_Empty(t *testing.T) {
	groups := Group(nil)
	if len(groups) != 0 {
		t.Fatalf("Group(nil) = %+v, want empty", groups)
	}
}

func TestGroup_CanonicalOrder(t *testing.T) {
	apps := []client.App{
		{ID: "b", DisplayName: "B", Category: "Debug"},
		{ID: "a", DisplayName: "A", Category: "Receive"},
		{ID: "c", DisplayName: "C", Category: "Utilities"},
		{ID: "d", DisplayName: "D", Category: "Transmit"},
	}
	got := categories(Group(apps))
	want := []string{"Receive", "Transmit", "Utilities", "Debug"}
	if len(got) != len(want) {
		t.Fatalf("categories = %v, want %v", got, want)
	}
	for i := range want {
		if got[i] != want[i] {
			t.Fatalf("categories = %v, want %v", got, want)
		}
	}
}

func TestGroup_OmitsEmptyCategories(t *testing.T) {
	apps := []client.App{{ID: "a", DisplayName: "A", Category: "Receive"}}
	groups := Group(apps)
	if len(groups) != 1 {
		t.Fatalf("groups = %+v, want exactly the one populated category", groups)
	}
	if groups[0].Category != "Receive" {
		t.Fatalf("groups[0].Category = %q, want Receive", groups[0].Category)
	}
}

func TestGroup_SortsWithinCategoryCaseInsensitive(t *testing.T) {
	apps := []client.App{
		{ID: "z", DisplayName: "zebra", Category: "Utilities"},
		{ID: "a", DisplayName: "Apple", Category: "Utilities"},
		{ID: "m", DisplayName: "mango", Category: "Utilities"},
	}
	groups := Group(apps)
	if len(groups) != 1 {
		t.Fatalf("groups = %+v, want one group", groups)
	}
	got := names(groups[0])
	want := []string{"Apple", "mango", "zebra"}
	for i := range want {
		if got[i] != want[i] {
			t.Fatalf("names = %v, want %v", got, want)
		}
	}
}

func TestGroup_TiesBrokenByID(t *testing.T) {
	apps := []client.App{
		{ID: "zzz", DisplayName: "Same Name", Category: "Utilities"},
		{ID: "aaa", DisplayName: "Same Name", Category: "Utilities"},
	}
	groups := Group(apps)
	if groups[0].Apps[0].ID != "aaa" || groups[0].Apps[1].ID != "zzz" {
		t.Fatalf("apps = %+v, want aaa before zzz on a display-name tie", groups[0].Apps)
	}
}

// TestGroup_ExcludesGames pins the product decision that the web portal does
// not offer the games (see excludedCategories). The two tests above used to
// use "Games" as an arbitrary category and now use "Utilities"; this is the
// test that actually asserts the exclusion, so that swap cannot quietly become
// the only thing keeping the suite green.
func TestGroup_ExcludesGames(t *testing.T) {
	apps := []client.App{
		{ID: "a", DisplayName: "A", Category: "Receive"},
		{ID: "breakout", DisplayName: "Breakout", Category: "Games"},
		{ID: "doom", DisplayName: "Doom", Category: "Games"},
	}
	groups := Group(apps)
	for _, g := range groups {
		if g.Category == "Games" {
			t.Fatalf("Games group present in portal output: %+v", g)
		}
		for _, a := range g.Apps {
			if a.ID == "breakout" || a.ID == "doom" {
				t.Fatalf("game %q leaked into category %q", a.ID, g.Category)
			}
		}
	}
	if len(groups) != 1 || groups[0].Category != "Receive" {
		t.Fatalf("groups = %+v, want only the Receive group", groups)
	}
	if !Excluded("Games") {
		t.Fatal(`Excluded("Games") = false, want true`)
	}
	if Excluded("Receive") {
		t.Fatal(`Excluded("Receive") = true, want false for a normal category`)
	}
}

// TestGroup_AllGamesYieldsNothing makes sure an all-games list produces an
// empty portal rather than an empty-but-present group the UI would render as
// a bare heading.
func TestGroup_AllGamesYieldsNothing(t *testing.T) {
	apps := []client.App{
		{ID: "breakout", DisplayName: "Breakout", Category: "Games"},
		{ID: "blackjack", DisplayName: "Blackjack", Category: "Games"},
	}
	if groups := Group(apps); len(groups) != 0 {
		t.Fatalf("Group(all games) = %+v, want empty", groups)
	}
}

func TestGroup_UnknownCategoryAppendedAtEnd(t *testing.T) {
	apps := []client.App{
		{ID: "a", DisplayName: "A", Category: "Receive"},
		{ID: "x", DisplayName: "X", Category: "Experimental"},
		{ID: "y", DisplayName: "Y", Category: "Beta"},
	}
	got := categories(Group(apps))
	want := []string{"Receive", "Beta", "Experimental"} // canonical first, then unknowns alphabetically
	if len(got) != len(want) {
		t.Fatalf("categories = %v, want %v", got, want)
	}
	for i := range want {
		if got[i] != want[i] {
			t.Fatalf("categories = %v, want %v", got, want)
		}
	}
}

func TestGroup_UnknownCategoryAppsStillPresent(t *testing.T) {
	apps := []client.App{{ID: "x", DisplayName: "X", Category: "Experimental"}}
	groups := Group(apps)
	total := 0
	for _, g := range groups {
		total += len(g.Apps)
	}
	if total != 1 {
		t.Fatalf("total apps across groups = %d, want 1 (app must not be dropped)", total)
	}
}

// TestGroup_PreservesAllAppFields makes sure Group doesn't lossily rebuild
// App values (e.g. forgetting HardwareLimited or Icon) on its way through.
func TestGroup_PreservesAllAppFields(t *testing.T) {
	apps := []client.App{
		{ID: "hf", DisplayName: "HF Radio", Category: "Receive", HardwareLimited: true, Icon: "radio"},
	}
	groups := Group(apps)
	got := groups[0].Apps[0]
	if !got.HardwareLimited || got.Icon != "radio" {
		t.Fatalf("app = %+v, want HardwareLimited=true Icon=radio preserved", got)
	}
}
