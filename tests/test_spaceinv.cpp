/*
 * mayhem-b200 — Space Invaders game-logic tests.
 *
 * All assertions come from upstream ui_spaceinv.cpp: the AABB bullet/invader
 * overlap test, the 30/20/10 row scoring, the wave-clear advance and the two
 * loss conditions (an invader reaching the player, or lives hitting zero). The
 * board geometry is the 240x304 host view area; expected coordinates are worked
 * out by hand from init_dimensions()'s formulae.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "test_main.hpp"

#include "ui_spaceinv.hpp"

using namespace app::spaceinv_game;
using namespace mb200test;

namespace {

/* A fresh game on the host view geometry with a full 5x6 invader grid. */
Game make_game() {
    Game g;
    g.set_dimensions(240, 304);
    g.reset_game();
    g.state = State::Playing;
    return g;
}

}  // namespace

/* ---- geometry sanity (drives every coordinate below) --------------------- */

TEST(si_dimensions_240x304) {
    const Dimensions d = Dimensions::make(240, 304);
    CHECK_EQ(d.player_w, 26);
    CHECK_EQ(d.player_y, 264);
    CHECK_EQ(d.cols, 6);
    CHECK_EQ(d.start_x, 35);  /* (240 - (6*20 + 5*10)) / 2 = 35 */
    CHECK_EQ(d.start_y, 60);  /* INFO_BAR_HEIGHT + 10          */
    CHECK_EQ(d.move_amount, 8);
    CHECK_EQ(d.drop_amount, 14);
}

/* ---- bullet / invader collision ------------------------------------------ */

TEST(si_bullet_hits_top_row_scores_30) {
    Game g = make_game();
    /* Invader (0,0) sits at (35,60); a bullet just inside it must register. */
    g.bullets[0] = Bullet{40, 65, true};

    const bool hit = g.check_collisions();

    CHECK(hit);
    CHECK(!g.invaders[0][0]);        /* invader removed        */
    CHECK(!g.bullets[0].active);     /* bullet consumed        */
    CHECK_EQ(g.score, 30u);          /* top row = 30 points    */
}

TEST(si_row_scoring_20_and_10) {
    /* Middle rows (1,2) score 20; bottom rows (3,4) score 10. */
    Game g = make_game();
    g.bullets[0] = Bullet{40, g.invader_py(1) + 2, true};  /* over invader (1,0) */
    CHECK(g.check_collisions());
    CHECK_EQ(g.score, 20u);

    Game g2 = make_game();
    g2.bullets[0] = Bullet{40, g2.invader_py(3) + 2, true};  /* over invader (3,0) */
    CHECK(g2.check_collisions());
    CHECK_EQ(g2.score, 10u);
}

TEST(si_bullet_miss_changes_nothing) {
    Game g = make_game();
    /* Below every invader (rows end well above y=250) and no player here. */
    g.bullets[0] = Bullet{40, 250, true};

    const bool hit = g.check_collisions();

    CHECK(!hit);
    CHECK(g.bullets[0].active);      /* bullet survives        */
    CHECK_EQ(g.score, 0u);
    CHECK(g.invaders[0][0]);         /* nothing destroyed      */
}

TEST(si_one_invader_per_collision_pass) {
    /* Upstream returns after the first hit, so a single pass clears one cell. */
    Game g = make_game();
    g.bullets[0] = Bullet{40, 65, true};
    g.bullets[1] = Bullet{g.invader_px(1) + 5, 65, true};  /* over (0,1) */

    CHECK(g.check_collisions());
    int destroyed = 0;
    for (int r = 0; r < INVADER_ROWS; r++)
        for (int c = 0; c < g.dim.cols; c++)
            if (!g.invaders[r][c]) destroyed++;
    CHECK_EQ(destroyed, 1);
}

/* ---- firing / bullet travel ---------------------------------------------- */

TEST(si_fire_bullet_caps_at_three) {
    Game g = make_game();
    g.player_x = 100;
    g.fire_bullet();
    CHECK(g.bullets[0].active);
    CHECK_EQ(g.bullets[0].x, 112);                 /* 100 + 26/2 - 3/2 */
    CHECK_EQ(g.bullets[0].y, g.dim.player_y - BULLET_HEIGHT);

    g.fire_bullet();
    g.fire_bullet();
    g.fire_bullet();  /* fourth request: no free slot */
    int active = 0;
    for (int i = 0; i < MAX_BULLETS; i++)
        if (g.bullets[i].active) active++;
    CHECK_EQ(active, MAX_BULLETS);
}

TEST(si_bullet_deactivates_above_info_bar) {
    Game g = make_game();
    g.bullets[0] = Bullet{50, 55, true};  /* just below the 50px info bar */
    g.update_bullets();                    /* 55 - 8 = 47 < 50 -> gone     */
    CHECK(!g.bullets[0].active);
}

