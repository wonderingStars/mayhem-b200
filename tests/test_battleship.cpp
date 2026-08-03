/*
 * mayhem-b200 — Battleship rules-engine tests.
 *
 * Exercises the UI-free battleship::Board: ship-placement validity (in bounds,
 * no overlap, no touching even diagonally) and shot resolution (miss / hit /
 * sunk / already-fired / out of bounds) plus fleet-defeat detection. Expected
 * outcomes come from the Battleship rules as implemented upstream, not from
 * whatever the code happens to return.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "test_main.hpp"

#include "ui_battleship.hpp"

using namespace mb200test;
using app::battleship::Board;
using app::battleship::CellState;
using app::battleship::kGridSize;
using app::battleship::ShotResult;

namespace {
/* Fire down a horizontal ship; returns the result of the final (sinking) shot. */
ShotResult sink_horizontal(Board& b, int x, int y, int size) {
    ShotResult r = ShotResult::INVALID;
    for (int i = 0; i < size; ++i)
        r = b.receive_shot(x + i, y);
    return r;
}
}  // namespace

/* ---- placement: bounds --------------------------------------------------- */

TEST(bt_fresh_board_state) {
    Board b;
    CHECK_EQ(b.ships_placed(), 0u);
    CHECK(!b.all_ships_placed());
    CHECK_EQ(b.next_ship_size(), 5u);  /* carrier first */
    CHECK_EQ(b.ships_remaining(), 0u);
    CHECK(!b.defeated());
    CHECK(b.at(0, 0) == CellState::EMPTY);
}

TEST(bt_placement_in_bounds) {
    Board b;  /* next ship is the carrier, size 5 */
    /* Horizontal running off the right edge. */
    CHECK(!b.can_place(6, 0, 5, true));
    CHECK(b.can_place(5, 0, 5, true));   /* 5..9 exactly fits */
    /* Vertical running off the bottom edge. */
    CHECK(!b.can_place(0, 6, 5, false));
    CHECK(b.can_place(0, 5, 5, false));  /* rows 5..9 exactly fits */
    /* Negative origins. */
    CHECK(!b.can_place(-1, 0, 5, true));
    CHECK(!b.can_place(0, -1, 5, false));
}

/* ---- placement: overlap and adjacency ------------------------------------ */

TEST(bt_place_marks_cells_and_advances) {
    Board b;
    CHECK(b.place_next(0, 0, true));
    CHECK_EQ(b.ships_placed(), 1u);
    CHECK_EQ(b.next_ship_size(), 4u);  /* battleship next */
    for (int x = 0; x < 5; ++x)
        CHECK(b.at(x, 0) == CellState::SHIP);
    CHECK(b.at(5, 0) == CellState::EMPTY);
}

TEST(bt_no_overlap) {
    Board b;
    CHECK(b.place_next(0, 0, true));      /* carrier on row 0, cols 0..4 */
    /* Next ship (battleship, 4) cannot sit on top of it. */
    CHECK(!b.can_place(0, 0, 4, true));
    CHECK(!b.place_next(0, 0, true));
    CHECK_EQ(b.ships_placed(), 1u);       /* placement was rejected */
}

TEST(bt_no_touching_orthogonal) {
    Board b;
    CHECK(b.place_next(0, 0, true));      /* row 0, cols 0..4 */
    /* Directly below is orthogonally adjacent -> illegal. */
    CHECK(!b.can_place(0, 1, 4, true));
    /* One row of clear water between them is legal. */
    CHECK(b.can_place(0, 2, 4, true));
}

TEST(bt_no_touching_diagonal) {
    Board b;
    CHECK(b.place_next(0, 0, true));      /* row 0, cols 0..4 */
    /* (5,1) touches the carrier's (4,0) corner diagonally -> illegal. */
    CHECK(!b.can_place(5, 1, 2, true));
    /* (6,1) is clear of the corner -> legal. */
    CHECK(b.can_place(6, 1, 2, true));
}

/* ---- shots: miss / hit / already fired / bounds -------------------------- */

TEST(bt_miss) {
    Board b;
    CHECK(b.place_next(0, 0, true));      /* carrier row 0 */
    CHECK(b.receive_shot(9, 9) == ShotResult::MISS);
    CHECK(b.at(9, 9) == CellState::MISS);
}

