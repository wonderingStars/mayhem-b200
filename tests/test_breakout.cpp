/*
 * mayhem-b200 — Breakout ball-physics tests.
 *
 * Ball, paddle and brick positions are set up by hand and checked against
 * upstream ui_breakout.cpp's collision rules: walls flip the perpendicular
 * velocity and clamp the ball inside; the paddle flips dy and sets dx from the
 * hit offset (dx = ((hit/width) - 0.5) * 4); a brick hit removes the brick,
 * flips dy, and scores (5 - row) * 10.
 *
 * Geometry uses a brick_width of 21 and 10 columns — the values init_game
 * derives for a 240 px wide screen — so the pixel constants below are exact.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "test_main.hpp"

#include "ui_breakout.hpp"

#include <vector>

using namespace app::breakout_logic;
using namespace mb200test;

namespace {

constexpr int kBW = 21;    /* brick width for a 240 px screen */
constexpr int kCols = 10;  /* brick columns for a 240 px screen */
constexpr int kScreenW = 240;

std::vector<std::vector<bool>> full_wall() {
    return std::vector<std::vector<bool>>(kBrickRows, std::vector<bool>(kCols, true));
}

unsigned count_bricks(const std::vector<std::vector<bool>>& b) {
    unsigned n = 0;
    for (const auto& row : b)
        for (bool cell : row)
            if (cell) ++n;
    return n;
}

}  // namespace

/* ---- brick geometry and scoring ------------------------------------------ */

TEST(bo_brick_geometry) {
    CHECK_EQ(brick_left(0, kBW), 0);
    CHECK_EQ(brick_left(1, kBW), 23);   /* 1 * (21 + 2) */
    CHECK_EQ(brick_left(2, kBW), 46);
    CHECK_EQ(brick_top(0), 55);         /* 50 + 0 + 5 */
    CHECK_EQ(brick_top(1), 67);         /* 50 + 12 + 5 */
    CHECK_EQ(brick_top(4), 103);        /* 50 + 48 + 5 */
}

TEST(bo_brick_score_by_row) {
    CHECK_EQ(brick_score(0), 50);  /* top row worth most */
    CHECK_EQ(brick_score(1), 40);
    CHECK_EQ(brick_score(2), 30);
    CHECK_EQ(brick_score(3), 20);
    CHECK_EQ(brick_score(4), 10);
}

/* ---- wall bounce --------------------------------------------------------- */

TEST(bo_wall_bounce_left) {
    Ball ball{-3.0f, 100.0f, -1.5f, 2.0f};
    const bool hit = wall_bounce(ball, kScreenW);
    CHECK(hit);
    CHECK_NEAR(ball.x, 0.0f, 0.001);
    CHECK_NEAR(ball.dx, 1.5f, 0.001);  /* flipped */
    CHECK_NEAR(ball.dy, 2.0f, 0.001);  /* untouched */
}

TEST(bo_wall_bounce_right) {
    Ball ball{236.0f, 100.0f, 1.5f, 2.0f};  /* > 240 - 8 */
    const bool hit = wall_bounce(ball, kScreenW);
    CHECK(hit);
    CHECK_NEAR(ball.x, 232.0f, 0.001);  /* clamped to screen_w - ball_size */
    CHECK_NEAR(ball.dx, -1.5f, 0.001);
}

TEST(bo_wall_bounce_top) {
    Ball ball{100.0f, 49.0f, 1.5f, -2.0f};  /* above GAME_AREA_TOP (50) */
    const bool hit = wall_bounce(ball, kScreenW);
    CHECK(hit);
    CHECK_NEAR(ball.y, 50.0f, 0.001);
    CHECK_NEAR(ball.dy, 2.0f, 0.001);  /* flipped downward */
}

TEST(bo_wall_bounce_none_in_middle) {
    Ball ball{100.0f, 100.0f, 1.5f, 2.0f};
    const bool hit = wall_bounce(ball, kScreenW);
    CHECK(!hit);
    CHECK_NEAR(ball.x, 100.0f, 0.001);
    CHECK_NEAR(ball.y, 100.0f, 0.001);
    CHECK_NEAR(ball.dx, 1.5f, 0.001);
    CHECK_NEAR(ball.dy, 2.0f, 0.001);
}

/* ---- paddle bounce ------------------------------------------------------- */

TEST(bo_paddle_center_goes_straight_up) {
    /* Paddle at x=100 (centre 120); ball centre at 120 => dx == 0. */
    Ball ball{116.0f, 274.0f, 1.5f, 2.0f};
    const bool hit = paddle_bounce(ball, /*paddle_x=*/100, /*paddle_top=*/280);
    CHECK(hit);
    CHECK(ball.dy < 0.0f);              /* reflected upward */
    CHECK_NEAR(ball.dy, -2.0f, 0.001);
    CHECK_NEAR(ball.dx, 0.0f, 0.001);  /* dead centre */
    CHECK_NEAR(ball.y, 272.0f, 0.001); /* lifted to paddle_top - ball_size */
}

TEST(bo_paddle_left_deflects_left) {
    /* Ball centre at 108, left of paddle centre 120 => negative dx. */
    Ball ball{104.0f, 274.0f, 1.5f, 2.0f};
    const bool hit = paddle_bounce(ball, 100, 280);
    CHECK(hit);
    CHECK(ball.dx < 0.0f);
    CHECK_NEAR(ball.dx, -1.2f, 0.001);  /* (8/40 - 0.5) * 4 */
}

