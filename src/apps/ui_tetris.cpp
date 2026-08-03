/*
 * mayhem-b200 — Tetris (Games).
 *
 * Copyright (C) 2026 mayhem-b200 contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "ui_tetris.hpp"

#include "app_context.hpp"
#include "theme.hpp"
#include "ui_navigation.hpp"

#include <algorithm>
#include <chrono>
#include <cstdlib>

namespace app {

/* ---- tetris_logic -------------------------------------------------------- */

namespace tetris_logic {

void TetrisEngine::clear_board() {
    for (int r = 0; r < kRows; ++r)
        for (int c = 0; c < kCols; ++c)
            board[r][c] = 0;
}

void TetrisEngine::spawn(unsigned char c) {
    colorIndex = c;
    boardY = 4;
    boardX = (c == 1) ? -1 : 0;  /* the I-tetromino spawns one row higher */
    for (int i = 0; i < 4; ++i) {
        X[i] = figuresX[c - 1][i];
        Y[i] = figuresY[c - 1][i];
    }
    rotation_state = 0;
}

bool TetrisEngine::inCollisionDown(int delta) const {
    for (int i = 0; i < 4; ++i) {
        const int nr = boardX + X[i] + delta;
        const int nc = boardY + Y[i];
        if (bottomEdge(nr)) return true;
        if (nr >= 0 && nr < kRows && nc >= 0 && nc < kCols && board[nr][nc] != 0)
            return true;
    }
    return false;
}

bool TetrisEngine::inCollisionLeft() const {
    for (int i = 0; i < 4; ++i) {
        const int nr = boardX + X[i];
        const int nc = boardY + Y[i] - 1;
        if (leftEdge(nc)) return true;
        if (nr >= 0 && nr < kRows && nc >= 0 && nc < kCols && board[nr][nc] != 0)
            return true;
    }
    return false;
}

bool TetrisEngine::inCollisionRight() const {
    for (int i = 0; i < 4; ++i) {
        const int nr = boardX + X[i];
        const int nc = boardY + Y[i] + 1;
        if (rightEdge(nc)) return true;
        if (nr >= 0 && nr < kRows && nc >= 0 && nc < kCols && board[nr][nc] != 0)
            return true;
    }
    return false;
}

bool TetrisEngine::moveDown(int delta) {
    if (!inCollisionDown(delta)) {
        boardX = static_cast<short>(boardX + delta);
        return true;
    }
    return false;
}

bool TetrisEngine::moveLeft() {
    if (!inCollisionLeft()) {
        boardY = static_cast<short>(boardY - 1);
        return true;
    }
    return false;
}

bool TetrisEngine::moveRight() {
    if (!inCollisionRight()) {
        boardY = static_cast<short>(boardY + 1);
        return true;
    }
    return false;
}

bool TetrisEngine::rotate() {
    short newX[4], newY[4];
    const int next_state = (rotation_state + 1) % 4;

    if (colorIndex == 2) return false;  /* O-tetromino does not rotate */

    static const short kick_tests_I[4][5][2] = {
        {{0, 0}, {-2, 0}, {1, 0}, {-2, -1}, {1, 2}},
        {{0, 0}, {-1, 0}, {2, 0}, {-1, 2}, {2, -1}},
        {{0, 0}, {2, 0}, {-1, 0}, {2, 1}, {-1, -2}},
        {{0, 0}, {1, 0}, {-2, 0}, {1, -2}, {-2, 1}}};
    static const short kick_tests_other[4][5][2] = {
        {{0, 0}, {-1, 0}, {-1, 1}, {0, -2}, {-1, -2}},
        {{0, 0}, {1, 0}, {1, -1}, {0, 2}, {1, 2}},
        {{0, 0}, {1, 0}, {1, 1}, {0, -2}, {1, -2}},
        {{0, 0}, {-1, 0}, {-1, -1}, {0, 2}, {-1, 2}}};

    const bool is_I = (colorIndex == 1);
    const short(*kick_tests)[5][2] = is_I ? kick_tests_I : kick_tests_other;

    for (int test = 0; test < 5; ++test) {
        const short kickX = kick_tests[rotation_state][test][0];
        const short kickY = kick_tests[rotation_state][test][1];

        bool ok = true;
        for (int i = 0; i < 4; ++i) {
            const short tmpX = static_cast<short>(X[i] - X[1]);
            const short tmpY = static_cast<short>(Y[i] - Y[1]);
            newX[i] = static_cast<short>(X[1] - tmpY);
            newY[i] = static_cast<short>(Y[1] + tmpX);
            const int testX = boardX + newX[i] + kickX;
            const int testY = boardY + newY[i] + kickY;
            if (testX < 0 || outOfBounds(testY, testX) ||
                (testX >= 0 && board[testX][testY] != 0)) {
                ok = false;
                break;
            }
        }
        if (!ok) continue;

        for (int i = 0; i < 4; ++i) {
            X[i] = newX[i];
            Y[i] = newY[i];
        }
        boardX = static_cast<short>(boardX + kickX);
        boardY = static_cast<short>(boardY + kickY);
        rotation_state = next_state;
        return true;
    }
    return false;
}

void TetrisEngine::onAttached() {
    for (int i = 0; i < 4; ++i) {
        const int r = boardX + X[i];
        const int c = boardY + Y[i];
        if (r >= 0 && r < kRows && c >= 0 && c < kCols)
            board[r][c] = colorIndex;
    }
}

void TetrisEngine::checkLines(short& firstLine, short& numberOfLines) const {
    firstLine = -1;
    numberOfLines = 0;
    for (int i = kRows - 1; i >= 0; --i) {
        short temp = 0;
        for (int j = 0; j < kCols; ++j) {
            if (board[i][j] == 0) {
                if (numberOfLines > 0) return;
                break;
            }
            ++temp;
        }
        if (temp == kCols) {
            ++numberOfLines;
            if (firstLine == -1) firstLine = static_cast<short>(i);
        }
    }
}

unsigned int TetrisEngine::updateScore(short numOfLines) const {
    unsigned int base = 0;
    switch (numOfLines) {
        case 1: base = 40; break;
        case 2: base = 100; break;
        case 3: base = 300; break;
        case 4: base = 1200; break;
        default: base = 0; break;
    }
    return base * (static_cast<unsigned int>(level) + 1u);
}

int TetrisEngine::updateBoard() {
    short firstLine, numberOfLines;
    int total = 0;
    do {
        checkLines(firstLine, numberOfLines);
        for (int i = firstLine; i >= numberOfLines; --i) {
            for (int j = 0; j < kCols; ++j) {
                board[i][j] = board[i - numberOfLines][j];
                board[i - numberOfLines][j] = 0;
            }
        }
        score += updateScore(numberOfLines);
        total += numberOfLines;
    } while (numberOfLines != 0);
    return total;
}

bool TetrisEngine::isOver() const {
    for (int j = 0; j < kCols; ++j)
        if (board[0][j] != 0) return true;
    return false;
}

}  // namespace tetris_logic

