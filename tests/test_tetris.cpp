/*
 * mayhem-b200 — Tetris rules-engine tests.
 *
 * The TetrisEngine is exercised headless against known board states, checking
 * the rules ported from upstream ui_tetris.cpp:
 *   - spawning: the 7 tetromino shapes and their spawn positions (the I-piece
 *     one row higher);
 *   - rotation: every piece stays inside the board after rotating through all
 *     four orientations, and the O-piece never rotates;
 *   - gravity/soft-drop collision against the floor;
 *   - locking a piece writes its colour into the board;
 *   - line detection for a filled row, multi-line (Tetris) clears, and the
 *     collapse/shift of the rows above a cleared line;
 *   - scoring: 40/100/300/1200 scaled by (level+1).
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "test_main.hpp"

#include "ui_tetris.hpp"

#include <algorithm>

using namespace app::tetris_logic;
using namespace mb200test;

/* ---- spawn / shapes ------------------------------------------------------ */

TEST(tetris_spawn_I_is_higher) {
    TetrisEngine e;
    e.clear_board();
    e.spawn(1);  /* I-tetromino */
    CHECK_EQ(static_cast<int>(e.boardX), -1);
    CHECK_EQ(static_cast<int>(e.boardY), 4);
    CHECK_EQ(static_cast<int>(e.colorIndex), 1);
    for (int i = 0; i < 4; ++i) {
        CHECK_EQ(static_cast<int>(e.X[i]), static_cast<int>(TetrisEngine::figuresX[0][i]));
        CHECK_EQ(static_cast<int>(e.Y[i]), static_cast<int>(TetrisEngine::figuresY[0][i]));
    }
}

TEST(tetris_spawn_others_at_top) {
    TetrisEngine e;
    e.clear_board();
    for (unsigned char c = 2; c <= 7; ++c) {
        e.spawn(c);
        CHECK_EQ(static_cast<int>(e.boardX), 0);
        CHECK_EQ(static_cast<int>(e.boardY), 4);
        CHECK_EQ(static_cast<int>(e.colorIndex), static_cast<int>(c));
    }
}

/* ---- rotation ------------------------------------------------------------ */

TEST(tetris_rotation_stays_in_bounds) {
    for (unsigned char c = 1; c <= 7; ++c) {
        TetrisEngine e;
        e.clear_board();
        e.spawn(c);
        /* Drop the piece into open space in the middle of the well. */
        for (int k = 0; k < 6; ++k) e.moveDown(1);

        for (int turn = 0; turn < 4; ++turn) {
            e.rotate();
            for (int i = 0; i < 4; ++i) {
                const int r = e.boardX + e.X[i];
                const int col = e.boardY + e.Y[i];
                CHECK(r >= 0 && r < TetrisEngine::kRows);
                CHECK(col >= 0 && col < TetrisEngine::kCols);
            }
        }
    }
}

TEST(tetris_O_does_not_rotate) {
    TetrisEngine e;
    e.clear_board();
    e.spawn(2);  /* O-tetromino */
    const int bx = e.boardX, by = e.boardY;
    const bool rotated = e.rotate();
    CHECK(!rotated);
    CHECK_EQ(static_cast<int>(e.boardX), bx);
    CHECK_EQ(static_cast<int>(e.boardY), by);
}

/* ---- horizontal movement / collision ------------------------------------- */

TEST(tetris_move_left_right) {
    TetrisEngine e;
    e.clear_board();
    e.spawn(2);  /* O-tetromino at cols 4-5 */
    const int start = e.boardY;

    CHECK(e.moveLeft());
    CHECK_EQ(static_cast<int>(e.boardY), start - 1);
    CHECK(e.moveRight());
    CHECK_EQ(static_cast<int>(e.boardY), start);

    while (e.moveLeft()) { /* slide to the left wall */
    }
    CHECK(e.inCollisionLeft());
    int min_col = 9;
    for (int i = 0; i < 4; ++i) min_col = std::min(min_col, static_cast<int>(e.Y[i]));
    CHECK_EQ(e.boardY + min_col, 0);  /* leftmost cell hard against the wall */
}

TEST(tetris_gravity_hits_floor) {
    TetrisEngine e;
    e.clear_board();
    e.spawn(2);  /* O-tetromino */
    while (e.moveDown(1)) { /* fall until it lands */
    }
    CHECK(e.inCollisionDown(1));
    int max_row = 0;
    for (int i = 0; i < 4; ++i) max_row = std::max(max_row, static_cast<int>(e.X[i]));
    CHECK_EQ(e.boardX + max_row, 19);  /* bottom cell on the last row */
}

/* ---- locking ------------------------------------------------------------- */

TEST(tetris_locked_piece_writes_board) {
    TetrisEngine e;
    e.clear_board();
    e.spawn(4);  /* S/Z-family piece, colour index 4 */
    e.moveDown(1);
    e.moveDown(1);
    e.onAttached();
    for (int i = 0; i < 4; ++i) {
        const int r = e.boardX + e.X[i];
        const int col = e.boardY + e.Y[i];
        CHECK_EQ(static_cast<int>(e.board[r][col]), 4);
    }
}

