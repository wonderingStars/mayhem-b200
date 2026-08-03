/*
 * mayhem-b200 — Battleship (naval combat game).
 *
 * Copyright (C) 2026 mayhem-b200 contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "ui_battleship.hpp"

#include "app_context.hpp"
#include "theme.hpp"
#include "ui_navigation.hpp"

namespace app {

/* ======================================================================== */
/* view (the rules engine, battleship::Board, is header-only)               */
/* ======================================================================== */

using battleship::Board;
using battleship::CellState;
using battleship::kGridSize;
using battleship::ShotResult;

namespace {
/* Left x of an n-character string centred on the screen (UI_POS_X_CENTER is a
 * macro that name-resolves inside namespace ui, so it is unusable here). */
int center_x(int chars) {
    return ui::screen_width / 2 - chars * ui::char_width / 2;
}
const char* team_name(int player) { return player == 0 ? "RED" : "BLUE"; }
ui::Color team_color(int player) {
    return player == 0 ? ui::Color::red() : ui::Color::blue();
}
const char* ship_name(uint8_t placed) {
    static const char* names[] = {"carrier (5)", "battleship (4)", "cruiser (3)",
                                  "submarine (3)", "destroyer (2)"};
    return placed < 5 ? names[placed] : "";
}
}  // namespace

BattleshipView::BattleshipView() {
    set_focusable(true);
    cell_size_ = ui::screen_width / kGridSize;
    if (cell_size_ < 1) cell_size_ = 1;
    reset_to_menu();
}

void BattleshipView::on_show() {
    View::on_show();
    focus();
    set_dirty();
}

void BattleshipView::focus() {
    ui::Widget::focus();
}

void BattleshipView::reset_to_menu() {
    boards_[0].reset();
    boards_[1].reset();
    state_ = State::MENU;
    placing_player_ = 0;
    attacker_ = 0;
    winner_ = -1;
    cursor_x_ = cursor_y_ = 0;
    target_x_ = target_y_ = 0;
    placing_horizontal_ = true;
    status_ = "SELECT to start";
    set_dirty();
}

void BattleshipView::begin_placing(int player) {
    placing_player_ = player;
    cursor_x_ = cursor_y_ = 0;
    placing_horizontal_ = true;
    state_ = State::PLACING;
    status_ = std::string{team_name(player)} + ": place " + ship_name(0);
    set_dirty();
}

void BattleshipView::move_cursor(const ui::KeyEvent key, uint8_t& cx, uint8_t& cy) {
    switch (key) {
        case ui::KeyEvent::Up:
            cy = (cy == 0) ? kGridSize - 1 : static_cast<uint8_t>(cy - 1);
            break;
        case ui::KeyEvent::Down:
            cy = static_cast<uint8_t>((cy + 1) % kGridSize);
            break;
        case ui::KeyEvent::Left:
            cx = (cx == 0) ? kGridSize - 1 : static_cast<uint8_t>(cx - 1);
            break;
        case ui::KeyEvent::Right:
            cx = static_cast<uint8_t>((cx + 1) % kGridSize);
            break;
        default:
            break;
    }
}

void BattleshipView::resolve_fire() {
    const int defender = 1 - attacker_;
    const ShotResult r = boards_[defender].receive_shot(target_x_, target_y_);

    switch (r) {
        case ShotResult::INVALID:
            status_ = "Already fired there";
            break;

        case ShotResult::MISS:
            /* Turn passes to the other player. */
            attacker_ = defender;
            handoff_to_ = team_name(attacker_);
            next_ = NextAfterHandoff::COMBAT;
            state_ = State::HANDOFF;
            status_ = "Miss! Pass to " + handoff_to_;
            break;

        case ShotResult::HIT:
            status_ = "Hit! Fire again";
            break;

        case ShotResult::SUNK:
            if (boards_[defender].defeated()) {
                winner_ = attacker_;
                state_ = State::GAMEOVER;
                status_ = std::string{team_name(winner_)} + " WINS!";
            } else {
                status_ = "Sunk! Fire again";
            }
            break;
    }
    set_dirty();
}

