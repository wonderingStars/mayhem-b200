/*
 * mayhem-b200 — 2048 (Games).
 *
 * Host port of PortaPack Mayhem's external/game2048 app (copyleft 2025 zxkmm).
 * The board rules, tile spawning odds (1-in-10 spawns a 4, else a 2), colours
 * and win/lose conditions are ported verbatim from upstream ui_game2048.cpp.
 *
 * The firmware wove the slide/merge logic into the View and drew from a
 * message-queue frame_sync with a pile of flicker-avoidance hacks for the slow
 * SPI LCD. Here the pure board logic lives in the game2048_logic namespace so it
 * can be unit-tested against known rows and grids, and the View is a thin shell
 * that renders on demand (state only changes on a keypress) exactly as the host
 * paint model wants.
 *
 * Copyright (C) 2026 mayhem-b200 contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef __MB200_UI_GAME2048_H__
#define __MB200_UI_GAME2048_H__

#include "ui.hpp"
#include "ui_widget.hpp"

#include <cstdint>
#include <random>
#include <string>

namespace app {

/* ---- pure board logic (tested in test_game2048.cpp) ---------------------- */
namespace game2048_logic {

constexpr int kGridSize = 4;

using Row = int[kGridSize];
using Grid = int[kGridSize][kGridSize];

struct MoveResult {
    bool moved;        /* did anything change? */
    int score_delta;   /* points gained this move */
};

/* Slide+merge one row toward index 0, upstream semantics: compress non-zero
 * tiles left, merge each adjacent equal pair once (scanning left→right so a
 * tile merges at most once per move), then compress again. */
MoveResult move_row_left(Row& row);

/* In-place 90-degree grid rotations, used to reduce the other three directions
 * to move_left, exactly as upstream does. */
void rotate_clockwise(Grid& grid);
void rotate_counter_clockwise(Grid& grid);

/* Whole-grid moves. Return {any row changed, total score gained}. */
MoveResult move_left(Grid& grid);
MoveResult move_right(Grid& grid);
MoveResult move_up(Grid& grid);
MoveResult move_down(Grid& grid);

/* A move is still possible iff there is an empty cell or an adjacent equal
 * pair; when this returns false the game is over. */
bool can_move(const Grid& grid);

/* True once any cell reaches 2048. */
bool is_won(const Grid& grid);

}  // namespace game2048_logic

enum class Game2048State : uint8_t { Playing, GameOver, Won };

class Game2048View : public ui::View {
   public:
    Game2048View();

    std::string title() const override { return "2048"; }

    void focus() override;
    void on_show() override;
    void paint(ui::Painter& painter) override;
    bool on_key(ui::KeyEvent key) override;

   private:
    void reset_game();
    void add_random_tile();
    void apply_move(const game2048_logic::MoveResult& r);

    void draw_tile(ui::Painter& painter, ui::Point origin, int x, int y, int value) const;
    ui::Color tile_color(int value) const;
    ui::Color text_color(int value) const;

    game2048_logic::Grid grid_{};
    int tile_size_{50};
    int score_{0};
    int best_score_{0};
    Game2048State state_{Game2048State::Playing};
    bool initialized_{false};
    std::mt19937 rng_{};

    /* Off-screen, focusable: it holds keyboard focus so the dispatcher routes
     * keys here, then forwards them to the View via on_dir / on_select. */
    ui::Button dummy_{{static_cast<int>(ui::screen_width), 0, 0, 0}, ""};
};

}  // namespace app

#endif /*__MB200_UI_GAME2048_H__*/
