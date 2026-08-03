/*
 * mayhem-b200 — 2048 board-logic tests.
 *
 * Rows and grids are set up by hand and checked against the definition of a
 * 2048 move: compress non-zero tiles toward index 0, merge each adjacent equal
 * pair exactly once (left→right), compress again; score gains the value of each
 * merged tile. A move is possible iff an empty cell or an adjacent equal pair
 * exists — its absence is game over.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "test_main.hpp"

#include "ui_game2048.hpp"

using namespace app::game2048_logic;
using namespace mb200test;

namespace {

/* Copy a 4-int list into a Row and run move_row_left, returning the result. */
MoveResult run_row(int v0, int v1, int v2, int v3, Row& out) {
    out[0] = v0;
    out[1] = v1;
    out[2] = v2;
    out[3] = v3;
    return move_row_left(out);
}

void set_grid(Grid& g, const int src[4][4]) {
    for (int i = 0; i < 4; ++i)
        for (int j = 0; j < 4; ++j) g[i][j] = src[i][j];
}

}  // namespace

/* ---- single-row slide + merge -------------------------------------------- */

TEST(g2048_row_slide_only) {
    /* No merge, just a slide: two 2s of different value nearby. */
    Row r;
    const MoveResult res = run_row(0, 0, 0, 2, r);
    CHECK(res.moved);
    CHECK_EQ(res.score_delta, 0);
    CHECK_EQ(r[0], 2);
    CHECK_EQ(r[1], 0);
    CHECK_EQ(r[2], 0);
    CHECK_EQ(r[3], 0);
}

TEST(g2048_row_simple_merge) {
    Row r;
    const MoveResult res = run_row(2, 2, 0, 0, r);
    CHECK(res.moved);
    CHECK_EQ(res.score_delta, 4);
    CHECK_EQ(r[0], 4);
    CHECK_EQ(r[1], 0);
    CHECK_EQ(r[2], 0);
    CHECK_EQ(r[3], 0);
}

TEST(g2048_row_merge_with_gap) {
    /* 2 . 2 . -> 4 . . . */
    Row r;
    const MoveResult res = run_row(2, 0, 2, 0, r);
    CHECK(res.moved);
    CHECK_EQ(res.score_delta, 4);
    CHECK_EQ(r[0], 4);
    CHECK_EQ(r[1], 0);
}

/* The load-bearing rule: a run of four equal tiles becomes TWO merges, not one
 * cascade. [2 2 2 2] -> [4 4 0 0], not [8 0 0 0]. */
TEST(g2048_row_merge_only_once_per_move) {
    Row r;
    const MoveResult res = run_row(2, 2, 2, 2, r);
    CHECK(res.moved);
    CHECK_EQ(res.score_delta, 8);  /* two 4s formed */
    CHECK_EQ(r[0], 4);
    CHECK_EQ(r[1], 4);
    CHECK_EQ(r[2], 0);
    CHECK_EQ(r[3], 0);
}

/* Three equal tiles: only the leftmost pair merges; the third slides in. */
TEST(g2048_row_triple_merges_leftmost_pair) {
    Row r;
    const MoveResult res = run_row(4, 4, 4, 0, r);
    CHECK(res.moved);
    CHECK_EQ(res.score_delta, 8);
    CHECK_EQ(r[0], 8);
    CHECK_EQ(r[1], 4);
    CHECK_EQ(r[2], 0);
    CHECK_EQ(r[3], 0);
}

TEST(g2048_row_two_independent_pairs) {
    /* 2 2 4 4 -> 4 8 0 0, score 4 + 8 = 12. */
    Row r;
    const MoveResult res = run_row(2, 2, 4, 4, r);
    CHECK(res.moved);
    CHECK_EQ(res.score_delta, 12);
    CHECK_EQ(r[0], 4);
    CHECK_EQ(r[1], 8);
    CHECK_EQ(r[2], 0);
    CHECK_EQ(r[3], 0);
}

TEST(g2048_row_no_move_when_settled) {
    /* Already-packed row with no equal neighbours does not move. */
    Row r;
    const MoveResult res = run_row(2, 4, 8, 16, r);
    CHECK(!res.moved);
    CHECK_EQ(res.score_delta, 0);
    CHECK_EQ(r[0], 2);
    CHECK_EQ(r[3], 16);
}

TEST(g2048_row_empty_is_no_move) {
    Row r;
    const MoveResult res = run_row(0, 0, 0, 0, r);
    CHECK(!res.moved);
    CHECK_EQ(res.score_delta, 0);
}

/* ---- whole-grid directional moves ---------------------------------------- */

