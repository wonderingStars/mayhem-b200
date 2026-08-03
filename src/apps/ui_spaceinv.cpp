/*
 * mayhem-b200 — Space Invaders game.
 *
 * Copyright (C) 2024 RocketGod (original game)
 * Copyright (C) 2026 mayhem-b200 contributors (host implementation)
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "ui_spaceinv.hpp"

#include "app_context.hpp"
#include "theme.hpp"
#include "ui_navigation.hpp"

#include <cstdlib>
#include <string>

namespace app {

namespace spaceinv_game {

/* --- Dimensions: mirrors upstream init_dimensions() ------------------------ */

Dimensions Dimensions::make(int w, int h) {
    Dimensions d;
    d.screen_w = w;
    d.screen_h = h;

    d.player_w = w / 9;
    d.player_h = h / 20;
    d.player_y = h - 40;

    int available_width = w - 40;  /* 20 px margin each side */
    d.cols = available_width / (INVADER_WIDTH + INVADER_GAP_X);
    if (d.cols > MAX_INVADER_COLS) d.cols = MAX_INVADER_COLS;
    if (d.cols < 6) d.cols = 6;

    int formation_width = d.cols * INVADER_WIDTH + (d.cols - 1) * INVADER_GAP_X;
    d.start_x = (w - formation_width) / 2;
    d.start_y = INFO_BAR_HEIGHT + 10;

    d.move_amount = w / 30;
    d.drop_amount = h / 21;
    return d;
}

int score_for_row(int row) {
    if (row == 0) return 30;
    if (row == 1 || row == 2) return 20;
    return 10;
}

/* --- Game ----------------------------------------------------------------- */

int Game::active_invader_count() const {
    int n = 0;
    for (int row = 0; row < INVADER_ROWS; row++)
        for (int col = 0; col < dim.cols; col++)
            if (invaders[row][col]) n++;
    return n;
}

void Game::set_dimensions(int w, int h) {
    dim = Dimensions::make(w, h);
    player_x = dim.screen_w / 2 - dim.player_w / 2;
}

void Game::init_invaders() {
    for (int row = 0; row < INVADER_ROWS; row++)
        for (int col = 0; col < MAX_INVADER_COLS; col++)
            invaders[row][col] = (col < dim.cols);

    invaders_x = dim.start_x;
    invaders_y = dim.start_y;
    invader_direction = 1;
    invader_move_counter = 0;

    for (int i = 0; i < MAX_ENEMY_BULLETS; i++) enemy_bullets[i].active = false;
    for (int i = 0; i < MAX_BULLETS; i++) bullets[i].active = false;
}

void Game::reset_game() {
    score = 0;
    lives = 3;
    wave = 1;
    speed_bonus = 0;
    invader_move_delay = BASE_MOVE_DELAY;
    enemy_fire_counter = 0;
    player_x = dim.screen_w / 2 - dim.player_w / 2;
    for (int i = 0; i < MAX_BULLETS; i++) bullets[i] = Bullet{};
    for (int i = 0; i < MAX_ENEMY_BULLETS; i++) enemy_bullets[i] = Bullet{};
    init_invaders();
}

void Game::fire_bullet() {
    for (int i = 0; i < MAX_BULLETS; i++) {
        if (!bullets[i].active) {
            bullets[i].active = true;
            bullets[i].x = player_x + dim.player_w / 2 - BULLET_WIDTH / 2;
            bullets[i].y = dim.player_y - BULLET_HEIGHT;
            break;
        }
    }
}

void Game::update_bullets() {
    for (int i = 0; i < MAX_BULLETS; i++) {
        if (!bullets[i].active) continue;
        bullets[i].y -= BULLET_SPEED;
        if (bullets[i].y < INFO_BAR_HEIGHT) bullets[i].active = false;
    }
}

bool Game::check_collisions() {
    for (int i = 0; i < MAX_BULLETS; i++) {
        if (!bullets[i].active) continue;

        for (int row = 0; row < INVADER_ROWS; row++) {
            for (int col = 0; col < dim.cols; col++) {
                if (!invaders[row][col]) continue;

                int inv_x = invader_px(col);
                int inv_y = invader_py(row);

                if (bullets[i].x < inv_x + INVADER_WIDTH &&
                    bullets[i].x + BULLET_WIDTH > inv_x &&
                    bullets[i].y < inv_y + INVADER_HEIGHT &&
                    bullets[i].y + BULLET_HEIGHT > inv_y) {
                    bullets[i].active = false;
                    invaders[row][col] = false;
                    score += score_for_row(row);
                    return true;
                }
            }
        }
    }
    return false;
}

