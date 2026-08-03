/*
 * mayhem-b200 — 2048 (Games).
 *
 * Copyright (C) 2026 mayhem-b200 contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "ui_game2048.hpp"

#include "app_context.hpp"
#include "theme.hpp"
#include "ui_navigation.hpp"

#include <chrono>
#include <string>
#include <utility>
#include <vector>

namespace app {

namespace {

/* Rendering constants, ported from upstream ui_game2048.hpp. */
constexpr int kTileMargin = 5;
constexpr int kBoardStartX = 15;
constexpr int kBoardStartY = 35;
constexpr int kInfoHeight = 30;

}  // namespace

/* ---- game2048_logic ------------------------------------------------------ */

namespace game2048_logic {

namespace {

void compress_left(Row& row) {
    int temp[kGridSize] = {0, 0, 0, 0};
    int index = 0;
    for (int i = 0; i < kGridSize; ++i) {
        if (row[i] != 0) temp[index++] = row[i];
    }
    for (int i = 0; i < kGridSize; ++i) row[i] = temp[i];
}

}  // namespace

MoveResult move_row_left(Row& row) {
    int original[kGridSize];
    for (int i = 0; i < kGridSize; ++i) original[i] = row[i];

    compress_left(row);

    int score_delta = 0;
    for (int i = 0; i < kGridSize - 1; ++i) {
        if (row[i] != 0 && row[i] == row[i + 1]) {
            row[i] *= 2;
            score_delta += row[i];
            row[i + 1] = 0;
        }
    }

    compress_left(row);

    bool moved = false;
    for (int i = 0; i < kGridSize; ++i) {
        if (original[i] != row[i]) {
            moved = true;
            break;
        }
    }
    return {moved, score_delta};
}

void rotate_clockwise(Grid& grid) {
    int temp[kGridSize][kGridSize];
    for (int i = 0; i < kGridSize; ++i)
        for (int j = 0; j < kGridSize; ++j)
            temp[j][kGridSize - 1 - i] = grid[i][j];
    for (int i = 0; i < kGridSize; ++i)
        for (int j = 0; j < kGridSize; ++j)
            grid[i][j] = temp[i][j];
}

void rotate_counter_clockwise(Grid& grid) {
    int temp[kGridSize][kGridSize];
    for (int i = 0; i < kGridSize; ++i)
        for (int j = 0; j < kGridSize; ++j)
            temp[kGridSize - 1 - j][i] = grid[i][j];
    for (int i = 0; i < kGridSize; ++i)
        for (int j = 0; j < kGridSize; ++j)
            grid[i][j] = temp[i][j];
}

MoveResult move_left(Grid& grid) {
    MoveResult result{false, 0};
    for (int i = 0; i < kGridSize; ++i) {
        const MoveResult r = move_row_left(grid[i]);
        result.moved = result.moved || r.moved;
        result.score_delta += r.score_delta;
    }
    return result;
}

MoveResult move_right(Grid& grid) {
    rotate_clockwise(grid);
    rotate_clockwise(grid);
    const MoveResult r = move_left(grid);
    rotate_clockwise(grid);
    rotate_clockwise(grid);
    return r;
}

MoveResult move_up(Grid& grid) {
    rotate_counter_clockwise(grid);
    const MoveResult r = move_left(grid);
    rotate_clockwise(grid);
    return r;
}

MoveResult move_down(Grid& grid) {
    rotate_clockwise(grid);
    const MoveResult r = move_left(grid);
    rotate_counter_clockwise(grid);
    return r;
}

bool can_move(const Grid& grid) {
    for (int i = 0; i < kGridSize; ++i) {
        for (int j = 0; j < kGridSize; ++j) {
            if (grid[i][j] == 0) return true;
            if (j < kGridSize - 1 && grid[i][j] == grid[i][j + 1]) return true;
            if (i < kGridSize - 1 && grid[i][j] == grid[i + 1][j]) return true;
        }
    }
    return false;
}

bool is_won(const Grid& grid) {
    for (int i = 0; i < kGridSize; ++i)
        for (int j = 0; j < kGridSize; ++j)
            if (grid[i][j] == 2048) return true;
    return false;
}

}  // namespace game2048_logic

