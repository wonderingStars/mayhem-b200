/*
 * mayhem-b200 — the scrolling result table every decoder app displays into.
 *
 * A port of firmware/application/recent_entries.hpp. The container is still a
 * std::list<Entry> with most-recent-first ordering, a bounded length and
 * find-or-create by key; the view is still a header row plus a table that
 * scrolls around the cursor. What changed for the host:
 *
 *   - The helpers live in namespace ui rather than at global scope. Apps call
 *     ui::on_packet(...) / ui::find(...) instead of ::on_packet(...).
 *   - RecentEntriesTable::draw() has a primary definition that dispatches to an
 *     on_draw std::function. Upstream leaves the primary undefined and requires
 *     every app to provide an explicit template specialization, which turns a
 *     forgotten specialization into a link error. Both mechanisms work here;
 *     the std::function is the one to reach for.
 *   - on_key() no longer dereferences front()/back() on an empty container.
 *     Upstream crashes if Up or Down arrives before the first packet.
 *   - The selection cursor is readable and settable (selected_key()), which
 *     upstream only exposes indirectly.
 *
 * Copyright (C) 2014 Jared Boone, ShareBrained Technology, Inc. (original design)
 * Copyright (C) 2026 mayhem-b200 contributors (host implementation)
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef __MB200_UI_RECENT_ENTRIES_H__
#define __MB200_UI_RECENT_ENTRIES_H__

#include "theme.hpp"
#include "ui.hpp"
#include "ui_painter.hpp"
#include "ui_widget.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <initializer_list>
#include <iterator>
#include <list>
#include <string>
#include <utility>
#include <vector>

namespace ui {

/* The container. A list rather than a vector because entries are constantly
 * moved to the front and dropped off the back, and because a reference handed
 * out by on_packet() has to stay valid while the caller fills the entry in. */
template <class Entry>
using RecentEntries = std::list<Entry>;

/* --- Container helpers ------------------------------------------------------
 *
 * Entry must provide:
 *   using Key = <comparable type>;
 *   static <const|constexpr> Key invalid_key;
 *   Entry(Key);            // construct-from-key
 *   Key key() const;
 */

template <typename ContainerType, typename Key>
typename ContainerType::const_iterator find_entry(const ContainerType& entries, const Key key) {
    return std::find_if(
        std::begin(entries), std::end(entries),
        [key](typename ContainerType::const_reference e) { return e.key() == key; });
}

template <typename ContainerType, typename Key>
typename ContainerType::iterator find_entry(ContainerType& entries, const Key key) {
    return std::find_if(
        std::begin(entries), std::end(entries),
        [key](typename ContainerType::const_reference e) { return e.key() == key; });
}

/* Upstream spells these `find`. Kept so ported app code reads the same; the
 * two-argument form never collides with the three-argument std::find. */
template <typename ContainerType, typename Key>
typename ContainerType::const_iterator find(const ContainerType& entries, const Key key) {
    return find_entry(entries, key);
}

template <typename ContainerType, typename Key>
typename ContainerType::iterator find(ContainerType& entries, const Key key) {
    return find_entry(entries, key);
}

/* Drops the least recently seen entries until at most entries_max remain. */
template <typename ContainerType>
void truncate_entries(ContainerType& entries, const size_t entries_max = 64) {
    while (entries.size() > entries_max) {
        entries.pop_back();
    }
}

/* Find-or-create by key, with the result at the front of the list.
 *
 * An existing entry keeps its contents and is bumped to the front; a new one is
 * constructed from the key and the list is truncated. The returned reference is
 * always entries.front(), which is what the caller then updates. */
template <typename ContainerType, typename Key>
typename ContainerType::reference on_packet(
    ContainerType& entries,
    const Key key,
    const size_t entries_max = 64) {
    auto matching_recent = find_entry(entries, key);
    if (matching_recent != std::end(entries)) {
        /* Found: move to the front, keeping whatever the app accumulated. */
        entries.push_front(*matching_recent);
        entries.erase(matching_recent);
    } else {
        entries.emplace_front(key);
        truncate_entries(entries, entries_max);
    }

    return entries.front();
}

/* The half-open range of at most `count` entries centred on `item`. Walks back
 * count/2 first, then forward until the window is full — so a cursor near the
 * top of the list still shows a full page below it. */
template <typename ContainerType>
std::pair<typename ContainerType::const_iterator, typename ContainerType::const_iterator> range_around(
    const ContainerType& entries,
    typename ContainerType::const_iterator item,
    const size_t count) {
    auto start = item;
    auto end = item;
    size_t i = 0;

    while ((start != std::begin(entries)) && (i < count / 2)) {
        std::advance(start, -1);
        i++;
    }

    while ((end != std::end(entries)) && (i < count)) {
        std::advance(end, 1);
        i++;
    }

    return {start, end};
}