bool Game::step_invaders() {
    uint32_t adjusted_delay =
        (speed_bonus >= invader_move_delay) ? 1 : invader_move_delay - speed_bonus;

    if (++invader_move_counter < adjusted_delay) return false;
    invader_move_counter = 0;

    int leftmost = dim.cols;
    int rightmost = -1;
    for (int col = 0; col < dim.cols; col++) {
        for (int row = 0; row < INVADER_ROWS; row++) {
            if (invaders[row][col]) {
                if (col < leftmost) leftmost = col;
                if (col > rightmost) rightmost = col;
            }
        }
    }

    bool hit_edge = false;
    if (invader_direction > 0) {
        int right_edge = invaders_x + rightmost * (INVADER_WIDTH + INVADER_GAP_X) + INVADER_WIDTH;
        if (right_edge >= dim.screen_w - 5) hit_edge = true;
    } else {
        int left_edge = invaders_x + leftmost * (INVADER_WIDTH + INVADER_GAP_X);
        if (left_edge <= 5) hit_edge = true;
    }

    if (hit_edge) {
        invaders_y += dim.drop_amount;
        invader_direction = -invader_direction;
    } else {
        invaders_x += invader_direction * dim.move_amount;
    }

    invader_animation_frame = !invader_animation_frame;
    return true;
}

void Game::fire_enemy_bullet(int shooter_rand) {
    int active_count = active_invader_count();
    if (active_count == 0) return;

    int shooter = shooter_rand % active_count;
    int count = 0;
    for (int row = 0; row < INVADER_ROWS; row++) {
        for (int col = 0; col < dim.cols; col++) {
            if (!invaders[row][col]) continue;
            if (count == shooter) {
                for (int i = 0; i < MAX_ENEMY_BULLETS; i++) {
                    if (!enemy_bullets[i].active) {
                        enemy_bullets[i].x = invader_px(col) + INVADER_WIDTH / 2;
                        enemy_bullets[i].y = invader_py(row) + INVADER_HEIGHT;
                        enemy_bullets[i].active = true;
                        return;
                    }
                }
                return;
            }
            count++;
        }
    }
}

void Game::update_invaders(int shooter_rand) {
    step_invaders();

    if (!easy_mode) {
        if (++enemy_fire_counter >= ENEMY_FIRE_PERIOD) {
            enemy_fire_counter = 0;
            fire_enemy_bullet(shooter_rand);
        }
    }
}

void Game::update_enemy_bullets() {
    for (int i = 0; i < MAX_ENEMY_BULLETS; i++) {
        if (!enemy_bullets[i].active) continue;

        enemy_bullets[i].y += ENEMY_BULLET_SPEED;

        if (enemy_bullets[i].y > dim.screen_h) {
            enemy_bullets[i].active = false;
            continue;
        }

        if (enemy_bullets[i].x >= player_x &&
            enemy_bullets[i].x <= player_x + dim.player_w &&
            enemy_bullets[i].y >= dim.player_y &&
            enemy_bullets[i].y <= dim.player_y + dim.player_h) {
            enemy_bullets[i].active = false;
            lives--;
            if (lives <= 0) state = State::GameOver;
        }
    }
}

bool Game::all_invaders_cleared() const {
    for (int row = 0; row < INVADER_ROWS; row++)
        for (int col = 0; col < dim.cols; col++)
            if (invaders[row][col]) return false;
    return true;
}

bool Game::invaders_reached_player() const {
    for (int row = 0; row < INVADER_ROWS; row++) {
        for (int col = 0; col < dim.cols; col++) {
            if (invaders[row][col]) {
                if (invader_py(row) + INVADER_HEIGHT >= dim.player_y) return true;
            }
        }
    }
    return false;
}

