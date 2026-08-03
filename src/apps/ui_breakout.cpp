/*
 * mayhem-b200 — Breakout (Games).
 *
 * Copyright (C) 2026 mayhem-b200 contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "ui_breakout.hpp"

#include "app_context.hpp"
#include "theme.hpp"
#include "ui_navigation.hpp"

#include <string>

namespace app {

/* ---- breakout_logic ------------------------------------------------------ */

namespace breakout_logic {

int brick_left(int col, int brick_width) {
    return col * (brick_width + kBrickGap);
}

int brick_top(int row) {
    return kGameAreaTop + row * (kBrickHeight + kBrickGap) + 5;
}

int brick_score(int row) {
    return (kBrickRows - row) * 10;
}

bool wall_bounce(Ball& ball, int screen_width) {
    bool hit = false;
    if (ball.x < 0) {
        ball.x = 0;
        ball.dx = -ball.dx;
        hit = true;
    } else if (ball.x > static_cast<float>(screen_width - kBallSize)) {
        ball.x = static_cast<float>(screen_width - kBallSize);
        ball.dx = -ball.dx;
        hit = true;
    }
    if (ball.y < static_cast<float>(kGameAreaTop)) {
        ball.y = static_cast<float>(kGameAreaTop);
        ball.dy = -ball.dy;
        hit = true;
    }
    return hit;
}

bool paddle_bounce(Ball& ball, int paddle_x, int paddle_top) {
    if (ball.y + kBallSize >= paddle_top && ball.y <= paddle_top + kPaddleHeight &&
        ball.x + kBallSize >= paddle_x && ball.x <= paddle_x + kPaddleWidth) {
        ball.y = static_cast<float>(paddle_top - kBallSize);
        ball.dy = -ball.dy;

        const float hit_position = (ball.x + (kBallSize / 2)) - paddle_x;
        const float angle = (hit_position / kPaddleWidth) - 0.5f;
        ball.dx = angle * 4.0f;
        return true;
    }
    return false;
}

bool ball_overlaps_brick(const Ball& ball, int row, int col, int brick_width) {
    const int bx = brick_left(col, brick_width);
    const int by = brick_top(row);
    return ball.x + kBallSize >= bx && ball.x <= bx + brick_width &&
           ball.y + kBallSize >= by && ball.y <= by + kBrickHeight;
}

bool ball_brick_cell(const Ball& ball, int brick_width, int brick_cols,
                     int& out_row, int& out_col) {
    const int grid_x = static_cast<int>(ball.x / (brick_width + kBrickGap));
    const int grid_y = static_cast<int>((ball.y - kGameAreaTop - 5) /
                                        (kBrickHeight + kBrickGap));
    if (grid_x >= 0 && grid_x < brick_cols && grid_y >= 0 && grid_y < kBrickRows) {
        out_row = grid_y;
        out_col = grid_x;
        return true;
    }
    return false;
}

BrickHit brick_collision(Ball& ball, std::vector<std::vector<bool>>& bricks,
                         int row, int col, int brick_width, unsigned& brick_count) {
    if (!bricks[row][col]) return {false, 0};

    if (ball_overlaps_brick(ball, row, col, brick_width)) {
        bricks[row][col] = false;
        if (brick_count > 0) --brick_count;
        ball.dy = -ball.dy;
        return {true, brick_score(row)};
    }
    return {false, 0};
}

}  // namespace breakout_logic

/* ---- BreakoutView -------------------------------------------------------- */

using namespace breakout_logic;

BreakoutView::BreakoutView() {
    add_children({&dummy_});
    dummy_.on_dir = [this](ui::Button&, ui::KeyEvent key) { return on_key(key); };
    dummy_.on_select = [this](ui::Button&) { on_key(ui::KeyEvent::Select); };
}

void BreakoutView::focus() {
    dummy_.focus();
}

void BreakoutView::on_show() {
    View::on_show();
    if (!initialized_) {
        init_game();
        initialized_ = true;
    }
    focus();
    set_dirty();
}