template <typename ContainerType, typename KeySelector, typename SortOrder>
void sort_entries_by(ContainerType& entries, KeySelector key_selector, SortOrder ascending) {
    entries.sort([key_selector, ascending](const auto& a, const auto& b) {
        return ascending ? key_selector(a) < key_selector(b) : key_selector(a) > key_selector(b);
    });
}

/* Erases every entry the predicate accepts. */
template <typename ContainerType, typename Predicate>
void reset_filtered_entries(ContainerType& entries, Predicate predicate) {
    auto it = entries.begin();
    while (it != entries.end()) {
        if (predicate(*it))
            it = entries.erase(it);
        else
            ++it;
    }
}

template <typename ContainerType, typename MemberPtr, typename Value>
void set_all_members_to_value(ContainerType& entries, MemberPtr member, const Value& value) {
    for (auto& entry : entries) {
        if (entry.*member != value) entry.*member = value;
    }
}

/* --- Columns ---------------------------------------------------------------- */

/* Column name and width in characters. A width of zero means "take whatever is
 * left"; only the first such column gets it. */
using RecentEntriesColumn = std::pair<std::string, size_t>;

class RecentEntriesColumns {
   public:
    using ContainerType = std::vector<RecentEntriesColumn>;

    RecentEntriesColumns(std::initializer_list<RecentEntriesColumn> columns);

    ContainerType::iterator begin() { return columns_.begin(); }
    ContainerType::iterator end() { return columns_.end(); }
    ContainerType::const_iterator begin() const { return columns_.begin(); }
    ContainerType::const_iterator end() const { return columns_.end(); }

    void set(size_t index, const std::string& name, size_t width) {
        if (index < columns_.size()) columns_[index] = {name, width};
    }

    const RecentEntriesColumn& at(size_t index) const { return columns_.at(index); }

    size_t size() const { return columns_.size(); }

   private:
    ContainerType columns_;
};

class RecentEntriesHeader : public Widget {
   public:
    explicit RecentEntriesHeader(RecentEntriesColumns& columns);

    void paint(Painter& painter) override;

   private:
    RecentEntriesColumns& columns_;
};

/* --- Table ------------------------------------------------------------------ */

template <class Entries>
class RecentEntriesTable : public Widget {
   public:
    using Entry = typename Entries::value_type;
    using EntryKey = typename Entry::Key;
    using DrawFunction = std::function<void(const Entry& entry,
                                            const Rect& target_rect,
                                            Painter& painter,
                                            const Style& style,
                                            RecentEntriesColumns& columns)>;

    std::function<void(const Entry& entry)> on_select{};

    /* How a row is rendered. Host addition — see the file header. */
    DrawFunction on_draw{};

    RecentEntriesTable(Entries& recent, RecentEntriesColumns& columns)
        : recent_{recent},
          columns_{columns} {
    }

    /* How many rows fit in the current parent rect. */
    size_t visible_item_count() const {
        const auto line_height = style().font.line_height();
        if (line_height <= 0) return 0;
        const auto h = screen_rect().height();
        return h > 0 ? static_cast<size_t>(h / line_height) : 0;
    }

    EntryKey selected_key() const { return selected_key_; }

    void set_selected_key(EntryKey key) {
        if (selected_key_ == key) return;
        selected_key_ = key;
        set_dirty();
    }

    /* The entry under the cursor, or nullptr when the cursor is invalid. */
    const Entry* selected_entry() const {
        auto it = find_entry(recent_, selected_key_);
        return (it == std::end(recent_)) ? nullptr : &(*it);
    }

    /* Moves the cursor. advance(0) re-validates it: after an eviction the
     * selected key may no longer exist, and the cursor snaps back to the front.
     * Public because apps and tests both need to drive it. */
    void advance(const int32_t amount) {
        auto selected = find_entry(recent_, selected_key_);
        if (selected == std::end(recent_)) {
            selected_key_ = recent_.empty() ? Entry::invalid_key : recent_.front().key();
        } else {
            if (amount < 0) {
                if (selected != std::begin(recent_)) std::advance(selected, -1);
            }
            if (amount > 0) {
                std::advance(selected, 1);
                /* Already on the last entry: stay put rather than wrap. */
                if (selected == std::end(recent_)) return;
            }
            selected_key_ = selected->key();
        }

        set_dirty();
    }