bool BattleshipView::on_key(const ui::KeyEvent key) {
    /* Back leaves the app from the menu, otherwise returns to the menu. */
    if (key == ui::KeyEvent::Back) {
        if (state_ == State::MENU) return false;
        reset_to_menu();
        return true;
    }

    switch (state_) {
        case State::MENU:
            if (key == ui::KeyEvent::Select) {
                begin_placing(0);
                return true;
            }
            return false;

        case State::HANDOFF:
            if (key == ui::KeyEvent::Select) {
                if (next_ == NextAfterHandoff::PLACE)
                    begin_placing(placing_player_);
                else {
                    state_ = State::COMBAT;
                    target_x_ = target_y_ = 0;
                    status_ = std::string{team_name(attacker_)} + ": fire!";
                    set_dirty();
                }
                return true;
            }
            return true;  /* swallow everything else while the board is hidden */

        case State::PLACING:
            if (key == ui::KeyEvent::Select) {
                Board& b = boards_[placing_player_];
                if (b.place_next(cursor_x_, cursor_y_, placing_horizontal_)) {
                    if (b.all_ships_placed()) {
                        if (placing_player_ == 0) {
                            placing_player_ = 1;
                            handoff_to_ = team_name(1);
                            next_ = NextAfterHandoff::PLACE;
                            state_ = State::HANDOFF;
                            status_ = "Pass to " + handoff_to_;
                        } else {
                            attacker_ = 0;
                            handoff_to_ = team_name(0);
                            next_ = NextAfterHandoff::COMBAT;
                            state_ = State::HANDOFF;
                            status_ = "Ready! Pass to " + handoff_to_;
                        }
                    } else {
                        status_ = std::string{team_name(placing_player_)} +
                                  ": place " + ship_name(b.ships_placed());
                    }
                } else {
                    status_ = "Invalid placement!";
                }
                set_dirty();
                return true;
            }
            move_cursor(key, cursor_x_, cursor_y_);
            set_dirty();
            return true;

        case State::COMBAT:
            if (key == ui::KeyEvent::Select) {
                resolve_fire();
                return true;
            }
            move_cursor(key, target_x_, target_y_);
            set_dirty();
            return true;

        case State::GAMEOVER:
            if (key == ui::KeyEvent::Select) {
                reset_to_menu();
                return true;
            }
            return true;
    }
    return false;
}

bool BattleshipView::on_encoder(const ui::EncoderEvent delta) {
    if (delta == 0) return false;
    /* During placement the knob rotates the ship, exactly as upstream. */
    if (state_ == State::PLACING) {
        placing_horizontal_ = !placing_horizontal_;
        set_dirty();
        return true;
    }
    if (state_ == State::COMBAT) {
        if (delta > 0)
            target_x_ = static_cast<uint8_t>((target_x_ + 1) % kGridSize);
        else
            target_x_ = (target_x_ == 0) ? kGridSize - 1
                                         : static_cast<uint8_t>(target_x_ - 1);
        set_dirty();
        return true;
    }
    return false;
}

/* ---- rendering ----------------------------------------------------------- */

void BattleshipView::draw_status(ui::Painter& painter, ui::Point origin) {
    const ui::Style style_hdr{
        .font = ui::font::fixed_8x16,
        .background = ui::Color::black(),
        .foreground = ui::Color::white()};

    /* Title bar tinted with the active team's colour. */
    int active = -1;
    if (state_ == State::PLACING) active = placing_player_;
    else if (state_ == State::COMBAT) active = attacker_;

    if (active >= 0) {
        const ui::Style style_team{
            .font = ui::font::fixed_8x16,
            .background = team_color(active),
            .foreground = ui::Color::white()};
        painter.fill_rectangle({origin.x(), origin.y(), ui::screen_width, 18},
                               team_color(active));
        painter.draw_string({origin.x() + 4, origin.y() + 1}, style_team,
                            std::string{team_name(active)} + " TEAM");
    } else {
        painter.fill_rectangle({origin.x(), origin.y(), ui::screen_width, 18},
                               ui::Color::black());
        painter.draw_string({origin.x() + 4, origin.y() + 1}, style_hdr,
                            "BATTLESHIP");
    }

    painter.fill_rectangle({origin.x(), origin.y() + 20, ui::screen_width, 16},
                           ui::Color::black());
    painter.draw_string({origin.x() + 4, origin.y() + 20}, style_hdr, status_);
}

