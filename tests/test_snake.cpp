/*
 * mayhem-b200 — Snake grid-logic tests.
 *
 * The SnakeEngine is exercised headless: positions are set up directly and the
 * outcome of a step (or a raw collision check) is asserted against the rules
 * ported from upstream ui_snake.cpp:
 *   - move: head advances by (dx,dy), the body follows, length is unchanged;
 *   - growth: the head reaching the food grows the snake by one and scores 10;
 *   - wall collision: the head leaving the grid ends the game;
 *   - self collision: the head landing on a body segment ends the game;
 *   - turn rule: a direction change is accepted only onto the perpendicular
 *     axis, so an instant 180 is impossible.
 *
 * The RNG is pinned to a fixed cell so food placement is deterministic where a
 * test cares; where it does not, food is written directly before stepping.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "test_main.hpp"

#include "ui_snake.hpp"

using namespace app::snake_logic;
using namespace mb200test;

namespace {

/* A fresh engine on a generous grid with a deterministic food source. */
SnakeEngine make_engine(int w = 23, int h = 27) {
    SnakeEngine e;
    /* Pin food far in a corner so it never sits under our test snakes. */
    e.rng = [](int max) { return max > 0 ? max - 1 : 0; };
    e.reset(w, h);
    return e;
}

}  // namespace

/* ---- reset / initial state ----------------------------------------------- */

TEST(snake_reset_centers_head) {
    SnakeEngine e = make_engine(23, 27);
    CHECK_EQ(e.snake_length, 1);
    CHECK_EQ(e.snake_x[0], 11);  /* 23/2 */
    CHECK_EQ(e.snake_y[0], 13);  /* 27/2 */
    CHECK_EQ(e.snake_dx, 1);
    CHECK_EQ(e.snake_dy, 0);
    CHECK_EQ(e.score, 0);
}

/* ---- plain movement (no food) -------------------------------------------- */

TEST(snake_step_moves_head_no_growth) {
    SnakeEngine e = make_engine();
    /* Put the food out of the way of this step. */
    e.food_x = 0;
    e.food_y = 0;
    e.snake_x[0] = 5;
    e.snake_y[0] = 5;
    e.snake_dx = 1;
    e.snake_dy = 0;
    e.snake_length = 1;

    const SnakeEngine::StepResult r = e.step();
    CHECK(!r.ate_food);
    CHECK(!r.collided);
    CHECK_EQ(e.snake_length, 1);
    CHECK_EQ(e.snake_x[0], 6);
    CHECK_EQ(e.snake_y[0], 5);
}

TEST(snake_body_follows_head) {
    SnakeEngine e = make_engine();
    e.food_x = 0;
    e.food_y = 0;
    /* Horizontal snake of length 3 heading right: head (5,5), body (4,5)(3,5). */
    e.snake_length = 3;
    e.snake_x[0] = 5; e.snake_y[0] = 5;
    e.snake_x[1] = 4; e.snake_y[1] = 5;
    e.snake_x[2] = 3; e.snake_y[2] = 5;
    e.snake_dx = 1; e.snake_dy = 0;

    const SnakeEngine::StepResult r = e.step();
    CHECK(!r.collided);
    CHECK_EQ(e.snake_length, 3);
    CHECK_EQ(e.snake_x[0], 6);  /* head advanced */
    CHECK_EQ(e.snake_x[1], 5);  /* segment took old head's place */
    CHECK_EQ(e.snake_x[2], 4);
}

/* ---- growth on food ------------------------------------------------------ */

TEST(snake_growth_on_food) {
    SnakeEngine e = make_engine();
    e.snake_length = 1;
    e.snake_x[0] = 5; e.snake_y[0] = 5;
    e.snake_dx = 1; e.snake_dy = 0;
    /* Food is the cell the head is about to enter. */
    e.food_x = 6; e.food_y = 5;

    const SnakeEngine::StepResult r = e.step();
    CHECK(r.ate_food);
    CHECK(!r.collided);
    CHECK_EQ(e.snake_length, 2);
    CHECK_EQ(e.score, 10);
    CHECK_EQ(e.snake_x[0], 6);
    CHECK_EQ(e.snake_y[0], 5);
}

TEST(snake_growth_scores_accumulate) {
    SnakeEngine e = make_engine();
    e.snake_length = 1;
    e.snake_x[0] = 5; e.snake_y[0] = 5;
    e.snake_dx = 1; e.snake_dy = 0;
    e.food_x = 6; e.food_y = 5;
    e.step();  /* eat -> score 10, length 2, respawns food at corner (22,26) */

    /* Head now at (6,5); keep eating by placing food directly ahead. */
    e.food_x = 7; e.food_y = 5;
    e.step();  /* eat -> score 20, length 3 */
    CHECK_EQ(e.score, 20);
    CHECK_EQ(e.snake_length, 3);
}

/* ---- wall collision ------------------------------------------------------ */

TEST(snake_wall_collision_right) {
    SnakeEngine e = make_engine(23, 27);
    e.food_x = 0; e.food_y = 0;
    e.snake_length = 1;
    e.snake_x[0] = e.grid_w - 1;  /* last column */
    e.snake_y[0] = 13;
    e.snake_dx = 1; e.snake_dy = 0;

    const SnakeEngine::StepResult r = e.step();
    CHECK(r.collided);  /* head steps to x == grid_w, off the board */
}