    void paint(Painter& painter) override {
        const auto r = screen_rect();
        const Style s = style();
        const auto line_height = s.font.line_height();
        if (line_height <= 0) return;

        Rect target_rect{r.location(), {r.width(), line_height}};
        const size_t visible = static_cast<size_t>(r.height() / line_height);

        /* An empty table must not hold focus, or the cursor keys stop working. */
        set_focusable(!recent_.empty());

        auto selected = find_entry(recent_, selected_key_);
        if (selected == std::end(recent_)) selected = std::begin(recent_);

        const auto range = range_around(recent_, selected, visible);

        for (auto p = range.first; p != range.second; ++p) {
            const auto& entry = *p;
            const bool is_selected = (selected_key_ == entry.key());
            const Style item_style = (has_focus() && is_selected) ? s.invert() : s;
            draw(entry, target_rect, painter, item_style, columns_);
            target_rect += Point{0, target_rect.height()};
        }

        /* Blank whatever is below the last row. */
        painter.fill_rectangle(
            {target_rect.left(), target_rect.top(), target_rect.width(),
             r.bottom() - target_rect.top()},
            s.background);
    }

    bool on_encoder(const EncoderEvent event) override {
        advance(event);
        return true;
    }

    bool on_key(const KeyEvent event) override {
        if (recent_.empty()) return false;

        if (event == KeyEvent::Select) {
            if (on_select) {
                const auto selected = find_entry(recent_, selected_key_);
                if (selected != std::end(recent_)) {
                    on_select(*selected);
                    return true;
                }
            }
        } else if (event == KeyEvent::Up) {
            /* Declining at the top hands the key to the focus manager, which is
             * how focus escapes the table upwards. */
            if (selected_key_ == recent_.front().key()) return false;
            advance(-1);
            return true;
        } else if (event == KeyEvent::Down) {
            if (selected_key_ == recent_.back().key()) return false;
            advance(1);
            return true;
        }

        return false;
    }

    void on_focus() override {
        advance(0);
    }

   private:
    Entries& recent_;
    RecentEntriesColumns& columns_;
    EntryKey selected_key_ = Entry::invalid_key;

    /* Renders one row. Apps set on_draw; the upstream idiom of fully
     * specializing this member for their Entries type also still works. */
    void draw(const Entry& entry,
              const Rect& target_rect,
              Painter& painter,
              const Style& style,
              RecentEntriesColumns& columns);
};

template <class Entries>
void RecentEntriesTable<Entries>::draw(
    const Entry& entry,
    const Rect& target_rect,
    Painter& painter,
    const Style& style,
    RecentEntriesColumns& columns) {
    if (on_draw) {
        on_draw(entry, target_rect, painter, style, columns);
        return;
    }

    /* No renderer was supplied. Say so on screen rather than drawing blank rows
     * that look like "no packets received yet". */
    std::string line{"(no row renderer)"};
    const auto chars = static_cast<size_t>(std::max(0, target_rect.width() / 8));
    line.resize(chars, ' ');
    painter.draw_string(target_rect.location(), style, line);
}

/* --- View ------------------------------------------------------------------- */

template <class Entries>
class RecentEntriesView : public View {
   public:
    using Entry = typename Entries::value_type;

    std::function<void(const Entry& entry)> on_select{};

    RecentEntriesView(RecentEntriesColumns& columns, Entries& recent)
        : header_{columns},
          table_{recent, columns} {
        /* A zero-width column means "fill the rest of the line". Resolve it now
         * that the screen width is known; only the first one gets the space. */
        size_t total_width = 0;
        size_t zero_width_count = 0;
        for (const auto& column : columns) {
            if (column.second == 0)
                zero_width_count++;
            else
                total_width += column.second;
        }
        if (columns.size() > 0) total_width += columns.size() - 1;  // inter-column spaces

        if (zero_width_count >= 1) {
            const size_t screen_width_chars =
                static_cast<size_t>(screen_width) / UI_POS_DEFAULT_WIDTH;
            const size_t remaining_width =
                screen_width_chars > total_width ? screen_width_chars - total_width : 0;
            size_t i = 0;
            for (const auto& column : columns) {
                if (column.second == 0) {
                    columns.set(i, column.first, remaining_width);
                    break;
                }
                i++;
            }
        }

        add_children({&header_, &table_});

        table_.on_select = [this](const Entry& entry) {
            if (this->on_select) this->on_select(entry);
        };
    }

    void set_parent_rect(const Rect new_parent_rect) override {
        constexpr Dim header_height = 16;

        View::set_parent_rect(new_parent_rect);
        header_.set_parent_rect({0, 0, new_parent_rect.width(), header_height});
        table_.set_parent_rect({0, header_height,
                                new_parent_rect.width(),
                                new_parent_rect.height() - header_height});
    }

    void paint(Painter&) override {
        /* The header and the table cover this view completely. */
    }

    void focus() override {
        table_.focus();
    }

    RecentEntriesTable<Entries>& table() { return table_; }
    const RecentEntriesTable<Entries>& table() const { return table_; }

   private:
    RecentEntriesHeader header_;
    RecentEntriesTable<Entries> table_;
};

}  // namespace ui

#endif /*__MB200_UI_RECENT_ENTRIES_H__*/
