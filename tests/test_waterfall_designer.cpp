/*
 * mayhem-b200 — waterfall gradient designer tests.
 *
 * Expected values come from firmware/application/gradient.cpp (Gradient::step /
 * set_default / load_file) and from the "index,R,G,B" acceptance rules in the
 * upstream designer's parse_level. The interpolation formula under test is
 *     prev + (t * (val - prev) + range/2) / range,  range >= 1
 * with C++ truncation semantics, evaluated here by hand.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "test_main.hpp"

#include "ui.hpp"
#include "ui_waterfall_designer.hpp"

#include <string>
#include <vector>

using namespace app::waterfall_designer_detail;

/* --- parse_level: acceptance ----------------------------------------------- */

TEST(wtf_parse_level_valid) {
    Level l{};
    CHECK(parse_level("86,0,0,255", l));
    CHECK_EQ(int{l.index}, 86);
    CHECK_EQ(int{l.r}, 0);
    CHECK_EQ(int{l.g}, 0);
    CHECK_EQ(int{l.b}, 255);

    /* Surrounding whitespace and a trailing CR are tolerated. */
    Level l2{};
    CHECK(parse_level(" 10 , 20 , 30 , 40 \r", l2));
    CHECK_EQ(int{l2.index}, 10);
    CHECK_EQ(int{l2.r}, 20);
    CHECK_EQ(int{l2.g}, 30);
    CHECK_EQ(int{l2.b}, 40);
}

TEST(wtf_parse_level_rejects) {
    Level l{};
    CHECK(!parse_level("", l));               /* blank */
    CHECK(!parse_level("#0,0,0,0", l));       /* comment */
    CHECK(!parse_level("1,2,3", l));          /* too few columns */
    CHECK(!parse_level("1,2,3,4,5", l));      /* too many columns */
    CHECK(!parse_level("256,0,0,0", l));      /* index out of range */
    CHECK(!parse_level("0,0,0,300", l));      /* channel out of range */
    CHECK(!parse_level("a,0,0,0", l));        /* non-numeric */
    CHECK(!parse_level("1,,2,3", l));         /* empty field */
    CHECK(!parse_level("-1,0,0,0", l));       /* negative (has '-') */
}

TEST(wtf_is_color_level) {
    CHECK(is_color_level("255,255,0,0"));
    CHECK(!is_color_level("# header"));
    CHECK(!is_color_level("garbage line"));
}

/* --- interp_component: exact integer maths --------------------------------- */

TEST(wtf_interp_endpoints) {
    /* At the control points the value equals the input exactly. */
    CHECK_EQ(interp_component(0, 255, 0, 86), 0);
    CHECK_EQ(interp_component(0, 255, 86, 86), 255);
    /* Equal endpoints stay flat. */
    CHECK_EQ(interp_component(100, 100, 5, 10), 100);
}

TEST(wtf_interp_midpoint_and_rounding) {
    /* Halfway from 0 to 255 over a span of 86: (43*255 + 43)/86 = 11008/86 = 128. */
    CHECK_EQ(interp_component(0, 255, 43, 86), 128);
    /* One step in: (1*255 + 43)/86 = 298/86 = 3. */
    CHECK_EQ(interp_component(0, 255, 1, 86), 3);
    /* Rounds to nearest via + range/2: 2.5 -> 3, 7.5 -> 8. */
    CHECK_EQ(interp_component(0, 10, 1, 4), 3);
    CHECK_EQ(interp_component(0, 10, 3, 4), 8);
}

TEST(wtf_interp_range_zero_guard) {
    /* range 0 is clamped to 1 (upstream's divide-by-zero guard). */
    CHECK_EQ(interp_component(0, 255, 0, 0), 0);
}

/* --- Gradient::set_default: control points and a midpoint ------------------ */

TEST(wtf_gradient_default_control_points) {
    Gradient g;
    g.set_default();

    /* Endpoints are exact even after RGB565 quantisation, because at a control
     * point the interpolated colour equals the input colour. */
    CHECK_EQ(g.lut[0].v, ui::Color(0, 0, 0).v);
    CHECK_EQ(g.lut[86].v, ui::Color(0, 0, 255).v);
    CHECK_EQ(g.lut[171].v, ui::Color(0, 255, 0).v);
    CHECK_EQ(g.lut[255].v, ui::Color(255, 0, 0).v);

    /* Halfway into the first segment: only blue moves, to 128. */
    CHECK_EQ(g.lut[43].v, ui::Color(0, 0, 128).v);
}

/* --- Gradient::load_levels: parity with set_default, comments, flat-extend -- */

TEST(wtf_gradient_load_levels_matches_default) {
    const std::vector<std::string> lines{
        "0,0,0,0", "86,0,0,255", "171,0,255,0", "255,255,0,0"};

    Gradient loaded;
    loaded.load_levels(lines);

    Gradient def;
    def.set_default();

    for (int i = 0; i < 256; i++)
        CHECK_EQ(loaded.lut[i].v, def.lut[i].v);
}

TEST(wtf_gradient_load_levels_skips_comments) {
    Gradient g;
    g.load_levels({"# a header line", "86,0,0,255", "garbage"});
    /* The one valid level still lands at index 86. */
    CHECK_EQ(g.lut[86].v, ui::Color(0, 0, 255).v);
}

TEST(wtf_gradient_load_levels_flat_extends) {
    Gradient g;
    g.load_levels({"100,255,255,255"});
    /* The last colour is held from its index out to 255. */
    CHECK_EQ(g.lut[100].v, ui::Color(255, 255, 255).v);
    CHECK_EQ(g.lut[200].v, ui::Color(255, 255, 255).v);
    CHECK_EQ(g.lut[255].v, ui::Color(255, 255, 255).v);
}

/* --- level_to_string round-trips through parse_level ----------------------- */

TEST(wtf_level_to_string_round_trip) {
    const Level in{86, 0, 0, 255};
    const std::string s = level_to_string(in);
    CHECK_STR_EQ(s, "86,0,0,255");

    Level out{};
    CHECK(parse_level(s, out));
    CHECK_EQ(int{out.index}, 86);
    CHECK_EQ(int{out.r}, 0);
    CHECK_EQ(int{out.g}, 0);
    CHECK_EQ(int{out.b}, 255);
}
