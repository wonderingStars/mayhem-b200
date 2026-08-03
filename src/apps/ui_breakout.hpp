/*
 * mayhem-b200 — Breakout (Games).
 *
 * Host port of PortaPack Mayhem's external/breakout app (by RocketGod). The
 * board sizes, paddle/ball/brick geometry, bounce angle formula and per-row
 * scoring ((5 - row) * 10) are ported verbatim from upstream ui_breakout.cpp.
 *
 * Upstream drove the ball from a 60 Hz timer, drew incrementally to spare the
 * SPI LCD, and played its beeps through the M4 baseband. Here the ball physics
 * and collision resolution live in the breakout_logic namespace so they can be
 * unit-tested against known ball/paddle/brick positions; the View advances that
 * physics once per on_frame_sync() and repaints the whole scene. Audio is not
 * ported — the beeps drove PortaPack baseband/audio hardware that has no bearing
 * on the game rules, so the volume menu is dropped rather than faked.
 *
 * Copyright (C) 2026 mayhem-b200 contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef __MB200_UI_BREAKOUT_H__
#define __MB200_UI_BREAKOUT_H__

#include "ui.hpp"
#include "ui_widget.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace app {

/* ---- pure ball physics (tested in test_breakout.cpp) --------------------- */
namespace breakout_logic {

/* Upstream #defines, kept as named constants. */
constexpr int kPaddleWidth = 40;
constexpr int kPaddleHeight = 10;
constexpr int kBallSize = 8;
constexpr int kBrickHeight = 10;
constexpr int kBrickRows = 5;
constexpr int kBrickGap = 2;
constexpr int kGameAreaTop = 50;
constexpr float kBallSpeedIncrease = 0.1f;

struct Ball {
    float x{0};
    float y{0};
    float dx{0};
    float dy{0};
};

/* Top-left pixel of the brick at (row, col); brick_width is screen-dependent so
 * it is passed in. Matches upstream draw_bricks / check_brick_collision. */
int brick_left(int col, int brick_width);
int brick_top(int row);

/* Points for clearing a brick in `row` (0 = top): (5 - row) * 10. */
int brick_score(int row);

/* Reflect off the left/right walls and the top border. Returns true if any wall
 * was hit; on a side hit dx flips, on the top dy flips, and the ball is clamped
 * back inside the play area, exactly as upstream update_game does. */
bool wall_bounce(Ball& ball, int screen_width);

/* Paddle collision. paddle_top is PADDLE_Y (top edge of the paddle). On a hit
 * the ball is lifted to rest on the paddle, dy flips upward, and dx is set from
 * where along the paddle it landed: dx = ((hit/width) - 0.5) * 4. Returns
 * whether it hit. */
bool paddle_bounce(Ball& ball, int paddle_x, int paddle_top);

/* True if the ball's box overlaps the brick occupying (row, col). */
bool ball_overlaps_brick(const Ball& ball, int row, int col, int brick_width);

/* The brick grid cell currently under the ball, matching upstream
 * check_collisions. Returns false when the ball is outside the brick field. */
bool ball_brick_cell(const Ball& ball, int brick_width, int brick_cols,
                     int& out_row, int& out_col);

struct BrickHit {
    bool hit{false};
    int score_delta{0};
};

/* Resolve a collision with the brick at (row, col): if a brick is present there
 * and the ball overlaps it, remove it, decrement brick_count, flip dy and
 * report the score gained. Mirrors upstream check_brick_collision. */
BrickHit brick_collision(Ball& ball, std::vector<std::vector<bool>>& bricks,
                         int row, int col, int brick_width, unsigned& brick_count);

}  // namespace breakout_logic

enum class BreakoutState : uint8_t { Menu, Playing, GameOver };

class BreakoutView : public ui::View {
   public:
    BreakoutView();

    std::string title() const override { return "Breakout"; }

    void focus() override;
    void on_show() override;
    void on_frame_sync() override;
    void paint(ui::Painter& painter) override;
    bool on_key(ui::KeyEvent key) override;

   private:
    void init_game();
    void init_level();
    void reset_game();
    void update_game();
    void next_level();
    void move_paddle_left();
    void move_paddle_right();
    void launch_ball();

    /* Screen-dependent geometry, filled from screen_rect() at init. */
    int screen_w_{240};
    int screen_h_{304};
    int game_area_bottom_{0};
    int paddle_top_{0};
    int brick_width_{0};
    int brick_cols_{0};
    int paddle_speed_{10};

    int paddle_x_{0};
    breakout_logic::Ball ball_{};
    int score_{0};
    int lives_{3};
    int level_{1};
    BreakoutState state_{BreakoutState::Menu};
    bool ball_attached_{true};
    unsigned brick_count_{0};
    std::vector<std::vector<bool>> bricks_{};
    int brick_colors_[breakout_logic::kBrickRows]{};

    bool initialized_{false};
    uint32_t blink_counter_{0};

    ui::Button dummy_{{static_cast<int>(ui::screen_width), 0, 0, 0}, ""};
};

}  // namespace app

#endif /*__MB200_UI_BREAKOUT_H__*/
