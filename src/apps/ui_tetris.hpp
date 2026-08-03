/*
 * mayhem-b200 — Tetris (Games).
 *
 * Host port of PortaPack Mayhem's external/tetris app (Copyright 2024 Mark
 * Thompson, 2025 updates by RocketGod). Upstream implemented the whole game as
 * free functions over a set of file-scope globals (board[20][10], the current
 * piece coordinates, boardX/boardY, level and score) with the fall/joystick
 * timers driven off the display frame-sync, and every rule call also poked the
 * LCD. Here the rules are lifted verbatim into a TetrisEngine so they can be
 * unit-tested headless — the 7 tetromino shapes, SRS-style rotation with wall
 * kicks, gravity/soft-drop collision, line detection/clearing, scoring and the
 * top-out test — while the View is a thin shell that renders the engine state
 * on demand and owns the level menu / game-over screens.
 *
 * The board coordinate convention is upstream's: board[row][col] with row 0..19
 * top-to-bottom and col 0..9; boardX is the piece's row offset and boardY its
 * column offset; X[i]/Y[i] are per-cell row/column offsets. A cell is empty when
 * 0, otherwise it holds the piece's colour index 1..7.
 *
 * Copyright (C) 2026 mayhem-b200 contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef __MB200_UI_TETRIS_H__
#define __MB200_UI_TETRIS_H__

#include "ui.hpp"
#include "ui_widget.hpp"

#include <cstdint>
#include <string>

namespace app {

/* ---- pure game logic (tested in test_tetris.cpp) ------------------------- */
namespace tetris_logic {

class TetrisEngine {
   public:
    static constexpr int kRows = 20;
    static constexpr int kCols = 10;

    /* The 7 tetromino cell offsets, indexed [pieceIndex 0..6][cell 0..3].
     * figuresX are row offsets, figuresY are column offsets — verbatim from
     * upstream ui_tetris.cpp. Piece index = colorIndex - 1. */
    static constexpr short figuresX[7][4] = {
        {0, 0, 0, 0}, {0, 0, 1, 1}, {0, 1, 1, 1}, {1, 1, 0, 0},
        {0, 1, 0, 1}, {1, 1, 1, 0}, {1, 1, 1, 0}};
    static constexpr short figuresY[7][4] = {
        {0, 1, 2, 3}, {1, 0, 0, 1}, {1, 1, 2, 0}, {0, 1, 1, 2},
        {0, 1, 1, 2}, {2, 1, 0, 0}, {0, 1, 2, 2}};

    /* Fall delay in seconds per level (0..3), upstream delays[]. */
    static constexpr float delays[4] = {1.2f, 0.7f, 0.4f, 0.25f};

    /* Board state, current piece and score/level. Public for testing. */
    short board[kRows][kCols]{};
    short X[4]{};
    short Y[4]{};
    short boardX{0};          /* piece row offset */
    short boardY{4};          /* piece column offset */
    unsigned char colorIndex{1};
    int rotation_state{0};
    unsigned int score{0};
    unsigned char level{0};

    void clear_board();

    /* Place piece c (1..7) at the top spawn position, upstream Initialize(). The
     * I-tetromino (c==1) spawns one row higher, exactly as upstream. */
    void spawn(unsigned char c);

    /* Edge/bounds predicates, upstream BottomEdge/LeftEdge/RightEdge/OutOfBounds. */
    static bool bottomEdge(int row) { return row > 19; }
    static bool leftEdge(int col) { return col < 0; }
    static bool rightEdge(int col) { return col > 9; }
    static bool outOfBounds(int col, int row) { return col < 0 || col > 9 || row > 19; }

    /* Collision tests for a candidate move (guarded against off-board reads,
     * which upstream leaves as latent UB but never actually reaches in play). */
    bool inCollisionDown(int delta) const;
    bool inCollisionLeft() const;
    bool inCollisionRight() const;

    /* Moves; each returns whether it actually happened. */
    bool moveDown(int delta);
    bool moveLeft();
    bool moveRight();

    /* SRS rotation with wall kicks; O-tetromino never rotates. Returns whether
     * a valid rotation (possibly kicked) was applied. */
    bool rotate();

    /* Lock the current piece into the board (upstream OnAttached()). */
    void onAttached();

    /* Find the lowest contiguous block of full lines: firstLine = its bottom
     * row index, numberOfLines = how many. Upstream CheckLines(). */
    void checkLines(short& firstLine, short& numberOfLines) const;

    /* Base line-clear award for a simultaneous n-line clear, scaled by level.
     * Upstream UpdateScore(): 40/100/300/1200 * (level+1). */
    unsigned int updateScore(short numOfLines) const;

    /* Clear every full line, collapse the board and add the score. Returns the
     * total number of lines cleared. Upstream UpdateBoard(). */
    int updateBoard();

    /* Top-out: any cell occupied on the top row. Upstream IsOver(). */
    bool isOver() const;
};

}  // namespace tetris_logic

enum class TetrisState : uint8_t { LevelMenu, Playing, GameOver };

class TetrisView : public ui::View {
   public:
    TetrisView();

    std::string title() const override { return "Tetris"; }

    void on_show() override;
    void on_frame_sync() override;
    void paint(ui::Painter& painter) override;
    bool on_key(ui::KeyEvent key) override;
    bool on_encoder(ui::EncoderEvent delta) override;

   private:
    static unsigned char random_piece();  /* 1..7 */

    void start_game();
    void play_step();       /* one gravity tick, upstream PlayGame() */
    void soft_drop();
    void change_level(int delta);

    void compute_dimensions();

    void paint_menu(ui::Painter& painter, ui::Point o) const;
    void paint_playing(ui::Painter& painter, ui::Point o) const;
    void paint_game_over(ui::Painter& painter, ui::Point o) const;
    void draw_cell(ui::Painter& painter, ui::Point o, int row, int col, int color_index) const;

    tetris_logic::TetrisEngine engine_{};
    TetrisState state_{TetrisState::LevelMenu};
    unsigned char next_figure_{1};
    bool initialized_{false};
    bool paused_{false};

    uint32_t frame_counter_{0};
    uint32_t fall_frames_{72};  /* delays[level] * 60, recomputed on start */

    /* Rendering geometry, computed from the view size (upstream Init()). */
    int dimension_{14};
    int dimension_next_{10};
    int board_right_{142};
    int info_left_{145};
};

}  // namespace app

#endif /*__MB200_UI_TETRIS_H__*/