TEST(snake_wall_collision_left) {
    SnakeEngine e = make_engine(23, 27);
    e.food_x = 5; e.food_y = 5;
    e.snake_length = 1;
    e.snake_x[0] = 0;
    e.snake_y[0] = 13;
    e.snake_dx = -1; e.snake_dy = 0;

    const SnakeEngine::StepResult r = e.step();
    CHECK(r.collided);  /* head steps to x == -1 */
}

TEST(snake_wall_collision_top) {
    SnakeEngine e = make_engine(23, 27);
    e.food_x = 5; e.food_y = 5;
    e.snake_length = 1;
    e.snake_x[0] = 11;
    e.snake_y[0] = 0;
    e.snake_dx = 0; e.snake_dy = -1;

    const SnakeEngine::StepResult r = e.step();
    CHECK(r.collided);  /* head steps to y == -1 */
}

TEST(snake_wall_collision_bottom) {
    SnakeEngine e = make_engine(23, 27);
    e.food_x = 5; e.food_y = 5;
    e.snake_length = 1;
    e.snake_x[0] = 11;
    e.snake_y[0] = e.grid_h - 1;
    e.snake_dx = 0; e.snake_dy = 1;

    const SnakeEngine::StepResult r = e.step();
    CHECK(r.collided);  /* head steps to y == grid_h */
}

TEST(snake_no_collision_in_bounds) {
    SnakeEngine e = make_engine();
    e.food_x = 0; e.food_y = 0;
    e.snake_length = 1;
    e.snake_x[0] = 10; e.snake_y[0] = 10;
    e.snake_dx = 1; e.snake_dy = 0;
    CHECK(!e.check_collision());
}

/* ---- self collision (check_collision against a known position) ----------- */

TEST(snake_self_collision_detected) {
    SnakeEngine e = make_engine();
    /* Head coincides with a mid-body segment: this is exactly what
     * check_collision must catch. Body index 2 is placed on the head cell. */
    e.snake_length = 4;
    e.snake_x[0] = 5; e.snake_y[0] = 5;  /* head */
    e.snake_x[1] = 6; e.snake_y[1] = 5;
    e.snake_x[2] = 5; e.snake_y[2] = 5;  /* overlaps head */
    e.snake_x[3] = 6; e.snake_y[3] = 6;
    CHECK(e.check_collision());
}

TEST(snake_no_self_collision_when_distinct) {
    SnakeEngine e = make_engine();
    e.snake_length = 4;
    e.snake_x[0] = 5; e.snake_y[0] = 5;
    e.snake_x[1] = 4; e.snake_y[1] = 5;
    e.snake_x[2] = 3; e.snake_y[2] = 5;
    e.snake_x[3] = 2; e.snake_y[3] = 5;
    CHECK(!e.check_collision());
}

/* Self collision produced by an actual step: a length-3 snake whose neck sits
 * directly ahead. After the body shift the head lands on a non-tail segment. */
TEST(snake_self_collision_via_step) {
    SnakeEngine e = make_engine();
    e.food_x = 0; e.food_y = 0;
    e.snake_length = 3;
    e.snake_x[0] = 5; e.snake_y[0] = 5;  /* head */
    e.snake_x[1] = 6; e.snake_y[1] = 5;  /* neck, in the direction of travel */
    e.snake_x[2] = 7; e.snake_y[2] = 5;
    e.snake_dx = 1; e.snake_dy = 0;      /* move onto the neck's old cell */

    const SnakeEngine::StepResult r = e.step();
    CHECK(r.collided);
}

/* ---- direction / turn rule ----------------------------------------------- */

TEST(snake_cannot_reverse) {
    SnakeEngine e = make_engine();
    e.snake_dx = 1; e.snake_dy = 0;      /* moving right */
    e.set_direction(Dir::Left);          /* reversal rejected (dx != 0) */
    CHECK_EQ(e.snake_dx, 1);
    CHECK_EQ(e.snake_dy, 0);
}

TEST(snake_can_turn_perpendicular) {
    SnakeEngine e = make_engine();
    e.snake_dx = 1; e.snake_dy = 0;      /* moving right */
    e.set_direction(Dir::Up);            /* dy == 0 so accepted */
    CHECK_EQ(e.snake_dx, 0);
    CHECK_EQ(e.snake_dy, -1);

    e.set_direction(Dir::Down);          /* now moving vertically: reject */
    CHECK_EQ(e.snake_dx, 0);
    CHECK_EQ(e.snake_dy, -1);

    e.set_direction(Dir::Right);         /* dx == 0 so accepted */
    CHECK_EQ(e.snake_dx, 1);
    CHECK_EQ(e.snake_dy, 0);
}

/* ---- food never spawns on the snake -------------------------------------- */

TEST(snake_food_avoids_body) {
    SnakeEngine e;
    /* RNG that first proposes the head cell, then a free cell — spawn_food must
     * reject the first and take the second. */
    int calls = 0;
    e.rng = [&calls](int max) -> int {
        (void)max;
        /* Sequence: (x=2,y=2) [on head], then (x=0,y=0) [free]. */
        static const int seq[] = {2, 2, 0, 0};
        return seq[calls++ % 4];
    };
    e.grid_w = 5;
    e.grid_h = 5;
    e.snake_x.assign(25, 0);
    e.snake_y.assign(25, 0);
    e.snake_length = 1;
    e.snake_x[0] = 2;
    e.snake_y[0] = 2;
    e.spawn_food();
    CHECK(!(e.food_x == 2 && e.food_y == 2));
    CHECK_EQ(e.food_x, 0);
    CHECK_EQ(e.food_y, 0);
}