/* ---- TetrisView ---------------------------------------------------------- */

using namespace tetris_logic;

namespace {

/* Palette, verbatim from upstream pp_colors[]; board cells hold colour indices
 * 1..7 which index directly into this table. */
const ui::Color pp_colors[8] = {
    ui::Color::white(),
    ui::Color::blue(),
    ui::Color::yellow(),
    ui::Color::purple(),
    ui::Color::green(),
    ui::Color::red(),
    ui::Color::magenta(),
    ui::Color::orange(),
};

int center_x(int w, int n) {
    const int text_w = n * static_cast<int>(ui::char_width);
    int x = (w - text_w) / 2;
    return x < 0 ? 0 : x;
}

}  // namespace

TetrisView::TetrisView() {
    /* The view holds keyboard focus itself so both keys and the encoder reach
     * on_key / on_encoder (the dispatcher offers the focused widget first). */
    set_focusable(true);
}

unsigned char TetrisView::random_piece() {
    return static_cast<unsigned char>((std::rand() % 7) + 1);
}

void TetrisView::compute_dimensions() {
    const int w = screen_rect().width();
    const int h = screen_rect().height();

    if (w == 240 && h == 320) {
        dimension_ = 16;
        dimension_next_ = 12;
        board_right_ = 162;
        info_left_ = 165;
    } else {
        const int available_width = w * 3 / 4;
        const int available_height = h - 10;
        const int max_dim_w = available_width / 10;
        const int max_dim_h = available_height / 20;
        dimension_ = std::min(max_dim_w, max_dim_h);
        if (dimension_ < 8) dimension_ = 8;
        dimension_next_ = dimension_ * 3 / 4;
        board_right_ = 10 * dimension_ + 2;
        info_left_ = board_right_ + 3;
    }
}

