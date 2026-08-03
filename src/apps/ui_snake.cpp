/*
 * mayhem-b200 — Snake (Games).
 *
 * Copyright (C) 2026 mayhem-b200 contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "ui_snake.hpp"

#include "app_context.hpp"
#include "theme.hpp"
#include "ui_navigation.hpp"

#include <chrono>

namespace app {

/* ---- snake_logic --------------------------------------------------------- */

namespace snake_logic {

void SnakeEngine::reset(int w, int h) {
    grid_w = w > 0 ? w : 1;
    grid_h = h > 0 ? h : 1;

    const int capacity = grid_w * grid_h;
    snake_x.assign(static_cast<size_t>(capacity), 0);
    snake_y.assign(static_cast<size_t>(capacity), 0);

    snake_x[0] = grid_w / 2;
    snake_y[0] = grid_h / 2;
    snake_length = 1;
    snake_dx = 1;
    snake_dy = 0;
    score = 0;
    spawn_food();
}

void SnakeEngine::set_direction(Dir d) {
    switch (d) {
        case Dir::Left:
            if (snake_dx == 0) {
                snake_dx = -1;
                snake_dy = 0;
            }
            break;
        case Dir::Right:
            if (snake_dx == 0) {
                snake_dx = 1;
                snake_dy = 0;
            }
            break;
        case Dir::Up:
            if (snake_dy == 0) {
                snake_dx = 0;
                snake_dy = -1;
            }
            break;
        case Dir::Down:
            if (snake_dy == 0) {
                snake_dx = 0;
                snake_dy = 1;
            }
            break;
    }
}

void SnakeEngine::spawn_food() {
    if (grid_w <= 0 || grid_h <= 0) return;
    bool valid;
    do {
        food_x = rng(grid_w);
        food_y = rng(grid_h);
        valid = true;
        for (int i = 0; i < snake_length; ++i) {
            if (snake_x[i] == food_x && snake_y[i] == food_y) {
                valid = false;
                break;
            }
        }
    } while (!valid);
}

SnakeEngine::StepResult SnakeEngine::step() {
    const int new_x = snake_x[0] + snake_dx;
    const int new_y = snake_y[0] + snake_dy;
    const bool ate_food = (new_x == food_x && new_y == food_y);

    const int tail_x = snake_x[snake_length - 1];
    const int tail_y = snake_y[snake_length - 1];

    for (int i = snake_length - 1; i > 0; --i) {
        snake_x[i] = snake_x[i - 1];
        snake_y[i] = snake_y[i - 1];
    }

    snake_x[0] = new_x;
    snake_y[0] = new_y;

    if (ate_food) {
        if (snake_length < static_cast<int>(snake_x.size())) {
            snake_x[snake_length] = tail_x;
            snake_y[snake_length] = tail_y;
            ++snake_length;
        }
        score += 10;
        spawn_food();
    }

    return {ate_food, check_collision()};
}

bool SnakeEngine::check_collision() const {
    if (snake_x[0] < 0 || snake_x[0] >= grid_w ||
        snake_y[0] < 0 || snake_y[0] >= grid_h) {
        return true;
    }
    for (int i = 1; i < snake_length; ++i) {
        if (snake_x[0] == snake_x[i] && snake_y[0] == snake_y[i]) {
            return true;
        }
    }
    return false;
}

}  // namespace snake_logic

/* ---- SnakeView ----------------------------------------------------------- */

using namespace snake_logic;

namespace {

const ui::Color kBackground = ui::Color::black();
const ui::Color kSnake = ui::Color::green();
const ui::Color kFood = ui::Color::red();
const ui::Color kBorder = ui::Color::white();

/* Centre an n-character string horizontally within a width-w area. */
int center_x(int w, int n) {
    const int text_w = n * static_cast<int>(ui::char_width);
    int x = (w - text_w) / 2;
    return x < 0 ? 0 : x;
}

}  // namespace

SnakeView::SnakeView() {
    /* The view holds keyboard focus itself so the dispatcher routes every key
     * to on_key (a game consumes the D-pad, so no child needs focus). */
    set_focusable(true);
}

void SnakeView::on_show() {
    View::on_show();

    if (!initialized_) {
        std::srand(static_cast<unsigned>(
            std::chrono::steady_clock::now().time_since_epoch().count()));
        /* Size the grid from the live view area (240x304 on this host). */
        const int w = screen_rect().width();
        const int h = screen_rect().height();
        const int game_area_height = h - kInfoBarHeight - 2;
        const int grid_w = (w - 2) / kCell;
        const int grid_h = game_area_height / kCell;
        engine_.reset(grid_w, grid_h);
        initialized_ = true;
    }

    focus();
    set_dirty();
}

void SnakeView::start_game() {
    engine_.reset(engine_.grid_w, engine_.grid_h);
    state_ = SnakeState::Playing;
    frame_counter_ = 0;
    set_dirty();
}

void SnakeView::tick() {
    const SnakeEngine::StepResult r = engine_.step();
    if (r.collided) {
        state_ = SnakeState::GameOver;
    }
    set_dirty();
}