void Game::check_wave_complete() {
    if (all_invaders_cleared()) {
        wave++;
        speed_bonus = (wave - 1) * 3;
        if (speed_bonus > 15) speed_bonus = 15;
        for (int i = 0; i < MAX_ENEMY_BULLETS; i++) enemy_bullets[i].active = false;
        state = State::WaveComplete;
        wave_complete_timer = WAVE_COMPLETE_FRAMES;
        return;
    }

    if (invaders_reached_player()) {
        state = State::GameOver;
    }
}

void Game::tick(int shooter_rand) {
    update_bullets();
    update_enemy_bullets();
    update_invaders(shooter_rand);
    check_collisions();
    check_wave_complete();
}

bool Game::advance_wave_timer() {
    if (wave_complete_timer > 0) wave_complete_timer--;
    if (wave_complete_timer == 0) {
        state = State::Playing;
        init_invaders();
        return true;
    }
    return false;
}

void Game::move_player(int delta) {
    int move_speed = dim.screen_w / 48;
    if (move_speed < 1) move_speed = 1;
    if (delta > 0) {
        player_x += move_speed;
        if (player_x > dim.screen_w - dim.player_w) player_x = dim.screen_w - dim.player_w;
    } else if (delta < 0) {
        player_x -= move_speed;
        if (player_x < 0) player_x = 0;
    }
}

}  // namespace spaceinv_game

/* ---- SpaceInvadersView ---------------------------------------------------- */

namespace {
/* High score survives the view being popped and re-pushed within a session,
 * standing in for the firmware's app_settings persistence (there is no host
 * equivalent wired in for games yet). */
uint32_t g_high_score = 0;
}  // namespace

using namespace spaceinv_game;

SpaceInvadersView::SpaceInvadersView() {
    add_children({&dummy_, &button_difficulty_});

    game_.easy_mode = false;
    button_difficulty_.set_text(game_.easy_mode ? "Mode: EASY" : "Mode: HARD");
    button_difficulty_.on_select = [this](ui::Button&) {
        game_.easy_mode = !game_.easy_mode;
        button_difficulty_.set_text(game_.easy_mode ? "Mode: EASY" : "Mode: HARD");
        set_dirty();
    };
}

void SpaceInvadersView::focus() {
    dummy_.focus();
}

void SpaceInvadersView::ensure_dimensions() {
    if (dims_ready_) return;
    const auto r = screen_rect();
    const int w = r.width() > 0 ? r.width() : ui::screen_width;
    const int h = r.height() > 0 ? r.height()
                                 : (ui::screen_height - ui::SystemStatusView::status_height);
    game_.set_dimensions(w, h);
    game_.init_invaders();

    /* Recentre the difficulty button in the (view-local) game area. */
    const int bx = (w - 100) / 2;
    const int by = h - 45;
    button_difficulty_.set_parent_rect({bx, by, 100, 20});
    dims_ready_ = true;
}

void SpaceInvadersView::on_show() {
    ui::View::on_show();
    ensure_dimensions();
    game_.state = State::Menu;
    blink_state_ = true;
    blink_counter_ = 0;
    button_difficulty_.hidden(false);
    set_dirty();
}

void SpaceInvadersView::start_game() {
    game_.reset_game();
    game_.state = State::Playing;
    button_difficulty_.hidden(true);
    dummy_.focus();
    set_dirty();
}

void SpaceInvadersView::back_to_menu() {
    game_.state = State::Menu;
    blink_state_ = true;
    blink_counter_ = 0;
    button_difficulty_.hidden(false);
    dummy_.focus();
    set_dirty();
}

void SpaceInvadersView::on_frame_sync() {
    ensure_dimensions();

    switch (game_.state) {
        case State::Playing:
            game_.tick(std::rand());
            if (game_.state == State::GameOver && game_.score > g_high_score)
                g_high_score = game_.score;
            set_dirty();
            break;

        case State::WaveComplete:
            game_.advance_wave_timer();
            set_dirty();
            break;

        case State::Menu:
        case State::GameOver:
            if (++blink_counter_ >= 30) {
                blink_counter_ = 0;
                blink_state_ = !blink_state_;
                set_dirty();
            }
            break;
    }

    ui::View::on_frame_sync();
}