void BreakoutView::init_game() {
    screen_w_ = screen_rect().width();
    screen_h_ = screen_rect().height();
    if (screen_w_ <= 0) screen_w_ = static_cast<int>(ui::screen_width);
    if (screen_h_ <= 0) screen_h_ = static_cast<int>(ui::screen_height);

    game_area_bottom_ = screen_h_ - 10;
    paddle_top_ = game_area_bottom_ - kPaddleHeight;
    paddle_speed_ = 10;

    int available_width = screen_w_ - 4;
    brick_cols_ = available_width / 22;
    if (brick_cols_ > 12) brick_cols_ = 12;
    if (brick_cols_ < 5) brick_cols_ = 5;
    brick_width_ = (available_width - (brick_cols_ - 1) * kBrickGap) / brick_cols_;

    paddle_x_ = (screen_w_ - kPaddleWidth) / 2;
    score_ = 0;
    lives_ = 3;
    level_ = 1;

    /* Row colours top→bottom, as upstream: red, orange, yellow, green, purple. */
    brick_colors_[0] = 0;
    brick_colors_[1] = 1;
    brick_colors_[2] = 2;
    brick_colors_[3] = 3;
    brick_colors_[4] = 4;

    bricks_.assign(kBrickRows, std::vector<bool>(brick_cols_, true));

    init_level();

    state_ = BreakoutState::Menu;
    blink_counter_ = 0;
}

void BreakoutView::init_level() {
    ball_.x = static_cast<float>(paddle_x_ + (kPaddleWidth / 2) - (kBallSize / 2));
    ball_.y = static_cast<float>(game_area_bottom_ - kPaddleHeight - kBallSize - 1);

    const float speed_multiplier = 1.0f + ((level_ - 1) * kBallSpeedIncrease);
    ball_.dx = 1.5f * speed_multiplier;
    ball_.dy = -2.0f * speed_multiplier;

    ball_attached_ = true;

    brick_count_ = 0;
    for (int row = 0; row < kBrickRows; ++row) {
        for (int col = 0; col < brick_cols_; ++col) {
            bricks_[row][col] = true;
            ++brick_count_;
        }
    }
}

void BreakoutView::reset_game() {
    level_ = 1;
    score_ = 0;
    lives_ = 3;
    state_ = BreakoutState::Playing;
    init_level();
}

void BreakoutView::next_level() {
    ++level_;
    init_level();
}

void BreakoutView::move_paddle_left() {
    paddle_x_ -= paddle_speed_;
    if (paddle_x_ < 0) paddle_x_ = 0;
    if (ball_attached_)
        ball_.x = static_cast<float>(paddle_x_ + (kPaddleWidth / 2) - (kBallSize / 2));
}

void BreakoutView::move_paddle_right() {
    paddle_x_ += paddle_speed_;
    if (paddle_x_ > screen_w_ - kPaddleWidth) paddle_x_ = screen_w_ - kPaddleWidth;
    if (ball_attached_)
        ball_.x = static_cast<float>(paddle_x_ + (kPaddleWidth / 2) - (kBallSize / 2));
}

void BreakoutView::launch_ball() {
    if (!ball_attached_) return;
    ball_attached_ = false;
    const float speed_multiplier = 1.0f + ((level_ - 1) * kBallSpeedIncrease);
    ball_.dx = 1.5f * speed_multiplier;
    ball_.dy = -2.0f * speed_multiplier;
}

void BreakoutView::update_game() {
    if (ball_attached_) return;

    ball_.x += ball_.dx;
    ball_.y += ball_.dy;

    wall_bounce(ball_, screen_w_);

    /* Bottom edge: lose a life (or the game). */
    if (ball_.y > game_area_bottom_) {
        --lives_;
        if (lives_ <= 0) {
            state_ = BreakoutState::GameOver;
            blink_counter_ = 0;
        } else {
            ball_attached_ = true;
            ball_.x = static_cast<float>(paddle_x_ + (kPaddleWidth / 2) - (kBallSize / 2));
            ball_.y = static_cast<float>(game_area_bottom_ - kPaddleHeight - kBallSize - 1);
        }
        return;
    }

    paddle_bounce(ball_, paddle_x_, paddle_top_);

    int row = 0;
    int col = 0;
    if (ball_brick_cell(ball_, brick_width_, brick_cols_, row, col)) {
        const BrickHit hit = brick_collision(ball_, bricks_, row, col, brick_width_, brick_count_);
        if (hit.hit) score_ += hit.score_delta;
    }

    if (brick_count_ == 0) next_level();
}

void BreakoutView::on_frame_sync() {
    View::on_frame_sync();
    if (state_ == BreakoutState::Playing) {
        update_game();
    } else {
        ++blink_counter_;
    }
    set_dirty();
}

