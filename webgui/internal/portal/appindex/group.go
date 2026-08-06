// SPDX-License-Identifier: GPL-2.0-or-later

// Part of mayhem-b200.

// Package appindex turns the flat app list returned by mayhem-b200's
// app-portal API into the grouped, deterministically ordered structure the
// app grid renders: apps bucketed by category, categories in the same order
// app::Category enumerates them (app_registry.hpp), apps within a category
// sorted by display name.
//
// This is pure, side-effect-free logic kept out of internal/portal/server
// specifically so it can be unit tested without an HTTP server or a fake
// backend in the loop. Search-as-you-type and the category filter chips are
// deliberately NOT here: they run client-side in app.js over the already-
// fetched list, since filtering ~103 already-in-memory items needs no round
// trip and instant feedback matters more than reusing this package's sort.
package appindex

import (
	"sort"
	"strings"

	"mayhemb200/webgui/internal/portal/client"
)

// canonicalOrder mirrors app::Category (app_registry.hpp) exactly. A
// category not listed here (the backend added one this package doesn't
// know about yet) is not dropped — Group appends it after every known
// category, alphabetically, so a new category still shows up rather than
// silently disappearing from the UI.
var canonicalOrder = []string{
	"Home",
	"Receive",
	"Transmit",
	"Transceiver",
	"Utilities",
	"Games",
	"Settings",
	"Debug",
}

// excludedCategories are categories the web portal deliberately does not
// offer. Games are a deliberate product decision, not an oversight: the
// portal exists to put mayhem-b200's *radio* work in a browser, and the
// games are the one group of apps whose whole value is the handheld device
// in front of you. They stay fully present in the desktop app — this is a
// choice about what this interface shows, and nothing about the apps
// themselves or the registry changes.
//
// Filtering here rather than in the C++ /api/apps keeps that API an honest
// view of the real registry (so /api/apps stays useful for diagnostics and
// any other client), and keeps the decision in the layer whose job is
// presentation.
var excludedCategories = map[string]bool{
	"Games": true,
}

// Excluded reports whether the portal hides apps in this category. Exported
// so callers that resolve an app by id can apply the same rule rather than
// re-deriving it.
func Excluded(category string) bool { return excludedCategories[category] }

// Filter returns apps minus the excluded categories, order preserved. Any
// response that carries a flat app list alongside Group's output must run it
// through here, so the two never disagree about what the portal offers — a
// client reading the flat list would otherwise still find the hidden apps.
// Always returns a non-nil slice, so it serializes as [] and never null.
func Filter(apps []client.App) []client.App {
	out := make([]client.App, 0, len(apps))
	for _, a := range apps {
		if !Excluded(a.Category) {
			out = append(out, a)
		}
	}
	return out
}

// CategoryGroup is one category's worth of apps, ready to render as a
// section.
type CategoryGroup struct {
	Category string       `json:"category"`
	Apps     []client.App `json:"apps"`
}

// Group buckets apps by Category, in canonical category order, with each
// group's apps sorted by DisplayName (case-insensitive; ties broken by ID
// for a fully deterministic order). Apps in an excluded category (see
// excludedCategories) are dropped. Categories with zero apps are omitted.
// Apps carrying an empty or unrecognized category are grouped under that
// literal string, appended after every canonical category — never dropped.
func Group(apps []client.App) []CategoryGroup {
	byCategory := make(map[string][]client.App, len(canonicalOrder))
	for _, a := range apps {
		if Excluded(a.Category) {
			continue
		}
		byCategory[a.Category] = append(byCategory[a.Category], a)
	}

	order := make([]string, 0, len(byCategory))
	seen := make(map[string]bool, len(canonicalOrder))
	for _, c := range canonicalOrder {
		seen[c] = true
		if len(byCategory[c]) > 0 {
			order = append(order, c)
		}
	}
	var extra []string
	for c := range byCategory {
		if !seen[c] {
			extra = append(extra, c)
		}
	}
	sort.Strings(extra)
	order = append(order, extra...)

	groups := make([]CategoryGroup, 0, len(order))
	for _, c := range order {
		items := byCategory[c]
		sort.Slice(items, func(i, j int) bool {
			li, lj := strings.ToLower(items[i].DisplayName), strings.ToLower(items[j].DisplayName)
			if li != lj {
				return li < lj
			}
			return items[i].ID < items[j].ID
		})
		groups = append(groups, CategoryGroup{Category: c, Apps: items})
	}
	return groups
}