void TetrisView::on_show() {
    View::on_show();

    if (!initialized_) {
        std::srand(static_cast<unsigned>(
            std::chrono::steady_clock::now().time_since_epoch().count()));
        compute_dimensions();
        engine_.clear_board();
        engine_.level = 0;
        engine_.score = 0;
        state_ = TetrisState::LevelMenu;
        initialized_ = true;
    }

    focus();
    set_dirty();
}

void TetrisView::change_level(int delta) {
    if (delta > 0) {
        engine_.level = static_cast<unsigned char>((engine_.level + 1) % 4);
    } else if (delta < 0) {
        engine_.level = (engine_.level == 0)
                            ? 3
                            : static_cast<unsigned char>(engine_.level - 1);
    }
}

void TetrisView::start_game() {
    engine_.clear_board();
    engine_.score = 0;
    engine_.spawn(random_piece());
    next_figure_ = random_piece();
    state_ = TetrisState::Playing;
    paused_ = false;
    frame_counter_ = 0;
    fall_frames_ = static_cast<uint32_t>(engine_.delays[engine_.level] * 60.0f);
    if (fall_frames_ == 0) fall_frames_ = 1;
    set_dirty();
}

void TetrisView::soft_drop() {
    engine_.moveDown(2);
    engine_.score += 2u * (static_cast<unsigned int>(engine_.level) + 1u);
}

void TetrisView::play_step() {
    if (!engine_.moveDown(1)) {
        engine_.onAttached();
        engine_.updateBoard();
        engine_.spawn(next_figure_);
        next_figure_ = random_piece();
        if (engine_.isOver()) {
            state_ = TetrisState::GameOver;
        }
    }
    set_dirty();
}

void TetrisView::on_frame_sync() {
    View::on_frame_sync();

    if (state_ != TetrisState::Playing || paused_) return;

    if (++frame_counter_ >= fall_frames_) {
        frame_counter_ = 0;
        play_step();
    }
}

bool TetrisView::on_key(ui::KeyEvent key) {
    if (key == ui::KeyEvent::Select) {
        if (state_ == TetrisState::LevelMenu) {
            start_game();
        } else if (state_ == TetrisState::Playing) {
            engine_.rotate();
        } else {  /* GameOver */
            engine_.clear_board();
            state_ = TetrisState::LevelMenu;
        }
        set_dirty();
        return true;
    }

    if (state_ == TetrisState::LevelMenu) {
        if (key == ui::KeyEvent::Up) {
            change_level(-1);
        } else if (key == ui::KeyEvent::Down) {
            change_level(+1);
        } else {
            return false;
        }
        set_dirty();
        return true;
    }

    if (state_ == TetrisState::Playing) {
        switch (key) {
            case ui::KeyEvent::Left: engine_.moveLeft(); break;
            case ui::KeyEvent::Right: engine_.moveRight(); break;
            case ui::KeyEvent::Down: soft_drop(); break;
            case ui::KeyEvent::Up: paused_ = !paused_; break;
            default: return false;
        }
        set_dirty();
        return true;
    }

    /* GameOver: swallow the D-pad so focus stays put; Select handled above. */
    if (key == ui::KeyEvent::Left || key == ui::KeyEvent::Right ||
        key == ui::KeyEvent::Up || key == ui::KeyEvent::Down) {
        return true;
    }
    return false;
}

bool TetrisView::on_encoder(ui::EncoderEvent delta) {
    if (state_ == TetrisState::LevelMenu) {
        if (delta > 0)
            change_level(+1);
        else if (delta < 0)
            change_level(-1);
        set_dirty();
        return true;
    }
    if (state_ == TetrisState::Playing) {
        if (delta != 0) engine_.rotate();
        set_dirty();
        return true;
    }
    return false;
}

/* ---- rendering ----------------------------------------------------------- */

void TetrisView::draw_cell(ui::Painter& painter, ui::Point o, int row, int col, int color_index) const {
    const int px = o.x() + col * dimension_;
    const int py = o.y() + row * dimension_;
    const ui::Color c = pp_colors[color_index & 7];
    painter.fill_rectangle({px, py, dimension_, dimension_}, c);
    painter.draw_rectangle({px, py, dimension_, dimension_}, ui::Color::black());
}