/* ---- line detection ------------------------------------------------------ */

TEST(tetris_check_lines_single_full_row) {
    TetrisEngine e;
    e.clear_board();
    for (int j = 0; j < TetrisEngine::kCols; ++j) e.board[19][j] = 1;

    short first_line = 0, count = 0;
    e.checkLines(first_line, count);
    CHECK_EQ(static_cast<int>(first_line), 19);
    CHECK_EQ(static_cast<int>(count), 1);
}

TEST(tetris_check_lines_ignores_partial_row) {
    TetrisEngine e;
    e.clear_board();
    for (int j = 0; j < TetrisEngine::kCols - 1; ++j) e.board[19][j] = 1;  /* one gap */

    short first_line = 0, count = 0;
    e.checkLines(first_line, count);
    CHECK_EQ(static_cast<int>(count), 0);
    CHECK_EQ(static_cast<int>(first_line), -1);
}

TEST(tetris_check_lines_four_rows) {
    TetrisEngine e;
    e.clear_board();
    for (int i = 16; i <= 19; ++i)
        for (int j = 0; j < TetrisEngine::kCols; ++j) e.board[i][j] = 2;

    short first_line = 0, count = 0;
    e.checkLines(first_line, count);
    CHECK_EQ(static_cast<int>(first_line), 19);
    CHECK_EQ(static_cast<int>(count), 4);
}

/* ---- line clearing / board collapse -------------------------------------- */

TEST(tetris_clear_single_line) {
    TetrisEngine e;
    e.clear_board();
    e.level = 0;
    for (int j = 0; j < TetrisEngine::kCols; ++j) e.board[19][j] = 1;

    const int cleared = e.updateBoard();
    CHECK_EQ(cleared, 1);
    CHECK_EQ(e.score, 40u);
    for (int j = 0; j < TetrisEngine::kCols; ++j)
        CHECK_EQ(static_cast<int>(e.board[19][j]), 0);
}

TEST(tetris_clear_shifts_rows_above) {
    TetrisEngine e;
    e.clear_board();
    e.level = 0;
    for (int j = 0; j < TetrisEngine::kCols; ++j) e.board[19][j] = 3;  /* full row */
    e.board[18][0] = 5;                                                /* lone block above */

    const int cleared = e.updateBoard();
    CHECK_EQ(cleared, 1);
    CHECK_EQ(static_cast<int>(e.board[19][0]), 5);  /* the block fell one row */
    CHECK_EQ(static_cast<int>(e.board[18][0]), 0);
    CHECK_EQ(e.score, 40u);
}

TEST(tetris_clear_four_lines_is_a_tetris) {
    TetrisEngine e;
    e.clear_board();
    e.level = 0;
    for (int i = 16; i <= 19; ++i)
        for (int j = 0; j < TetrisEngine::kCols; ++j) e.board[i][j] = 2;

    const int cleared = e.updateBoard();
    CHECK_EQ(cleared, 4);
    CHECK_EQ(e.score, 1200u);
    for (int i = 16; i <= 19; ++i)
        for (int j = 0; j < TetrisEngine::kCols; ++j)
            CHECK_EQ(static_cast<int>(e.board[i][j]), 0);
}

TEST(tetris_no_full_line_no_change) {
    TetrisEngine e;
    e.clear_board();
    e.level = 0;
    e.board[19][0] = 1;
    e.board[19][1] = 1;

    const int cleared = e.updateBoard();
    CHECK_EQ(cleared, 0);
    CHECK_EQ(e.score, 0u);
    CHECK_EQ(static_cast<int>(e.board[19][0]), 1);  /* untouched */
    CHECK_EQ(static_cast<int>(e.board[19][1]), 1);
}

/* ---- scoring ------------------------------------------------------------- */

TEST(tetris_score_table_level0) {
    TetrisEngine e;
    e.level = 0;
    CHECK_EQ(e.updateScore(0), 0u);
    CHECK_EQ(e.updateScore(1), 40u);
    CHECK_EQ(e.updateScore(2), 100u);
    CHECK_EQ(e.updateScore(3), 300u);
    CHECK_EQ(e.updateScore(4), 1200u);
}

TEST(tetris_score_table_scales_with_level) {
    TetrisEngine e;
    e.level = 1;
    CHECK_EQ(e.updateScore(1), 80u);
    CHECK_EQ(e.updateScore(4), 2400u);
    e.level = 3;
    CHECK_EQ(e.updateScore(1), 160u);
    CHECK_EQ(e.updateScore(4), 4800u);
}

/* ---- top-out ------------------------------------------------------------- */

TEST(tetris_is_over) {
    TetrisEngine e;
    e.clear_board();
    CHECK(!e.isOver());
    e.board[0][5] = 1;
    CHECK(e.isOver());
}