void BattleshipView::draw_board(ui::Painter& painter, ui::Point origin,
                                const Board& board, bool show_ships,
                                bool show_cursor, uint8_t cur_x, uint8_t cur_y,
                                bool show_preview) {
    const int cs = cell_size_;
    const int gx = origin.x();
    const int gy = origin.y() + grid_offset_y_;
    const int span = kGridSize * cs;

    painter.fill_rectangle({gx, gy, span, span}, ui::Color::dark_blue());
    for (int i = 0; i <= kGridSize; ++i) {
        painter.draw_vline({gx + i * cs, gy}, span, ui::Color::grey());
        painter.draw_hline({gx, gy + i * cs}, span, ui::Color::grey());
    }

    for (int y = 0; y < kGridSize; ++y) {
        for (int x = 0; x < kGridSize; ++x) {
            const int px = gx + x * cs + 1;
            const int py = gy + y * cs + 1;
            const int w = cs - 2;
            const CellState st = board.at(x, y);

            switch (st) {
                case CellState::SHIP:
                    if (show_ships)
                        painter.fill_rectangle({px, py, w, w}, ui::Color::grey());
                    break;
                case CellState::HIT:
                    painter.fill_rectangle({px, py, w, w}, ui::Color::red());
                    break;
                case CellState::SUNK:
                    painter.fill_rectangle({px, py, w, w}, ui::Color::dark_red());
                    break;
                case CellState::MISS:
                    painter.fill_rectangle({px + w / 2 - 2, py + w / 2 - 2, 4, 4},
                                           ui::Color::light_grey());
                    break;
                case CellState::EMPTY:
                default:
                    break;
            }
            if (st == CellState::HIT || st == CellState::SUNK) {
                painter.draw_line({px + 2, py + 2}, {px + w - 2, py + w - 2},
                                  ui::Color::white());
                painter.draw_line({px + w - 2, py + 2}, {px + 2, py + w - 2},
                                  ui::Color::white());
            }
        }
    }

    /* Ship placement preview: colour the footprint green (valid) or red. */
    if (show_preview) {
        const int size = board.next_ship_size();
        if (size > 0) {
            const bool ok = board.can_place(cur_x, cur_y, size, placing_horizontal_);
            const int dx = placing_horizontal_ ? 1 : 0;
            const int dy = placing_horizontal_ ? 0 : 1;
            const ui::Color c = ok ? ui::Color::green() : ui::Color::red();
            for (int i = 0; i < size; ++i) {
                const int x = cur_x + i * dx;
                const int y = cur_y + i * dy;
                if (x < kGridSize && y < kGridSize) {
                    painter.fill_rectangle(
                        {gx + x * cs + 1, gy + y * cs + 1, cs - 2, cs - 2}, c);
                }
            }
        }
    }

    if (show_cursor) {
        painter.draw_rectangle(
            {gx + cur_x * cs, gy + cur_y * cs, cs, cs},
            (state_ == State::COMBAT) ? ui::Color::yellow() : ui::Color::cyan());
    }
}

void BattleshipView::paint(ui::Painter& painter) {
    const ui::Point origin = screen_pos();
    painter.fill_rectangle(screen_rect(), ui::Color::black());

    const ui::Style style_c{
        .font = ui::font::fixed_8x16,
        .background = ui::Color::black(),
        .foreground = ui::Color::white()};

    if (state_ == State::MENU) {
        painter.draw_string({origin.x() + center_x(10), origin.y() + 40},
                            style_c, "BATTLESHIP");
        const char* lines[] = {
            "Local 2-player hot-seat.",
            "(upstream played head-to-",
            " head over RF; a B200 host",
            " has no POCSAG baseband,",
            " so both fleets share this",
            " one screen and you take",
            " turns.)",
            "",
            "SELECT: place / fire",
            "Arrows: move cursor",
            "Knob: rotate ship",
            "BACK: menu / exit",
        };
        int y = origin.y() + 80;
        for (const char* s : lines) {
            painter.draw_string({origin.x() + 6, y}, style_c, s);
            y += 16;
        }
        painter.draw_string({origin.x() + center_x(15), origin.y() + 288},
                            style_c, "SELECT to start");
        return;
    }

    draw_status(painter, origin);

    if (state_ == State::HANDOFF) {
        painter.draw_string({origin.x() + center_x(9), origin.y() + 130},
                            style_c, "Pass device to");
        const ui::Style style_team{
            .font = ui::font::fixed_8x16,
            .background = ui::Color::black(),
            .foreground = team_color(next_ == NextAfterHandoff::PLACE
                                         ? placing_player_
                                         : attacker_)};
        painter.draw_string({origin.x() + center_x(9), origin.y() + 150},
                            style_team, handoff_to_ + " TEAM");
        painter.draw_string({origin.x() + center_x(13), origin.y() + 190},
                            style_c, "SELECT when ready");
        return;
    }

    if (state_ == State::PLACING) {
        draw_board(painter, origin, boards_[placing_player_],
                   /*show_ships=*/true, /*show_cursor=*/true, cursor_x_, cursor_y_,
                   /*show_preview=*/true);
        return;
    }

    if (state_ == State::COMBAT) {
        const int defender = 1 - attacker_;
        draw_board(painter, origin, boards_[defender],
                   /*show_ships=*/false, /*show_cursor=*/true, target_x_, target_y_,
                   /*show_preview=*/false);
        const std::string left =
            std::string{"Enemy ships: "} +
            std::to_string(boards_[defender].ships_remaining());
        painter.draw_string(
            {origin.x() + 4, origin.y() + grid_offset_y_ + kGridSize * cell_size_ + 6},
            style_c, left);
        return;
    }

    if (state_ == State::GAMEOVER) {
        const int defender = 1 - winner_;
        draw_board(painter, origin, boards_[defender], /*show_ships=*/true,
                   /*show_cursor=*/false, 0, 0, /*show_preview=*/false);
        painter.draw_string({origin.x() + center_x(15), origin.y() + 292},
                            style_c, "SELECT to play again");
        return;
    }
}

}  // namespace app

#include "app_registry.hpp"
#include "bitmaps.hpp"

namespace {
const app::Registrar reg_battleship{{
    "battleship",
    "Battleship",
    app::Category::Games,
    ui::Color::green(),
    &ui::bitmap_icon_games,
    [] { return std::make_unique<app::BattleshipView>(); },
    false,
}};
}  // namespace