void TetrisView::paint_menu(ui::Painter& painter, ui::Point o) const {
    const int w = screen_rect().width();
    const ui::Style white{ui::font::fixed_8x16, ui::Color::black(), ui::Color::white()};
    const ui::Style green{ui::font::fixed_8x16, ui::Color::black(), ui::Color::green()};

    painter.draw_string({o.x() + center_x(w, 6), o.y() + 20}, green, "TETRIS");

    const int ys[4] = {60, 110, 160, 210};
    for (int lvl = 0; lvl < 4; ++lvl) {
        const std::string s = "LEVEL " + std::to_string(lvl + 1);
        const bool selected = (engine_.level == lvl);
        const ui::Style st = selected ? green : white;
        painter.draw_string({o.x() + center_x(w, 7), o.y() + ys[lvl]}, st, s);
        if (selected) {
            painter.fill_rectangle(
                {o.x() + center_x(w, 7) - 16, o.y() + ys[lvl] + 2, 10, 10},
                ui::Color::green());
        }
    }

    painter.draw_string({o.x() + center_x(w, 20), o.y() + 270}, white,
                        "SELECT to start play");
}

void TetrisView::paint_playing(ui::Painter& painter, ui::Point o) const {
    const int w = screen_rect().width();
    const int h = screen_rect().height();

    /* Divider between the board and the info column. */
    painter.draw_rectangle({o.x() + board_right_, o.y(), 2, h}, ui::Color::white());

    /* Locked cells. */
    for (int r = 0; r < TetrisEngine::kRows; ++r)
        for (int c = 0; c < TetrisEngine::kCols; ++c)
            if (engine_.board[r][c] != 0)
                draw_cell(painter, o, r, c, engine_.board[r][c]);

    /* Falling piece. */
    for (int i = 0; i < 4; ++i) {
        const int r = engine_.boardX + engine_.X[i];
        const int c = engine_.boardY + engine_.Y[i];
        if (r >= 0)
            draw_cell(painter, o, r, c, engine_.colorIndex);
    }

    /* Info column: score and next piece. */
    const ui::Style white{ui::font::fixed_8x16, ui::Color::black(), ui::Color::white()};
    painter.draw_string({o.x() + info_left_, o.y() + 10}, white, "SCORE");
    painter.draw_string({o.x() + info_left_, o.y() + 30}, white,
                        std::to_string(engine_.score));
    painter.draw_string({o.x() + info_left_, o.y() + 66}, white, "NEXT");

    const int next_ox = o.x() + info_left_ + 6;
    const int next_oy = o.y() + 86;
    for (int i = 0; i < 4; ++i) {
        const int cx = next_ox + dimension_next_ * TetrisEngine::figuresY[next_figure_ - 1][i];
        const int cy = next_oy + dimension_next_ * TetrisEngine::figuresX[next_figure_ - 1][i];
        painter.fill_rectangle({cx, cy, dimension_next_, dimension_next_},
                               pp_colors[next_figure_ & 7]);
        painter.draw_rectangle({cx, cy, dimension_next_, dimension_next_},
                               ui::Color::black());
    }

    if (paused_) {
        painter.draw_string({o.x() + center_x(w, 6), o.y() + h / 2}, white, "PAUSED");
    }
}

void TetrisView::paint_game_over(ui::Painter& painter, ui::Point o) const {
    const int w = screen_rect().width();
    const ui::Style red{ui::font::fixed_8x16, ui::Color::black(), ui::Color::red()};
    const ui::Style white{ui::font::fixed_8x16, ui::Color::black(), ui::Color::white()};
    const ui::Style green{ui::font::fixed_8x16, ui::Color::black(), ui::Color::green()};

    painter.draw_string({o.x() + center_x(w, 9), o.y() + 110}, red, "GAME OVER");
    const std::string s = "YOUR SCORE IS " + std::to_string(engine_.score);
    painter.draw_string({o.x() + center_x(w, static_cast<int>(s.size())), o.y() + 150},
                        white, s);
    painter.draw_string({o.x() + center_x(w, 20), o.y() + 200}, green,
                        "SELECT for the menu");
}

void TetrisView::paint(ui::Painter& painter) {
    const ui::Point o = screen_pos();
    const int w = screen_rect().width();
    const int h = screen_rect().height();

    painter.fill_rectangle({o.x(), o.y(), w, h}, ui::Color::black());

    switch (state_) {
        case TetrisState::LevelMenu:
            paint_menu(painter, o);
            break;
        case TetrisState::Playing:
            paint_playing(painter, o);
            break;
        case TetrisState::GameOver:
            paint_game_over(painter, o);
            break;
    }
}

}  // namespace app

#include "app_registry.hpp"
#include "bitmaps.hpp"

namespace {
const app::Registrar reg_tetris{{"tetris", "Tetris",
                                 app::Category::Games, ui::Color::green(),
                                 &ui::bitmap_icon_games,
                                 [] { return std::make_unique<app::TetrisView>(); }}};
}  // namespace