bool BreakoutView::on_key(ui::KeyEvent key) {
    if (key == ui::KeyEvent::Select) {
        if (state_ == BreakoutState::Menu) {
            reset_game();
        } else if (state_ == BreakoutState::Playing && ball_attached_) {
            launch_ball();
        } else if (state_ == BreakoutState::GameOver) {
            state_ = BreakoutState::Menu;
            blink_counter_ = 0;
        }
        set_dirty();
        return true;
    }

    switch (key) {
        case ui::KeyEvent::Left:
            if (state_ == BreakoutState::Playing) move_paddle_left();
            set_dirty();
            return true;
        case ui::KeyEvent::Right:
            if (state_ == BreakoutState::Playing) move_paddle_right();
            set_dirty();
            return true;
        case ui::KeyEvent::Up:
        case ui::KeyEvent::Down:
            return true;  /* consumed; upstream used these for volume (no audio here) */
        default:
            return false; /* Back etc. fall through to the navigation dispatcher */
    }
}

void BreakoutView::paint(ui::Painter& painter) {
    const ui::Point o = screen_pos();
    painter.fill_rectangle({o.x(), o.y(), screen_w_, screen_h_}, ui::Color::black());

    const ui::Color row_colors[kBrickRows] = {
        ui::Color::red(), ui::Color::orange(), ui::Color::yellow(),
        ui::Color::green(), ui::Color::purple()};

    const ui::Style st_green{ui::font::fixed_8x16, ui::Color::black(), ui::Color::green()};
    const ui::Style st_red{ui::font::fixed_8x16, ui::Color::black(), ui::Color::red()};
    const ui::Style st_yellow{ui::font::fixed_8x16, ui::Color::black(), ui::Color::yellow()};
    const ui::Style st_cyan{ui::font::fixed_8x16, ui::Color::black(), ui::Color::cyan()};
    const ui::Style st_blue{ui::font::fixed_8x16, ui::Color::black(), ui::Color::blue()};

    auto center = [&](int y, const ui::Style& st, const std::string& s) {
        const int x = screen_w_ / 2 - static_cast<int>(s.size()) * ui::char_width / 2;
        painter.draw_string({o.x() + x, o.y() + y}, st, s);
    };

    if (state_ == BreakoutState::Menu) {
        center(40, st_yellow, "*** BREAKOUT ***");
        center(100, st_blue, "====================");
        center(130, st_cyan, "LEFT/RIGHT: PADDLE");
        center(160, st_cyan, "SELECT: START/LAUNCH");
        center(190, st_blue, "====================");
        if ((blink_counter_ / 30) % 2 == 0)
            center(230, st_red, "* PRESS SELECT *");
        return;
    }

    if (state_ == BreakoutState::GameOver) {
        center(100, st_red, "GAME OVER");
        center(150, st_yellow, "SCORE: " + std::to_string(score_));
        if ((blink_counter_ / 30) % 2 == 0)
            center(200, st_green, "PRESS SELECT");
        return;
    }

    /* Playing. */
    painter.draw_hline({o.x(), o.y() + kGameAreaTop}, screen_w_, ui::Color::white());

    for (int row = 0; row < kBrickRows; ++row) {
        for (int col = 0; col < brick_cols_; ++col) {
            if (!bricks_[row][col]) continue;
            const int bx = brick_left(col, brick_width_);
            const int by = brick_top(row);
            const ui::Rect r{o.x() + bx, o.y() + by, brick_width_, kBrickHeight};
            painter.fill_rectangle(r, row_colors[brick_colors_[row]]);
            painter.draw_rectangle(r, ui::Color::black());
        }
    }

    painter.fill_rectangle(
        {o.x() + paddle_x_, o.y() + paddle_top_, kPaddleWidth, kPaddleHeight},
        ui::Color::blue());
    painter.fill_rectangle(
        {o.x() + static_cast<int>(ball_.x), o.y() + static_cast<int>(ball_.y),
         kBallSize, kBallSize},
        ui::Color::white());

    painter.draw_string({o.x() + 5, o.y() + 10}, st_green, "Score: " + std::to_string(score_));
    painter.draw_string({o.x() + 5, o.y() + 30}, st_red, "Lives: " + std::to_string(lives_));
    painter.draw_string({o.x() + 90, o.y() + 30}, st_yellow, "Level: " + std::to_string(level_));
}

}  // namespace app

#include "app_registry.hpp"
#include "bitmaps.hpp"

namespace {
const app::Registrar reg_breakout{{"breakout", "Breakout",
                                   app::Category::Games, ui::Color::green(),
                                   &ui::bitmap_icon_games,
                                   [] { return std::make_unique<app::BreakoutView>(); }}};
}  // namespace
