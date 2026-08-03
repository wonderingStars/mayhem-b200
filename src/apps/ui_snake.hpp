/*
 * mayhem-b200 — Snake (Games).
 *
 * Host port of PortaPack Mayhem's external/snake app (by RocketGod,
 * https://betaskynet.com). The upstream View wove the grid rules, food spawning
 * and collision straight into the paint/frame_sync path with a pile of
 * incremental LCD draws to dodge the slow SPI display's flicker. Here the pure
 * grid logic lives in the snake_logic namespace (a small SnakeEngine) so the
 * growth-on-food, wall-collision and self-collision rules can be unit-tested
 * headless against known board positions, and the View is a thin shell that
 * renders the whole board on demand (state only changes on a game tick or a
 * keypress), which is what the host paint model wants.
 *
 * Rules ported verbatim from upstream ui_snake.cpp:
 *   - the snake moves one grid cell per tick (5 ticks/second);
 *   - a direction change is only accepted onto the perpendicular axis, so the
 *     snake can never reverse into itself in one step;
 *   - eating food (the head reaching the food cell) grows the snake by one and
 *     scores 10; food then respawns on a free cell;
 *   - the game ends when the head leaves the grid or lands on its own body.
 *
 * Copyright (C) 2026 mayhem-b200 contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef __MB200_UI_SNAKE_H__
#define __MB200_UI_SNAKE_H__

#include "ui.hpp"
#include "ui_widget.hpp"

#include <cstdint>
#include <cstdlib>
#include <functional>
#include <string>
#include <vector>

namespace app {

/* ---- pure grid logic (tested in test_snake.cpp) -------------------------- */
namespace snake_logic {

enum class Dir { Left, Right, Up, Down };

/* The board rules, separated from all rendering. Members are public so tests
 * can set up a known position and assert the outcome directly. */
class SnakeEngine {
   public:
    /* (Re)start on a grid_w x grid_h board: head centred, length 1, heading
     * right (dx=1), score 0, one food placed on a free cell. */
    void reset(int w, int h);

    /* Upstream turn rule: a turn is accepted only when it is onto the axis the
     * snake is not currently moving along (dx==0 to turn left/right, dy==0 to
     * turn up/down). This is what makes an instant 180 impossible. */
    void set_direction(Dir d);

    struct StepResult {
        bool ate_food;  /* head reached the food this step */
        bool collided;  /* head left the grid or hit the body after moving */
    };

    /* Advance one tick: move the head, shift the body, grow+score+respawn on
     * food, then test collision. Mirrors upstream update_game(). */
    StepResult step();

    /* Head out of bounds, or head on any body segment [1..length-1]. */
    bool check_collision() const;

    /* Place food on a random cell not occupied by the snake. */
    void spawn_food();

    /* State (public for testing). */
    int grid_w{0};
    int grid_h{0};
    std::vector<int> snake_x{};
    std::vector<int> snake_y{};
    int snake_length{1};
    int snake_dx{1};
    int snake_dy{0};
    int food_x{0};
    int food_y{0};
    int score{0};

    /* Injectable RNG returning an int in [0, max); defaults to std::rand so the
     * View gets normal randomness and tests can pin it down deterministically. */
    std::function<int(int)> rng{[](int max) { return max > 0 ? (std::rand() % max) : 0; }};
};

}  // namespace snake_logic

enum class SnakeState : uint8_t { Menu, Playing, GameOver };

class SnakeView : public ui::View {
   public:
    SnakeView();

    std::string title() const override { return "Snake"; }

    void on_show() override;
    void on_frame_sync() override;
    void paint(ui::Painter& painter) override;
    bool on_key(ui::KeyEvent key) override;

   private:
    void start_game();
    void tick();

    void paint_menu(ui::Painter& painter, ui::Point o) const;
    void paint_playing(ui::Painter& painter, ui::Point o) const;
    void paint_game_over(ui::Painter& painter, ui::Point o) const;

    snake_logic::SnakeEngine engine_{};
    SnakeState state_{SnakeState::Menu};
    bool initialized_{false};

    /* Grid geometry, derived from the view size on start (see .cpp). */
    static constexpr int kCell = 10;          /* SNAKE_SIZE */
    static constexpr int kInfoBarHeight = 25; /* score strip at the top */
    static constexpr int kGameAreaTop = kInfoBarHeight + 1;

    /* Fall/step timing: 60 Hz frames, one move every kMoveFrames (~5/s). */
    static constexpr uint32_t kMoveFrames = 12;  /* (1/5 s) * 60 */
    uint32_t frame_counter_{0};
};

}  // namespace app

#endif /*__MB200_UI_SNAKE_H__*/
