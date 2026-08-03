/*
 * mayhem-b200 — Battleship (naval combat game).
 *
 * Host port of PortaPack Mayhem's external/battleship app
 * (Copyright RocketGod, https://betaskynet.com). Upstream is a *two-device*
 * game: each PortaPack runs the same view and the two units exchange moves over
 * RF as POCSAG pages (READY / SHOT / HIT / MISS / SUNK / WIN) on 433.92 MHz.
 * None of that transport is available on a B200 host build — the POCSAG
 * baseband image, pocsag_encode/BCHCode and shared_memory.bb_data the firmware
 * relied on are not part of this project — so the RF link is replaced by a
 * local hot-seat: both fleets live on the one device and the two players take
 * turns on the same screen. Every game rule is upstream's, unchanged: 10x10
 * grid, the same five ships, the same placement constraints (in bounds, no
 * overlap, no touching — not even diagonally), fire again on a hit, and you
 * win by sinking the opponent's whole fleet.
 *
 * The rules live in a UI-free `battleship::Board` so ship-placement validity
 * and hit/miss/sunk resolution can be unit tested against known positions
 * (see tests/test_battleship.cpp); the View is only presentation and turn flow.
 *
 * Copyright (C) 2026 mayhem-b200 contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef __MB200_UI_BATTLESHIP_H__
#define __MB200_UI_BATTLESHIP_H__

#include "ui.hpp"
#include "ui_widget.hpp"

#include <array>
#include <cstdint>
#include <string>

namespace app {

/* ---- pure rules engine (tested in test_battleship.cpp) ------------------- */
namespace battleship {

constexpr int kGridSize = 10;
constexpr int kShipCount = 5;

/* Ship lengths, in the fixed placement order upstream uses:
 * carrier(5), battleship(4), cruiser(3), submarine(3), destroyer(2). */
constexpr std::array<uint8_t, kShipCount> kShipSizes{5, 4, 3, 3, 2};

enum class CellState : uint8_t {
    EMPTY,
    SHIP,
    HIT,
    MISS,
    SUNK,
};

/* Result of firing at a board. INVALID = out of bounds or an already-fired
 * cell (the shot does nothing). */
enum class ShotResult : uint8_t {
    INVALID,
    MISS,
    HIT,
    SUNK,
};

struct Ship {
    uint8_t size{0};
    uint8_t x{0};
    uint8_t y{0};
    bool horizontal{true};
    uint8_t hits{0};
    bool placed{false};

    bool is_sunk() const { return placed && hits >= size; }
};

/* One player's own board: their fleet plus the shots taken against it. */
class Board {
   public:
    Board() { reset(); }

    void reset();

    CellState at(int x, int y) const;

    static bool in_bounds(int x, int y) {
        return x >= 0 && x < kGridSize && y >= 0 && y < kGridSize;
    }

    /* --- placement (sequential, in kShipSizes order) --- */
    uint8_t ships_placed() const { return placed_count_; }
    bool all_ships_placed() const { return placed_count_ >= kShipCount; }
    /* Length of the ship that place_next() would place, or 0 when done. */
    uint8_t next_ship_size() const;

    /* Upstream's can_place_ship: in bounds, every cell empty, and no SHIP cell
     * in the 8-neighbourhood of any cell (ships may not touch). */
    bool can_place(int x, int y, int size, bool horizontal) const;

    /* Places the next ship at (x,y). No-op returning false if invalid. */
    bool place_next(int x, int y, bool horizontal);

    /* --- combat (an opponent fires at THIS board) --- */
    ShotResult receive_shot(int x, int y);

    /* Placed ships not yet sunk. */
    uint8_t ships_remaining() const;
    /* True once at least one ship was placed and all placed ships are sunk. */
    bool defeated() const;

    const Ship& ship(int i) const { return ships_[i]; }

   private:
    /* Index of the ship occupying (x,y), or -1. */
    int ship_index_at(int x, int y) const;

