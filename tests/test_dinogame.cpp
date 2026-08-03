/*
 * mayhem-b200 — Dino game physics/collision tests.
 *
 * Covers the two things that decide whether the game is fair: the jump arc
 * (rise, peak, fall, land) and the dino/obstacle collision test, including the
 * timing question the whole game hinges on — a well-timed jump clears an
 * obstacle, a mistimed jump (too late, or too early so the dino has already
 * landed) does not, and not jumping at all collides. All constants and the arc
 * come from upstream's game_loop()/check_collision().
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "test_main.hpp"

#include "ui_dinogame.hpp"

using namespace mb200test;
using namespace app::dino_logic;

namespace {

Obstacle large_cactus(int x) {
    Obstacle o;
    o.active = true;
    o.width = 20;  /* upstream "large single" */
    o.height = 35;
    o.x = static_cast<int16_t>(x);
    return o;
}

/* Simulate the runner one obstacle at a time, using game_loop()'s per-frame
 * order (obstacle moves, jump advances, then collision is tested). The jump is
 * started at the beginning of frame jump_frame (<0 = never). Returns true if
 * the dino ever collides before the obstacle leaves the screen. */
bool collides_during_run(int start_x, int jump_frame) {
    Jump j;
    Obstacle o = large_cactus(start_x);
    for (int f = 0; f < 200; ++f) {
        if (f == jump_frame) j.start();
        o.move(0);
        j.step(0);
        if (o.x < -50) break;
        if (dino_collides(j.height, false, o)) return true;
    }
    return false;
}

}  // namespace

/* ---- jump arc ------------------------------------------------------------ */

TEST(dg_jump_arc_rise_peak_land) {
    Jump j;
    CHECK(!j.jumping);
    j.start();
    CHECK(j.jumping);
    CHECK_EQ(j.height, static_cast<int16_t>(0));

    int peak = 0;
    int frames = 0;
    while (j.jumping && frames < 500) {
        j.step(0);
        if (j.height > peak) peak = j.height;
        ++frames;
    }

    /* Comes back down and ends cleanly on the ground. */
    CHECK(!j.jumping);
    CHECK_EQ(j.height, static_cast<int16_t>(0));

    /* Peak is within one step of the configured max height. */
    CHECK(peak >= kJumpMaxHeight - kJumpSpeed);
    CHECK(peak <= kJumpMaxHeight + kJumpSpeed);

    /* Airtime is sane (a bit under a second at 60 fps). */
    CHECK(frames >= 30);
    CHECK(frames <= 60);
}

TEST(dg_no_double_jump) {
    Jump j;
    j.start();
    for (int i = 0; i < 5; ++i) j.step(0);
    const int16_t mid = j.height;
    CHECK(j.jumping);
    /* A second press mid-air must not restart the arc. */
    j.start();
    CHECK(j.jumping);
    CHECK_EQ(j.height, mid);
}

TEST(dg_faster_when_ramped) {
    /* With a speed modifier the ascent is steeper, so height after N steps is
     * higher than at base speed. */
    Jump a;
    Jump b;
    a.start();
    b.start();
    for (int i = 0; i < 4; ++i) {
        a.step(0);
        b.step(3);
    }
    CHECK(b.height > a.height);
}

/* ---- collision geometry -------------------------------------------------- */

TEST(dg_collision_vertical_threshold) {
    const Obstacle o = large_cactus(40);  /* squarely in the dino's x-range */

    /* Grounded: collides. */
    CHECK(dino_collides(0, false, o));
    /* Just short of clearing (obs is 35 tall; need >=30 of jump). */
    CHECK(dino_collides(29, false, o));
    /* Exactly enough to clear. */
    CHECK(!dino_collides(30, false, o));
    /* Well above it. */
    CHECK(!dino_collides(50, false, o));
}

TEST(dg_collision_horizontal_edges) {
    /* dino hitbox spans x in [35,59); an obstacle at x=59 is one pixel clear,
     * at x=58 it just overlaps. */
    CHECK(!dino_collides(0, false, large_cactus(59)));
    CHECK(dino_collides(0, false, large_cactus(58)));
    /* Far away either side: no collision. */
    CHECK(!dino_collides(0, false, large_cactus(200)));
    CHECK(!dino_collides(0, false, large_cactus(-40)));
}

/* ---- timing: the core of the game --------------------------------------- */

TEST(dg_no_jump_collides) {
    /* An obstacle that reaches the dino while it stands on the ground hits. */
    CHECK(collides_during_run(/*start_x=*/100, /*jump_frame=*/-1));
}

TEST(dg_well_timed_jump_clears) {
    /* Same obstacle, but jumping as it approaches clears it every frame. */
    CHECK(!collides_during_run(/*start_x=*/100, /*jump_frame=*/0));
}

TEST(dg_late_jump_collides) {
    /* Jumping only after the obstacle is already on top of the dino fails. */
    CHECK(collides_during_run(/*start_x=*/100, /*jump_frame=*/30));
}

TEST(dg_early_jump_collides) {
    /* Jumping far too soon: the dino has landed again before a distant
     * obstacle arrives, so it still collides. */
    CHECK(collides_during_run(/*start_x=*/250, /*jump_frame=*/0));
}