void SnakeView::on_frame_sync() {
    View::on_frame_sync();

    if (state_ != SnakeState::Playing) return;

    if (++frame_counter_ >= kMoveFrames) {
        frame_counter_ = 0;
        tick();
    }
}

bool SnakeView::on_key(ui::KeyEvent key) {
    switch (key) {
        case ui::KeyEvent::Select:
            if (state_ == SnakeState::Menu || state_ == SnakeState::GameOver) {
                start_game();
            }
            return true;

        case ui::KeyEvent::Left:
            if (state_ == SnakeState::Playing) engine_.set_direction(Dir::Left);
            return true;
        case ui::KeyEvent::Right:
            if (state_ == SnakeState::Playing) engine_.set_direction(Dir::Right);
            return true;
        case ui::KeyEvent::Up:
            if (state_ == SnakeState::Playing) engine_.set_direction(Dir::Up);
            return true;
        case ui::KeyEvent::Down:
            if (state_ == SnakeState::Playing) engine_.set_direction(Dir::Down);
            return true;

        default:
            /* Back / Dfu fall through so the navigation stack can pop the app. */
            return false;
    }
}

void SnakeView::paint_menu(ui::Painter& painter, ui::Point o) const {
    const int w = screen_rect().width();
    const ui::Style yellow{ui::font::fixed_8x16, kBackground, ui::Color::yellow()};
    const ui::Style blue{ui::font::fixed_8x16, kBackground, ui::Color::blue()};
    const ui::Style green{ui::font::fixed_8x16, kBackground, ui::Color::green()};

    painter.draw_string({o.x() + center_x(w, 17), o.y() + 40}, yellow, "* * * SNAKE * * *");
    painter.draw_string({o.x() + center_x(w, 21), o.y() + 120}, blue, "USE THE D-PAD TO MOVE");
    painter.draw_string({o.x() + center_x(w, 27), o.y() + 150}, blue, "EAT THE RED SQUARES TO GROW");
    painter.draw_string({o.x() + center_x(w, 27), o.y() + 180}, blue, "DON'T HIT THE WALLS OR SELF");
    painter.draw_string({o.x() + center_x(w, 27), o.y() + 240}, green, "** PRESS SELECT TO START **");
}

void SnakeView::paint_playing(ui::Painter& painter, ui::Point o) const {
    const int w = screen_rect().width();
    const int h = screen_rect().height();

    /* Border around the game area, matching upstream draw_borders(). */
    painter.draw_rectangle({o.x(), o.y() + kGameAreaTop - 1, w, 1}, kBorder);
    painter.draw_rectangle({o.x(), o.y() + kGameAreaTop, w, h - kGameAreaTop}, kBorder);

    /* Food. */
    painter.fill_rectangle({o.x() + 1 + engine_.food_x * kCell,
                            o.y() + kGameAreaTop + engine_.food_y * kCell,
                            kCell, kCell},
                           kFood);

    /* Snake. */
    for (int i = 0; i < engine_.snake_length; ++i) {
        painter.fill_rectangle({o.x() + 1 + engine_.snake_x[i] * kCell,
                                o.y() + kGameAreaTop + engine_.snake_y[i] * kCell,
                                kCell, kCell},
                               kSnake);
    }

    /* Score. */
    const ui::Style blue{ui::font::fixed_8x16, kBackground, ui::Color::blue()};
    const std::string s = "Score: " + std::to_string(engine_.score);
    painter.draw_string({o.x() + center_x(w, static_cast<int>(s.size())), o.y() + 5}, blue, s);
}

void SnakeView::paint_game_over(ui::Painter& painter, ui::Point o) const {
    const int w = screen_rect().width();
    const ui::Style red{ui::font::fixed_8x16, kBackground, ui::Color::red()};
    const ui::Style yellow{ui::font::fixed_8x16, kBackground, ui::Color::yellow()};
    const ui::Style green{ui::font::fixed_8x16, kBackground, ui::Color::green()};

    painter.draw_string({o.x() + center_x(w, 9), o.y() + 90}, red, "GAME OVER");
    const std::string s = "SCORE: " + std::to_string(engine_.score);
    painter.draw_string({o.x() + center_x(w, static_cast<int>(s.size())), o.y() + 150}, yellow, s);
    painter.draw_string({o.x() + center_x(w, 23), o.y() + 220}, green, "PRESS SELECT TO RESTART");
}

void SnakeView::paint(ui::Painter& painter) {
    const ui::Point o = screen_pos();
    const int w = screen_rect().width();
    const int h = screen_rect().height();

    painter.fill_rectangle({o.x(), o.y(), w, h}, kBackground);

    switch (state_) {
        case SnakeState::Menu:
            paint_menu(painter, o);
            break;
        case SnakeState::Playing:
            paint_playing(painter, o);
            break;
        case SnakeState::GameOver:
            paint_game_over(painter, o);
            break;
    }
}

}  // namespace app

#include "app_registry.hpp"
#include "bitmaps.hpp"

namespace {
const app::Registrar reg_snake{{"snake", "Snake",
                                app::Category::Games, ui::Color::green(),
                                &ui::bitmap_icon_games,
                                [] { return std::make_unique<app::SnakeView>(); }}};
}  // namespace