/* ---- invader march ------------------------------------------------------- */

TEST(si_invaders_step_right_when_clear) {
    Game g = make_game();
    g.invader_move_counter = BASE_MOVE_DELAY - 1;  /* next call trips the move */
    const int x0 = g.invaders_x;
    CHECK(g.step_invaders());
    CHECK_EQ(g.invaders_x, x0 + g.dim.move_amount);
    CHECK_EQ(g.invader_direction, 1);
}

TEST(si_invaders_drop_and_reverse_at_edge) {
    Game g = make_game();
    g.invaders_x = 70;  /* right edge = 70 + 5*30 + 20 = 240 >= 235 */
    g.invader_direction = 1;
    g.invader_move_counter = BASE_MOVE_DELAY - 1;
    const int y0 = g.invaders_y;
    CHECK(g.step_invaders());
    CHECK_EQ(g.invaders_y, y0 + g.dim.drop_amount);
    CHECK_EQ(g.invader_direction, -1);
}

/* ---- win: all invaders cleared ------------------------------------------- */

TEST(si_wave_clear_advances_wave) {
    Game g = make_game();
    for (int r = 0; r < INVADER_ROWS; r++)
        for (int c = 0; c < g.dim.cols; c++)
            g.invaders[r][c] = false;

    CHECK(g.all_invaders_cleared());
    g.check_wave_complete();

    CHECK(g.state == State::WaveComplete);
    CHECK_EQ(g.wave, 2u);
    CHECK_EQ(g.speed_bonus, 3u);
    CHECK_EQ(g.wave_complete_timer, WAVE_COMPLETE_FRAMES);
}

TEST(si_speed_bonus_caps_at_15) {
    Game g = make_game();
    g.wave = 6;  /* becomes 7 -> (7-1)*3 = 18 -> capped to 15 */
    for (int r = 0; r < INVADER_ROWS; r++)
        for (int c = 0; c < g.dim.cols; c++)
            g.invaders[r][c] = false;
    g.check_wave_complete();
    CHECK_EQ(g.speed_bonus, 15u);
}

/* ---- lose: invaders reach the player ------------------------------------- */

TEST(si_invaders_reaching_player_is_game_over) {
    Game g = make_game();
    g.invaders_y = g.dim.player_y;  /* row 0 bottom = player_y + 16 >= player_y */

    CHECK(g.invaders_reached_player());
    g.check_wave_complete();
    CHECK(g.state == State::GameOver);
}

TEST(si_invaders_not_yet_at_player_keeps_playing) {
    Game g = make_game();  /* fresh formation sits high on the board */
    CHECK(!g.invaders_reached_player());
    g.check_wave_complete();
    CHECK(g.state == State::Playing);
}

/* ---- lose: lives exhausted by enemy fire --------------------------------- */

TEST(si_enemy_bullet_hits_player_costs_a_life) {
    Game g = make_game();
    g.player_x = 100;              /* player spans 100..126 */
    g.lives = 3;
    g.enemy_bullets[0] = Bullet{110, g.dim.player_y - 1, true};

    g.update_enemy_bullets();      /* y -> player_y+2, inside the player box */

    CHECK_EQ(g.lives, 2);
    CHECK(!g.enemy_bullets[0].active);
    CHECK(g.state == State::Playing);
}

TEST(si_last_life_lost_is_game_over) {
    Game g = make_game();
    g.player_x = 100;
    g.lives = 1;
    g.enemy_bullets[0] = Bullet{110, g.dim.player_y, true};

    g.update_enemy_bullets();

    CHECK_EQ(g.lives, 0);
    CHECK(g.state == State::GameOver);
}

TEST(si_enemy_bullet_leaves_bottom) {
    Game g = make_game();
    g.enemy_bullets[0] = Bullet{110, g.dim.screen_h, true};
    g.update_enemy_bullets();      /* y -> screen_h + 3 > screen_h */
    CHECK(!g.enemy_bullets[0].active);
}

/* ---- easy mode disables enemy fire --------------------------------------- */

TEST(si_easy_mode_never_fires) {
    Game g = make_game();
    g.easy_mode = true;
    g.enemy_fire_counter = ENEMY_FIRE_PERIOD;  /* would fire in hard mode */
    g.update_invaders(0);
    for (int i = 0; i < MAX_ENEMY_BULLETS; i++)
        CHECK(!g.enemy_bullets[i].active);
}

TEST(si_hard_mode_fires_on_period) {
    Game g = make_game();
    g.easy_mode = false;
    g.enemy_fire_counter = ENEMY_FIRE_PERIOD - 1;  /* trips this frame */
    g.update_invaders(0);                           /* shooter index 0 */
    bool any = false;
    for (int i = 0; i < MAX_ENEMY_BULLETS; i++)
        any = any || g.enemy_bullets[i].active;
    CHECK(any);
}
