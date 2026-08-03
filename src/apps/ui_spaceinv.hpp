/*
 * mayhem-b200 — Space Invaders game.
 *
 * Host port of PortaPack Mayhem's external/spaceinv app (Made by RocketGod,
 * https://betaskynet.com). The firmware ran the whole game from a 60 Hz Ticker
 * callback that drew directly to the ILI9341 with a file-scope Painter, updating
 * the screen incrementally (clearing an old bullet rectangle, drawing the new
 * one). On the host there is no second-core message queue: the game steps in
 * on_frame_sync() and the whole board is repainted from state in paint(). To
 * keep the rules honest and unit-testable, all of the game logic (movement,
 * collision, scoring, wave/loss detection) lives in the render-free
 * `spaceinv_game` namespace and is exercised in tests/test_spaceinv.cpp against
 * the exact bounds, delays and point values from upstream.
 *
 * Copyright (C) 2024 RocketGod (original game)
 * Copyright (C) 2026 mayhem-b200 contributors (host implementation)
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef __MB200_UI_SPACEINV_H__
#define __MB200_UI_SPACEINV_H__

#include "ui.hpp"
#include "ui_widget.hpp"

#include <cstdint>
#include <string>

namespace app {

/* ---- pure, render-free game logic (tested in test_spaceinv.cpp) ----------- */
namespace spaceinv_game {

/* Rules constants, verbatim from upstream ui_spaceinv.cpp. */
constexpr int MAX_BULLETS = 3;
constexpr int MAX_ENEMY_BULLETS = 3;
constexpr int INVADER_ROWS = 5;
constexpr int MAX_INVADER_COLS = 11;  /* invaders[5][11] in upstream */

constexpr int BULLET_WIDTH = 3;
constexpr int BULLET_HEIGHT = 10;
constexpr int BULLET_SPEED = 8;
constexpr int ENEMY_BULLET_SPEED = 3;

constexpr int INVADER_WIDTH = 16 + 4;   /* 20 */
constexpr int INVADER_HEIGHT = 16;
constexpr int INVADER_GAP_X = 10;
constexpr int INVADER_GAP_Y = 8;
constexpr int INFO_BAR_HEIGHT = 50;

constexpr uint32_t BASE_MOVE_DELAY = 30;      /* frames between invader moves   */
constexpr uint32_t ENEMY_FIRE_PERIOD = 120;   /* frames between enemy shots (hard) */
constexpr uint32_t WAVE_COMPLETE_FRAMES = 60; /* 1 s banner between waves        */

enum class State : uint8_t { Menu, Playing, GameOver, WaveComplete };

struct Bullet {
    int x = 0;
    int y = 0;
    bool active = false;
};

/* Screen-derived dimensions. init_dimensions() in upstream scales the player and
 * invader-formation geometry to the display; make() reproduces that maths so the
 * host can pass the 240x304 view area (or a test can pass anything). */
struct Dimensions {
    int screen_w = 240;
    int screen_h = 320;
    int player_w = 26;
    int player_h = 16;
    int player_y = 280;
    int cols = 6;
    int move_amount = 8;
    int drop_amount = 15;
    int start_x = 40;  /* left edge of the formation at wave start */
    int start_y = 60;  /* top  edge of the formation at wave start */

    static Dimensions make(int w, int h);
};

/* Point value for an invader row: top=30, middle two=20, bottom two=10. */
int score_for_row(int row);

struct Game {
    Dimensions dim{};
    State state = State::Menu;

    int player_x = 107;
    uint32_t score = 0;
    int lives = 3;
    uint32_t wave = 1;
    uint32_t speed_bonus = 0;
    uint32_t wave_complete_timer = 0;

    Bullet bullets[MAX_BULLETS]{};
    Bullet enemy_bullets[MAX_ENEMY_BULLETS]{};
    uint32_t enemy_fire_counter = 0;

    bool invaders[INVADER_ROWS][MAX_INVADER_COLS]{};
    int invaders_x = 40;
    int invaders_y = 60;
    int invader_direction = 1;
    uint32_t invader_move_counter = 0;
    uint32_t invader_move_delay = BASE_MOVE_DELAY;
    bool invader_animation_frame = false;

    bool easy_mode = false;

    /* Pixel position of a cell's top-left corner. */
    int invader_px(int col) const { return invaders_x + col * (INVADER_WIDTH + INVADER_GAP_X); }
    int invader_py(int row) const { return invaders_y + row * (INVADER_HEIGHT + INVADER_GAP_Y); }
    int active_invader_count() const;

    void set_dimensions(int w, int h);
    void reset_game();     /* fresh game: score/lives/wave reset, invaders placed */
    void init_invaders();  /* repopulate the grid and recentre the formation      */

    /* Player cannon fire and travel. */
    void fire_bullet();
    void update_bullets();
    /* One collision pass: at most one invader is destroyed per call (as upstream).
     * Returns true when a hit occurred. */
    bool check_collisions();

    /* Throttled invader march; returns true on the frames they actually move. */
    bool step_invaders();
    /* One playing-frame invader update: march + (hard-mode) enemy fire. */
    void update_invaders(int shooter_rand);
    void fire_enemy_bullet(int shooter_rand);
    void update_enemy_bullets();  /* travel + player hit; may set GameOver */

    bool all_invaders_cleared() const;
    bool invaders_reached_player() const;
    /* Win → WaveComplete (+ wave/speed bump); loss (reached player) → GameOver. */
    void check_wave_complete();

    /* Whole playing-state frame, in upstream order. */
    void tick(int shooter_rand);
    /* WaveComplete banner countdown; returns true the frame play resumes. */
    bool advance_wave_timer();

    void move_player(int delta);  /* encoder step */
};

}  // namespace spaceinv_game

/* ---- view ----------------------------------------------------------------- */

class SpaceInvadersView : public ui::View {
   public:
    SpaceInvadersView();

    std::string title() const override { return "Space Invaders"; }

    void focus() override;
    void on_show() override;
    void on_frame_sync() override;
    void paint(ui::Painter& painter) override;
    bool on_key(const ui::KeyEvent key) override;
    bool on_encoder(const ui::EncoderEvent delta) override;

   private:
    void ensure_dimensions();
    void start_game();
    void back_to_menu();

    /* rendering helpers (offset everything by the view origin) */
    void paint_menu(ui::Painter& painter, ui::Point o);
    void paint_playing(ui::Painter& painter, ui::Point o);
    void paint_wave_complete(ui::Painter& painter, ui::Point o);
    void paint_game_over(ui::Painter& painter, ui::Point o);
    void draw_player(ui::Painter& painter, ui::Point o);
    void draw_invader(ui::Painter& painter, ui::Point o, int row, int col);

    spaceinv_game::Game game_{};
    bool dims_ready_ = false;
    bool blink_state_ = true;
    uint32_t blink_counter_ = 0;

    ui::Button button_difficulty_{{70, 259, 100, 20}, "Mode: HARD"};
    /* Off-screen focus holder: no on_select, so Select falls through to the view
     * (the dispatcher only forwards an unconsumed Select to the top view). */
    ui::Button dummy_{{240, 0, 0, 0}, ""};
};

}  // namespace app

#endif /*__MB200_UI_SPACEINV_H__*/