bool SpaceInvadersView::on_key(const ui::KeyEvent key) {
    if (key != ui::KeyEvent::Select) return false;

    switch (game_.state) {
        case State::Menu:
            start_game();
            return true;
        case State::Playing:
            game_.fire_bullet();
            set_dirty();
            return true;
        case State::GameOver:
            back_to_menu();
            return true;
        case State::WaveComplete:
            return true;
    }
    return false;
}

bool SpaceInvadersView::on_encoder(const ui::EncoderEvent delta) {
    if (game_.state == State::Playing) {
        game_.move_player(delta);
        set_dirty();
    }
    return true;
}

/* ---- rendering ------------------------------------------------------------ */

void SpaceInvadersView::paint(ui::Painter& painter) {
    ensure_dimensions();
    const ui::Point o = screen_pos();

    switch (game_.state) {
        case State::Menu:          paint_menu(painter, o); break;
        case State::Playing:       paint_playing(painter, o); break;
        case State::WaveComplete:  paint_wave_complete(painter, o); break;
        case State::GameOver:      paint_game_over(painter, o); break;
    }
}

void SpaceInvadersView::paint_menu(ui::Painter& painter, ui::Point o) {
    const int W = game_.dim.screen_w;
    const int H = game_.dim.screen_h;
    painter.fill_rectangle({o.x(), o.y(), W, H}, ui::Color::black());

    const auto* green = ui::Theme::getInstance()->fg_green;
    const auto* yellow = ui::Theme::getInstance()->fg_yellow;
    const auto* cyan = ui::Theme::getInstance()->fg_cyan;
    const auto* red = ui::Theme::getInstance()->fg_red;
    const auto* white = ui::Theme::getInstance()->fg_light;

    auto cx = [&](int chars) { return o.x() + (W / 2) - (chars * 8 / 2); };

    painter.draw_string({cx(14), o.y() + 20}, *green, "SPACE INVADERS");
    painter.draw_string({cx(22), o.y() + 40}, *yellow, "======================");

    int box_x = cx(22);
    painter.draw_rectangle({box_x - 5, o.y() + 70, 22 * 8 + 10, 70}, ui::Color::white());
    painter.draw_string({box_x, o.y() + 80}, *cyan, " ENCODER: MOVE SHIP");
    painter.draw_string({box_x, o.y() + 98}, *cyan, " SELECT: FIRE");
    painter.draw_string({box_x, o.y() + 116}, *cyan, " DEFEND EARTH!");

    painter.draw_string({cx(16), o.y() + 155}, *red, "TOP ROW = 30 PTS");
    painter.draw_string({cx(16), o.y() + 170}, *yellow, "MID ROW = 20 PTS");
    painter.draw_string({cx(16), o.y() + 185}, *green, "BOT ROW = 10 PTS");

    std::string hs = "HIGH SCORE: " + std::to_string(g_high_score);
    painter.draw_string({cx(static_cast<int>(hs.size())), o.y() + 210}, *white, hs);

    if (blink_state_)
        painter.draw_string({cx(12), o.y() + 235}, *red, "PRESS SELECT");
}