TEST(g2048_move_left_accumulates_score) {
    const int src[4][4] = {
        {2, 2, 2, 2},
        {0, 0, 0, 0},
        {4, 0, 4, 0},
        {0, 0, 0, 0},
    };
    Grid g;
    set_grid(g, src);
    const MoveResult res = move_left(g);
    CHECK(res.moved);
    CHECK_EQ(res.score_delta, 8 + 8);  /* row0: 4+4=8, row2: one 8 */
    CHECK_EQ(g[0][0], 4);
    CHECK_EQ(g[0][1], 4);
    CHECK_EQ(g[2][0], 8);
}

TEST(g2048_move_right) {
    const int src[4][4] = {
        {2, 2, 0, 0},
        {0, 0, 0, 0},
        {0, 0, 0, 0},
        {0, 0, 0, 0},
    };
    Grid g;
    set_grid(g, src);
    const MoveResult res = move_right(g);
    CHECK(res.moved);
    CHECK_EQ(res.score_delta, 4);
    CHECK_EQ(g[0][3], 4);
    CHECK_EQ(g[0][2], 0);
}

TEST(g2048_move_up_merges_column) {
    const int src[4][4] = {
        {2, 0, 0, 0},
        {2, 0, 0, 0},
        {0, 0, 0, 0},
        {0, 0, 0, 0},
    };
    Grid g;
    set_grid(g, src);
    const MoveResult res = move_up(g);
    CHECK(res.moved);
    CHECK_EQ(res.score_delta, 4);
    CHECK_EQ(g[0][0], 4);
    CHECK_EQ(g[1][0], 0);
}

TEST(g2048_move_down_merges_column) {
    const int src[4][4] = {
        {2, 0, 0, 0},
        {2, 0, 0, 0},
        {0, 0, 0, 0},
        {0, 0, 0, 0},
    };
    Grid g;
    set_grid(g, src);
    const MoveResult res = move_down(g);
    CHECK(res.moved);
    CHECK_EQ(res.score_delta, 4);
    CHECK_EQ(g[3][0], 4);
    CHECK_EQ(g[2][0], 0);
}

TEST(g2048_move_that_changes_nothing_reports_no_move) {
    /* Everything packed left already, no vertical merges: move_left is a no-op. */
    const int src[4][4] = {
        {2, 4, 8, 16},
        {4, 8, 16, 32},
        {8, 16, 32, 64},
        {16, 32, 64, 128},
    };
    Grid g;
    set_grid(g, src);
    const MoveResult res = move_left(g);
    CHECK(!res.moved);
    CHECK_EQ(res.score_delta, 0);
}

/* ---- can_move / game over ------------------------------------------------ */

TEST(g2048_can_move_with_empty_cell) {
    const int src[4][4] = {
        {2, 4, 2, 4},
        {4, 2, 4, 2},
        {2, 4, 2, 4},
        {4, 2, 4, 0},  /* one empty cell */
    };
    Grid g;
    set_grid(g, src);
    CHECK(can_move(g));
}

TEST(g2048_can_move_with_horizontal_pair) {
    const int src[4][4] = {
        {2, 2, 8, 16},  /* adjacent equal on the top row */
        {4, 8, 16, 32},
        {8, 16, 32, 64},
        {16, 32, 64, 128},
    };
    Grid g;
    set_grid(g, src);
    CHECK(can_move(g));
}

TEST(g2048_can_move_with_vertical_pair) {
    const int src[4][4] = {
        {2, 4, 8, 16},
        {2, 8, 16, 32},  /* column 0 has 2 over 2 */
        {8, 16, 32, 64},
        {16, 32, 64, 128},
    };
    Grid g;
    set_grid(g, src);
    CHECK(can_move(g));
}

TEST(g2048_game_over_when_full_and_no_pairs) {
    /* A full board in checkerboard values: no empty cell, no equal neighbour. */
    const int src[4][4] = {
        {2, 4, 2, 4},
        {4, 2, 4, 2},
        {2, 4, 2, 4},
        {4, 2, 4, 2},
    };
    Grid g;
    set_grid(g, src);
    CHECK(!can_move(g));
}

/* ---- win detection ------------------------------------------------------- */

TEST(g2048_is_won_true_when_2048_present) {
    const int src[4][4] = {
        {2, 4, 8, 16},
        {32, 64, 128, 256},
        {512, 1024, 2048, 2},
        {4, 8, 16, 32},
    };
    Grid g;
    set_grid(g, src);
    CHECK(is_won(g));
}

TEST(g2048_is_won_false_otherwise) {
    const int src[4][4] = {
        {2, 4, 8, 16},
        {32, 64, 128, 256},
        {512, 1024, 4, 2},
        {4, 8, 16, 32},
    };
    Grid g;
    set_grid(g, src);
    CHECK(!is_won(g));
}