/* ---- Game2048View -------------------------------------------------------- */

using namespace game2048_logic;

Game2048View::Game2048View() {
    add_children({&dummy_});

    dummy_.on_dir = [this](ui::Button&, ui::KeyEvent key) { return on_key(key); };
    dummy_.on_select = [this](ui::Button&) { on_key(ui::KeyEvent::Select); };

    /* Same tile-size formula upstream computes in its constructor. */
    tile_size_ = (static_cast<int>(ui::screen_width) - 2 * kBoardStartX -
                  (kGridSize + 1) * kTileMargin) /
                 kGridSize;

    for (int i = 0; i < kGridSize; ++i)
        for (int j = 0; j < kGridSize; ++j) grid_[i][j] = 0;
}

void Game2048View::focus() {
    dummy_.focus();
}

void Game2048View::on_show() {
    View::on_show();
    if (!initialized_) {
        rng_.seed(static_cast<uint32_t>(
            std::chrono::steady_clock::now().time_since_epoch().count()));
        reset_game();
        initialized_ = true;
    }
    focus();
    set_dirty();
}

void Game2048View::reset_game() {
    for (int i = 0; i < kGridSize; ++i)
        for (int j = 0; j < kGridSize; ++j) grid_[i][j] = 0;
    score_ = 0;
    state_ = Game2048State::Playing;
    add_random_tile();
    add_random_tile();
    set_dirty();
}

void Game2048View::add_random_tile() {
    std::vector<std::pair<int, int>> empty_cells;
    for (int i = 0; i < kGridSize; ++i)
        for (int j = 0; j < kGridSize; ++j)
            if (grid_[i][j] == 0) empty_cells.emplace_back(i, j);

    if (empty_cells.empty()) return;

    std::uniform_int_distribution<size_t> pick(0, empty_cells.size() - 1);
    const auto cell = empty_cells[pick(rng_)];
    /* 1-in-10 spawns a 4, matching upstream's rand() % 10 == 0. */
    std::uniform_int_distribution<int> four_chance(0, 9);
    grid_[cell.first][cell.second] = (four_chance(rng_) == 0) ? 4 : 2;
}

void Game2048View::apply_move(const MoveResult& r) {
    if (!r.moved) return;

    score_ += r.score_delta;
    add_random_tile();
    if (score_ > best_score_) best_score_ = score_;

    if (is_won(grid_) && state_ == Game2048State::Playing) {
        state_ = Game2048State::Won;
    } else if (!can_move(grid_)) {
        state_ = Game2048State::GameOver;
    }
    set_dirty();
}

bool Game2048View::on_key(ui::KeyEvent key) {
    if (key == ui::KeyEvent::Select) {
        if (state_ == Game2048State::GameOver || state_ == Game2048State::Won) {
            reset_game();
            return true;
        }
        return false;
    }

    if (state_ != Game2048State::Playing) return false;

    switch (key) {
        case ui::KeyEvent::Left:
            apply_move(move_left(grid_));
            return true;
        case ui::KeyEvent::Right:
            apply_move(move_right(grid_));
            return true;
        case ui::KeyEvent::Up:
            apply_move(move_up(grid_));
            return true;
        case ui::KeyEvent::Down:
            apply_move(move_down(grid_));
            return true;
        default:
            return false;
    }
}