void SpaceInvadersView::draw_invader(ui::Painter& painter, ui::Point o, int row, int col) {
    int x = o.x() + game_.invader_px(col);
    int y = o.y() + game_.invader_py(row);

    ui::Color color = (row == 0)             ? ui::Color::red()
                      : (row == 1 || row == 2) ? ui::Color::yellow()
                                               : ui::Color::green();

    const bool f = game_.invader_animation_frame;
    if (row == 0) {
        painter.draw_hline({x + 8, y + 2}, 4, color);
        painter.draw_hline({x + 6, y + 3}, 8, color);
        painter.draw_hline({x + 4, y + 4}, 12, color);
        painter.draw_hline({x + 2, y + 5}, 16, color);
        painter.draw_hline({x + 2, y + 6}, 16, color);
        painter.draw_hline({x, y + 7}, 20, color);
        painter.draw_hline({x, y + 8}, 20, color);
        if (f) {
            painter.draw_hline({x + 2, y + 9}, 2, color);
            painter.draw_hline({x + 6, y + 9}, 8, color);
            painter.draw_hline({x + 16, y + 9}, 2, color);
            painter.draw_hline({x + 4, y + 10}, 2, color);
            painter.draw_hline({x + 14, y + 10}, 2, color);
        } else {
            painter.draw_hline({x + 4, y + 9}, 2, color);
            painter.draw_hline({x + 8, y + 9}, 4, color);
            painter.draw_hline({x + 14, y + 9}, 2, color);
            painter.draw_hline({x + 2, y + 10}, 2, color);
            painter.draw_hline({x + 16, y + 10}, 2, color);
        }
    } else if (row == 1 || row == 2) {
        painter.draw_hline({x + 4, y + 2}, 2, color);
        painter.draw_hline({x + 14, y + 2}, 2, color);
        painter.draw_hline({x + 2, y + 3}, 2, color);
        painter.draw_hline({x + 6, y + 3}, 8, color);
        painter.draw_hline({x + 16, y + 3}, 2, color);
        painter.draw_hline({x + 2, y + 4}, 16, color);
        painter.draw_hline({x, y + 5}, 6, color);
        painter.draw_hline({x + 8, y + 5}, 4, color);
        painter.draw_hline({x + 14, y + 5}, 6, color);
        painter.draw_hline({x, y + 6}, 20, color);
        painter.draw_hline({x + 2, y + 7}, 16, color);
        painter.draw_hline({x + 4, y + 8}, 2, color);
        painter.draw_hline({x + 14, y + 8}, 2, color);
        if (f) {
            painter.draw_hline({x + 2, y + 9}, 4, color);
            painter.draw_hline({x + 14, y + 9}, 4, color);
        } else {
            painter.draw_hline({x, y + 9}, 2, color);
            painter.draw_hline({x + 6, y + 9}, 2, color);
            painter.draw_hline({x + 12, y + 9}, 2, color);
            painter.draw_hline({x + 18, y + 9}, 2, color);
        }
    } else {
        painter.draw_hline({x + 6, y + 2}, 8, color);
        painter.draw_hline({x + 4, y + 3}, 12, color);
        painter.draw_hline({x + 2, y + 4}, 16, color);
        painter.draw_hline({x, y + 5}, 20, color);
        painter.draw_hline({x, y + 6}, 20, color);
        painter.draw_hline({x + 4, y + 7}, 4, color);
        painter.draw_hline({x + 12, y + 7}, 4, color);
        painter.draw_hline({x + 2, y + 8}, 4, color);
        painter.draw_hline({x + 8, y + 8}, 4, color);
        painter.draw_hline({x + 14, y + 8}, 4, color);
        if (f) {
            painter.draw_hline({x + 4, y + 9}, 2, color);
            painter.draw_hline({x + 14, y + 9}, 2, color);
        } else {
            painter.draw_hline({x + 2, y + 9}, 2, color);
            painter.draw_hline({x + 16, y + 9}, 2, color);
        }
    }
}

void SpaceInvadersView::draw_player(ui::Painter& painter, ui::Point o) {
    const int px = o.x() + game_.player_x;
    const int py = o.y() + game_.dim.player_y;
    const ui::Color green = ui::Color::green();

    int ux = game_.dim.player_w / 26;
    int uy = game_.dim.player_h / 16;
    if (ux < 1) ux = 1;
    if (uy < 1) uy = 1;

    painter.fill_rectangle({px + 12 * ux, py, 2 * ux, 2 * uy}, green);
    painter.fill_rectangle({px + 11 * ux, py + 2 * uy, 4 * ux, 2 * uy}, green);
    painter.fill_rectangle({px + 10 * ux, py + 4 * uy, 6 * ux, 2 * uy}, green);
    painter.fill_rectangle({px + 2 * ux, py + 6 * uy, 22 * ux, 10 * uy}, green);
    painter.fill_rectangle({px + ux, py + 8 * uy, 24 * ux, 8 * uy}, green);
    painter.fill_rectangle({px, py + 10 * uy, 26 * ux, 6 * uy}, green);
}