TEST(bt_hit_then_already_fired) {
    Board b;
    CHECK(b.place_next(0, 0, true));
    CHECK(b.receive_shot(0, 0) == ShotResult::HIT);
    CHECK(b.at(0, 0) == CellState::HIT);
    /* Firing the same cell again does nothing. */
    CHECK(b.receive_shot(0, 0) == ShotResult::INVALID);
    /* Re-firing a previous miss is also invalid. */
    CHECK(b.receive_shot(9, 9) == ShotResult::MISS);
    CHECK(b.receive_shot(9, 9) == ShotResult::INVALID);
}

TEST(bt_shot_out_of_bounds) {
    Board b;
    CHECK(b.place_next(0, 0, true));
    CHECK(b.receive_shot(-1, 0) == ShotResult::INVALID);
    CHECK(b.receive_shot(kGridSize, 0) == ShotResult::INVALID);
    CHECK(b.receive_shot(0, kGridSize) == ShotResult::INVALID);
}

/* ---- sinking ------------------------------------------------------------- */

TEST(bt_hits_before_sunk_stay_hit) {
    Board b;
    CHECK(b.place_next(0, 0, true));      /* carrier, size 5 */
    /* First four cells are HIT, not SUNK, and the fleet is still afloat. */
    for (int x = 0; x < 4; ++x) {
        CHECK(b.receive_shot(x, 0) == ShotResult::HIT);
        CHECK_EQ(b.ships_remaining(), 1u);
        CHECK(!b.defeated());
    }
    /* Fifth cell sinks it. */
    CHECK(b.receive_shot(4, 0) == ShotResult::SUNK);
    CHECK_EQ(b.ships_remaining(), 0u);
    CHECK(b.defeated());
    /* Every cell of the ship is now SUNK. */
    for (int x = 0; x < 5; ++x)
        CHECK(b.at(x, 0) == CellState::SUNK);
}

TEST(bt_vertical_ship_sinks) {
    Board b;
    CHECK(b.place_next(0, 0, false));     /* carrier vertical, col 0 rows 0..4 */
    CHECK(b.receive_shot(0, 0) == ShotResult::HIT);
    CHECK(sink_horizontal(b, 0, 0, 1) == ShotResult::INVALID);  /* already fired */
    for (int y = 1; y < 4; ++y)
        CHECK(b.receive_shot(0, y) == ShotResult::HIT);
    CHECK(b.receive_shot(0, 4) == ShotResult::SUNK);
    CHECK(b.defeated());
}

/* ---- full fleet: only defeated once every ship is sunk ------------------- */

TEST(bt_full_fleet_defeat) {
    Board b;
    /* Five ships on even rows, each separated by a clear row. */
    CHECK(b.place_next(0, 0, true));  /* carrier    5 -> row 0, c0..4 */
    CHECK(b.place_next(0, 2, true));  /* battleship 4 -> row 2, c0..3 */
    CHECK(b.place_next(0, 4, true));  /* cruiser    3 -> row 4, c0..2 */
    CHECK(b.place_next(0, 6, true));  /* submarine  3 -> row 6, c0..2 */
    CHECK(b.place_next(0, 8, true));  /* destroyer  2 -> row 8, c0..1 */
    CHECK(b.all_ships_placed());
    CHECK_EQ(b.next_ship_size(), 0u);
    CHECK(!b.place_next(0, 0, true)); /* no sixth ship */
    CHECK_EQ(b.ships_remaining(), 5u);

    CHECK(sink_horizontal(b, 0, 0, 5) == ShotResult::SUNK);
    CHECK_EQ(b.ships_remaining(), 4u);
    CHECK(!b.defeated());

    CHECK(sink_horizontal(b, 0, 2, 4) == ShotResult::SUNK);
    CHECK(sink_horizontal(b, 0, 4, 3) == ShotResult::SUNK);
    CHECK(sink_horizontal(b, 0, 6, 3) == ShotResult::SUNK);
    CHECK_EQ(b.ships_remaining(), 1u);
    CHECK(!b.defeated());

    CHECK(sink_horizontal(b, 0, 8, 2) == ShotResult::SUNK);
    CHECK_EQ(b.ships_remaining(), 0u);
    CHECK(b.defeated());
}