TEST(bo_paddle_right_deflects_right) {
    /* Ball centre at 132, right of paddle centre 120 => positive dx. */
    Ball ball{128.0f, 274.0f, 1.5f, 2.0f};
    const bool hit = paddle_bounce(ball, 100, 280);
    CHECK(hit);
    CHECK(ball.dx > 0.0f);
    CHECK_NEAR(ball.dx, 1.2f, 0.001);  /* (32/40 - 0.5) * 4 */
}

TEST(bo_paddle_no_hit_when_above) {
    Ball ball{116.0f, 100.0f, 1.5f, 2.0f};  /* well above the paddle */
    const bool hit = paddle_bounce(ball, 100, 280);
    CHECK(!hit);
    CHECK_NEAR(ball.dy, 2.0f, 0.001);  /* unchanged */
    CHECK_NEAR(ball.y, 100.0f, 0.001);
}

/* ---- ball -> brick cell mapping ------------------------------------------ */

TEST(bo_ball_brick_cell_top_left) {
    Ball ball{0.0f, 55.0f, 1.0f, 2.0f};
    int row = -1, col = -1;
    CHECK(ball_brick_cell(ball, kBW, kCols, row, col));
    CHECK_EQ(row, 0);
    CHECK_EQ(col, 0);
}

TEST(bo_ball_brick_cell_row1_col2) {
    Ball ball{46.0f, 67.0f, 1.0f, 2.0f};
    int row = -1, col = -1;
    CHECK(ball_brick_cell(ball, kBW, kCols, row, col));
    CHECK_EQ(row, 1);
    CHECK_EQ(col, 2);
}

TEST(bo_ball_brick_cell_below_field) {
    Ball ball{0.0f, 200.0f, 1.0f, 2.0f};  /* well below the brick rows */
    int row = -1, col = -1;
    CHECK(!ball_brick_cell(ball, kBW, kCols, row, col));
}

TEST(bo_ball_brick_cell_past_last_column) {
    Ball ball{300.0f, 55.0f, 1.0f, 2.0f};  /* col index would be >= kCols */
    int row = -1, col = -1;
    CHECK(!ball_brick_cell(ball, kBW, kCols, row, col));
}

/* ---- brick collision: removal, score, reflection ------------------------- */

TEST(bo_brick_collision_removes_and_scores) {
    auto bricks = full_wall();
    unsigned count = count_bricks(bricks);
    CHECK_EQ(count, 50u);

    /* Ball overlapping brick (0,0), moving down. */
    Ball ball{0.0f, 55.0f, 1.0f, 2.0f};
    const BrickHit hit = brick_collision(ball, bricks, 0, 0, kBW, count);

    CHECK(hit.hit);
    CHECK_EQ(hit.score_delta, 50);          /* top row */
    CHECK(!bricks[0][0]);                    /* brick removed */
    CHECK_EQ(count, 49u);                    /* count decremented */
    CHECK(ball.dy < 0.0f);                   /* dy reflected */
    CHECK_NEAR(ball.dy, -2.0f, 0.001);
}

TEST(bo_brick_collision_bottom_row_scores_ten) {
    auto bricks = full_wall();
    unsigned count = count_bricks(bricks);
    /* Brick (4,0): left 0, top 103. */
    Ball ball{0.0f, 103.0f, 1.0f, 2.0f};
    const BrickHit hit = brick_collision(ball, bricks, 4, 0, kBW, count);
    CHECK(hit.hit);
    CHECK_EQ(hit.score_delta, 10);
    CHECK(!bricks[4][0]);
}

TEST(bo_brick_collision_absent_brick_is_no_hit) {
    auto bricks = full_wall();
    bricks[0][0] = false;  /* already cleared */
    unsigned count = count_bricks(bricks);
    Ball ball{0.0f, 55.0f, 1.0f, 2.0f};
    const BrickHit hit = brick_collision(ball, bricks, 0, 0, kBW, count);
    CHECK(!hit.hit);
    CHECK_EQ(hit.score_delta, 0);
    CHECK_NEAR(ball.dy, 2.0f, 0.001);  /* ball untouched */
}

TEST(bo_brick_collision_present_but_no_overlap) {
    auto bricks = full_wall();
    unsigned count = count_bricks(bricks);
    /* Brick (0,0) exists but the ball is nowhere near it. */
    Ball ball{200.0f, 200.0f, 1.0f, 2.0f};
    const BrickHit hit = brick_collision(ball, bricks, 0, 0, kBW, count);
    CHECK(!hit.hit);
    CHECK(bricks[0][0]);          /* brick survives */
    CHECK_EQ(count, 50u);
    CHECK_NEAR(ball.dy, 2.0f, 0.001);
}

TEST(bo_clearing_last_brick_empties_the_wall) {
    /* A one-brick wall: after the hit the count reaches zero (level complete). */
    std::vector<std::vector<bool>> bricks(kBrickRows, std::vector<bool>(kCols, false));
    bricks[0][0] = true;
    unsigned count = 1;
    Ball ball{0.0f, 55.0f, 1.0f, 2.0f};
    const BrickHit hit = brick_collision(ball, bricks, 0, 0, kBW, count);
    CHECK(hit.hit);
    CHECK_EQ(count, 0u);
    CHECK_EQ(count_bricks(bricks), 0u);
}