void SpaceInvadersView::paint_playing(ui::Painter& painter, ui::Point o) {
    const int W = game_.dim.screen_w;
    const int H = game_.dim.screen_h;
    painter.fill_rectangle({o.x(), o.y(), W, H}, ui::Color::black());

    const auto* green = ui::Theme::getInstance()->fg_green;
    painter.draw_string({o.x() + 5, o.y() + 8}, *green, "Score: " + std::to_string(game_.score));
    painter.draw_string({o.x() + 5, o.y() + 26}, *green, "Lives: " + std::to_string(game_.lives));
    std::string wv = "Wave: " + std::to_string(game_.wave);
    painter.draw_string({o.x() + W - static_cast<int>(wv.size()) * 8 - 5, o.y() + 8}, *green, wv);
    painter.draw_hline({o.x(), o.y() + INFO_BAR_HEIGHT - 1}, W, ui::Color::white());

    for (int row = 0; row < INVADER_ROWS; row++)
        for (int col = 0; col < game_.dim.cols; col++)
            if (game_.invaders[row][col]) draw_invader(painter, o, row, col);

    for (int i = 0; i < MAX_BULLETS; i++)
        if (game_.bullets[i].active)
            painter.fill_rectangle({o.x() + game_.bullets[i].x, o.y() + game_.bullets[i].y,
                                    BULLET_WIDTH, BULLET_HEIGHT}, ui::Color::white());

    for (int i = 0; i < MAX_ENEMY_BULLETS; i++)
        if (game_.enemy_bullets[i].active)
            painter.fill_rectangle({o.x() + game_.enemy_bullets[i].x, o.y() + game_.enemy_bullets[i].y,
                                    BULLET_WIDTH, BULLET_HEIGHT}, ui::Color::red());

    draw_player(painter, o);
}

void SpaceInvadersView::paint_wave_complete(ui::Painter& painter, ui::Point o) {
    const int W = game_.dim.screen_w;
    const int H = game_.dim.screen_h;
    painter.fill_rectangle({o.x(), o.y(), W, H}, ui::Color::black());
    const auto* green = ui::Theme::getInstance()->fg_green;
    std::string t = "WAVE " + std::to_string(game_.wave);
    painter.draw_string({o.x() + (W / 2) - static_cast<int>(t.size()) * 4, o.y() + H / 2}, *green, t);
}

void SpaceInvadersView::paint_game_over(ui::Painter& painter, ui::Point o) {
    const int W = game_.dim.screen_w;
    const int H = game_.dim.screen_h;
    painter.fill_rectangle({o.x(), o.y(), W, H}, ui::Color::black());

    const auto* red = ui::Theme::getInstance()->fg_red;
    const auto* yellow = ui::Theme::getInstance()->fg_yellow;
    const auto* cyan = ui::Theme::getInstance()->fg_cyan;
    const auto* white = ui::Theme::getInstance()->fg_light;

    auto cx = [&](int chars) { return o.x() + (W / 2) - (chars * 8 / 2); };

    painter.draw_string({cx(9), o.y() + 40}, *red, "GAME OVER");

    std::string s = "SCORE: " + std::to_string(game_.score);
    painter.draw_string({cx(static_cast<int>(s.size())), o.y() + 90}, *cyan, s);
    std::string w = "WAVE: " + std::to_string(game_.wave);
    painter.draw_string({cx(static_cast<int>(w.size())), o.y() + 110}, *cyan, w);

    if (game_.score >= g_high_score && game_.score > 0) {
        painter.draw_string({cx(15), o.y() + 160}, *yellow, "NEW HIGH SCORE!");
        std::string hv = std::to_string(g_high_score);
        painter.draw_string({cx(static_cast<int>(hv.size())), o.y() + 180}, *yellow, hv);
    } else {
        painter.draw_string({cx(11), o.y() + 160}, *white, "HIGH SCORE:");
        std::string hv = std::to_string(g_high_score);
        painter.draw_string({cx(static_cast<int>(hv.size())), o.y() + 180}, *white, hv);
    }

    if (blink_state_)
        painter.draw_string({cx(12), o.y() + 230}, *red, "PRESS SELECT");
}

}  // namespace app

#include "app_registry.hpp"
#include "bitmaps.hpp"

namespace {
const app::Registrar reg_spaceinv{{"spaceinv", "Space Invaders",
                                   app::Category::Games, ui::Color::magenta(),
                                   &ui::bitmap_icon_games,
                                   [] { return std::make_unique<app::SpaceInvadersView>(); },
                                   false}};
}  // namespace
