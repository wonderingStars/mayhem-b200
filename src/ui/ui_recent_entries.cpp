/*
 * mayhem-b200 — scrolling result table, non-template parts.
 *
 * Copyright (C) 2014 Jared Boone, ShareBrained Technology, Inc. (original design)
 * Copyright (C) 2026 mayhem-b200 contributors (host implementation)
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "ui_recent_entries.hpp"

namespace ui {

RecentEntriesColumns::RecentEntriesColumns(
    std::initializer_list<RecentEntriesColumn> columns)
    : columns_{columns} {
}

RecentEntriesHeader::RecentEntriesHeader(RecentEntriesColumns& columns)
    : columns_{columns} {
}

void RecentEntriesHeader::paint(Painter& painter) {
    const auto r = screen_rect();
    const Style parent_style = style();

    const Style header_style{
        .font = parent_style.font,
        .background = *Theme::getInstance()->bg_table_header,
        .foreground = parent_style.foreground,
    };

    /* Upstream only paints under the column captions, which leaves whatever was
     * on screen before showing to the right of the last one. Clearing first
     * costs one fill and removes that artefact. */
    painter.fill_rectangle(r, header_style.background);

    auto p = r.location();
    for (const auto& column : columns_) {
        const auto width = column.second;
        auto text = column.first;
        if (width > text.length()) text.append(width - text.length(), ' ');

        painter.draw_string(p, header_style, text);
        p += Point{static_cast<Coord>((width * 8) + 8), 0};
    }
}

}  // namespace ui