ui::Color Game2048View::tile_color(int value) const {
    switch (value) {
        case 0: return ui::Color::light_grey();
        case 2: return ui::Color::white();
        case 4: return ui::Color::yellow();
        case 8: return ui::Color::orange();
        case 16: return ui::Color::red();
        case 32: return ui::Color::magenta();
        case 64: return ui::Color::purple();
        case 128: return ui::Color::blue();
        case 256: return ui::Color::cyan();
        case 512: return ui::Color::green();
        case 1024: return ui::Color::dark_green();
        case 2048: return ui::Color::dark_red();
        default: return ui::Color::black();
    }
}

ui::Color Game2048View::text_color(int value) const {
    return (value <= 4) ? ui::Color::black() : ui::Color::white();
}

void Game2048View::draw_tile(ui::Painter& painter, ui::Point origin, int x, int y, int value) const {
    const int tile_x = kBoardStartX + x * (tile_size_ + kTileMargin);
    const int tile_y = kBoardStartY + y * (tile_size_ + kTileMargin);
    const ui::Color color = tile_color(value);

    const ui::Rect r{origin.x() + tile_x, origin.y() + tile_y, tile_size_, tile_size_};
    painter.fill_rectangle(r, color);
    painter.draw_rectangle(r, ui::Color::dark_grey());

    if (value > 0) {
        const std::string s = std::to_string(value);
        const int text_w = static_cast<int>(s.size()) * ui::char_width;
        const int text_h = ui::char_height;
        const int tx = tile_x + (tile_size_ - text_w) / 2;
        const int ty = tile_y + (tile_size_ - text_h) / 2;
        const ui::Style st{ui::font::fixed_8x16, color, text_color(value)};
        painter.draw_string({origin.x() + tx, origin.y() + ty}, st, s);
    }
}

void Game2048View::paint(ui::Painter& painter) {
    const ui::Point o = screen_pos();
    const int w = screen_rect().width();
    const int h = screen_rect().height();

    /* Score bar over a solid black strip, board over dark grey. */
    painter.fill_rectangle({o.x(), o.y(), w, kInfoHeight}, ui::Color::black());
    painter.fill_rectangle({o.x(), o.y() + kInfoHeight, w, h - kInfoHeight},
                           ui::Color::dark_grey());

    for (int i = 0; i < kGridSize; ++i)
        for (int j = 0; j < kGridSize; ++j)
            draw_tile(painter, o, j, i, grid_[i][j]);

    const ui::Style score_style{ui::font::fixed_8x16, ui::Color::black(), ui::Color::white()};
    painter.draw_string({o.x() + 10, o.y() + 5}, score_style,
                        "Score: " + std::to_string(score_));
    if (best_score_ > 0)
        painter.draw_string({o.x() + 150, o.y() + 5}, score_style,
                            "Best: " + std::to_string(best_score_));

    if (state_ == Game2048State::GameOver || state_ == Game2048State::Won) {
        const ui::Rect box{o.x() + 40, o.y() + 100, 160, 100};
        painter.fill_rectangle(box, ui::Color::black());
        painter.draw_rectangle(box, ui::Color::white());
        const ui::Style ov{ui::font::fixed_8x16, ui::Color::black(), ui::Color::white()};
        const char* head = (state_ == Game2048State::Won) ? "YOU WON!" : "GAME OVER";
        const int head_len = static_cast<int>(std::string(head).size());
        painter.draw_string({o.x() + 120 - head_len * (ui::char_width / 2), o.y() + 115}, ov, head);
        painter.draw_string({o.x() + 120 - 12 * (ui::char_width / 2), o.y() + 135}, ov, "Press SELECT");
        painter.draw_string({o.x() + 120 - 10 * (ui::char_width / 2), o.y() + 155}, ov, "to restart");
    }
}

}  // namespace app

#include "app_registry.hpp"
#include "bitmaps.hpp"

namespace {
const app::Registrar reg_game2048{{"game2048", "2048",
                                   app::Category::Games, ui::Color::yellow(),
                                   &ui::bitmap_icon_games,
                                   [] { return std::make_unique<app::Game2048View>(); }}};
}  // namespace