    std::array<std::array<CellState, kGridSize>, kGridSize> grid_{};
    std::array<Ship, kShipCount> ships_{};
    uint8_t placed_count_{0};
};

/* --- Board, defined inline so the rules are usable (and testable) without
 *     dragging in any UI. --- */

inline void Board::reset() {
    for (auto& row : grid_)
        row.fill(CellState::EMPTY);
    for (int i = 0; i < kShipCount; ++i)
        ships_[static_cast<size_t>(i)] =
            Ship{kShipSizes[static_cast<size_t>(i)], 0, 0, true, 0, false};
    placed_count_ = 0;
}

inline CellState Board::at(int x, int y) const {
    if (!in_bounds(x, y)) return CellState::EMPTY;
    return grid_[static_cast<size_t>(y)][static_cast<size_t>(x)];
}

inline uint8_t Board::next_ship_size() const {
    if (placed_count_ >= kShipCount) return 0;
    return kShipSizes[placed_count_];
}

inline bool Board::can_place(int x, int y, int size, bool horizontal) const {
    if (size <= 0) return false;

    /* Off the edge in the direction of travel. */
    if (horizontal) {
        if (x < 0 || y < 0 || x + size > kGridSize || y >= kGridSize) return false;
    } else {
        if (x < 0 || y < 0 || y + size > kGridSize || x >= kGridSize) return false;
    }

    const int dx = horizontal ? 1 : 0;
    const int dy = horizontal ? 0 : 1;

    for (int i = 0; i < size; ++i) {
        const int cx = x + i * dx;
        const int cy = y + i * dy;
        if (!in_bounds(cx, cy)) return false;
        if (grid_[static_cast<size_t>(cy)][static_cast<size_t>(cx)] != CellState::EMPTY)
            return false;

        /* No ship may touch another, not even diagonally. */
        for (int ddy = -1; ddy <= 1; ++ddy) {
            for (int ddx = -1; ddx <= 1; ++ddx) {
                const int ax = cx + ddx;
                const int ay = cy + ddy;
                if (in_bounds(ax, ay) &&
                    grid_[static_cast<size_t>(ay)][static_cast<size_t>(ax)] ==
                        CellState::SHIP)
                    return false;
            }
        }
    }
    return true;
}

inline bool Board::place_next(int x, int y, bool horizontal) {
    if (placed_count_ >= kShipCount) return false;
    const int size = kShipSizes[placed_count_];
    if (!can_place(x, y, size, horizontal)) return false;

    const int dx = horizontal ? 1 : 0;
    const int dy = horizontal ? 0 : 1;
    for (int i = 0; i < size; ++i)
        grid_[static_cast<size_t>(y + i * dy)][static_cast<size_t>(x + i * dx)] =
            CellState::SHIP;

    Ship& s = ships_[placed_count_];
    s.x = static_cast<uint8_t>(x);
    s.y = static_cast<uint8_t>(y);
    s.horizontal = horizontal;
    s.hits = 0;
    s.placed = true;

    ++placed_count_;
    return true;
}

inline int Board::ship_index_at(int x, int y) const {
    for (int i = 0; i < kShipCount; ++i) {
        const Ship& s = ships_[static_cast<size_t>(i)];
        if (!s.placed) continue;
        const int dx = s.horizontal ? 1 : 0;
        const int dy = s.horizontal ? 0 : 1;
        for (int j = 0; j < s.size; ++j) {
            if (s.x + j * dx == x && s.y + j * dy == y) return i;
        }
    }
    return -1;
}

inline ShotResult Board::receive_shot(int x, int y) {
    if (!in_bounds(x, y)) return ShotResult::INVALID;

    CellState& c = grid_[static_cast<size_t>(y)][static_cast<size_t>(x)];
    if (c == CellState::HIT || c == CellState::MISS || c == CellState::SUNK)
        return ShotResult::INVALID;

    if (c != CellState::SHIP) {
        c = CellState::MISS;
        return ShotResult::MISS;
    }

    c = CellState::HIT;

    const int idx = ship_index_at(x, y);
    if (idx < 0) return ShotResult::HIT;  /* unreachable: SHIP cell with no ship */

    Ship& s = ships_[static_cast<size_t>(idx)];
    ++s.hits;

    if (!s.is_sunk()) return ShotResult::HIT;

    /* Sunk: paint every cell of the ship SUNK. */
    const int dx = s.horizontal ? 1 : 0;
    const int dy = s.horizontal ? 0 : 1;
    for (int j = 0; j < s.size; ++j)
        grid_[static_cast<size_t>(s.y + j * dy)][static_cast<size_t>(s.x + j * dx)] =
            CellState::SUNK;

    return ShotResult::SUNK;
}

inline uint8_t Board::ships_remaining() const {
    uint8_t n = 0;
    for (int i = 0; i < kShipCount; ++i)
        if (ships_[static_cast<size_t>(i)].placed &&
            !ships_[static_cast<size_t>(i)].is_sunk())
            ++n;
    return n;
}

inline bool Board::defeated() const {
    return placed_count_ > 0 && ships_remaining() == 0;
}

}  // namespace battleship

/* ---- view --------------------------------------------------------------- */

class BattleshipView : public ui::View {
   public:
    BattleshipView();

    std::string title() const override { return "Battleship"; }

    void on_show() override;
    void focus() override;
    void paint(ui::Painter& painter) override;
    bool on_key(const ui::KeyEvent key) override;
    bool on_encoder(const ui::EncoderEvent delta) override;

   private:
    enum class State : uint8_t {
        MENU,
        PLACING,   /* the player in placing_player_ is laying out ships */
        HANDOFF,   /* board hidden; press Select to hand the device over */
        COMBAT,    /* the player in attacker_ is firing */
        GAMEOVER,
    };

    /* What HANDOFF advances to when Select is pressed. */
    enum class NextAfterHandoff : uint8_t { PLACE, COMBAT };

    void reset_to_menu();
    void begin_placing(int player);
    void resolve_fire();
    void move_cursor(const ui::KeyEvent key, uint8_t& cx, uint8_t& cy);

    /* rendering helpers, all in view-local coordinates */
    void draw_status(ui::Painter& painter, ui::Point origin);
    void draw_board(ui::Painter& painter, ui::Point origin,
                    const battleship::Board& board, bool show_ships,
                    bool show_cursor, uint8_t cur_x, uint8_t cur_y,
                    bool show_preview);

    battleship::Board boards_[2]{};  /* [0] = RED, [1] = BLUE */

    State state_{State::MENU};
    int placing_player_{0};
    int attacker_{0};
    int winner_{-1};
    NextAfterHandoff next_{NextAfterHandoff::PLACE};

    uint8_t cursor_x_{0};
    uint8_t cursor_y_{0};
    bool placing_horizontal_{true};
    uint8_t target_x_{0};
    uint8_t target_y_{0};

    int cell_size_{24};
    int grid_offset_y_{40};

    std::string status_{};
    std::string handoff_to_{};
};

}  // namespace app

#endif /*__MB200_UI_BATTLESHIP_H__*/
