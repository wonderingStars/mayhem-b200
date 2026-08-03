/*
 * mayhem-b200 — Chrome Dino endless-runner game.
 *
 * Host port of PortaPack Mayhem's external/dinogame app
 * (Copyright RocketGod, https://betaskynet.com; based on the original DinoGame
 * by various contributors). This game needs no radio — it is a self-contained
 * single-player runner — so the port is behaviour-for-behaviour: the same jump
 * arc, the same cactus/pterodactyl obstacles, the same collision hitboxes and
 * scoring. What changes is only the rendering model: the firmware drew each
 * frame incrementally to nurse the slow SPI LCD, while the host simply redraws
 * the whole view every frame from on_frame_sync().
 *
 * The physics that matter for correctness — the jump arc and the dino/obstacle
 * collision test — live in a UI-free `dino_logic` namespace so they can be unit
 * tested (see tests/test_dinogame.cpp) with the exact constants upstream uses.
 *
 * Copyright (C) 2026 mayhem-b200 contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef __MB200_UI_DINOGAME_H__
#define __MB200_UI_DINOGAME_H__

#include "ui.hpp"
#include "ui_widget.hpp"

#include <cstdint>
#include <string>

namespace app {

/* ---- pure physics/collision (tested in test_dinogame.cpp) ---------------- */
namespace dino_logic {

/* Reference geometry for the 240x320 PortaPack/host screen, computed exactly
 * as upstream's init_dimensions() does for that size. */
constexpr int kScreenWidth = 240;
constexpr int kScreenHeight = 320;
constexpr int kGroundHeight = 10;
constexpr int kDinoWidth = 34;
constexpr int kDinoHeight = 36;
constexpr int kDinoDuckWidth = 45;
constexpr int kDinoDuckHeight = 22;
constexpr int kGameAreaHeight = kScreenHeight * 160 / 320;         /* 160 */
constexpr int kGameAreaTop = kScreenHeight / 4;                    /* 80  */
constexpr int kGroundY = kGameAreaTop + kGameAreaHeight - kGroundHeight;  /* 230 */
constexpr int kDinoX = kScreenWidth / 8;                           /* 30  */
constexpr int kDinoY = kGroundY - kDinoHeight;                     /* 194 */
constexpr int kDinoDuckY = kGroundY - kDinoDuckHeight;             /* 208 */
constexpr int kJumpMaxHeight = kGameAreaHeight * 70 / 160;         /* 70  */
constexpr int kJumpSpeed = 3;
constexpr int kGameSpeedBase = 3;

/* The dino's vertical jump state machine, lifted verbatim from upstream's
 * per-frame jump handling in game_loop(). */
struct Jump {
    int16_t height{0};
    bool jumping{false};
    bool falling{false};

    void start() {
        if (!jumping) {
            jumping = true;
            falling = false;
            height = 0;
        }
    }

    /* Advance one frame. speed_modifier ramps the ascent/descent rate.
     * Lifted verbatim from upstream's per-frame jump handling. */
    void step(int speed_modifier) {
        if (!jumping) return;
        if (!falling)
            height = static_cast<int16_t>(height + kJumpSpeed + speed_modifier);
        if (height > kJumpMaxHeight && !falling)
            falling = true;
        if (falling)
            height = static_cast<int16_t>(height - (kJumpSpeed + speed_modifier));
        if (height < 0) {
            falling = false;
            jumping = false;
            height = 0;
        }
    }
};

/* A ground obstacle (cactus). Rests on the ground line; height is how far it
 * rises from it. */
struct Obstacle {
    int16_t x{0};
    uint8_t width{0};
    uint8_t height{0};
    bool active{false};

    void move(int speed_modifier) {
        x = static_cast<int16_t>(x - (kGameSpeedBase + speed_modifier));
    }
};

/* True when the dino overlaps the obstacle, using upstream's hitbox (a 5px
 * inset on every side for fairness). Pure geometry — the caller decides whether
 * the obstacle is active. */
inline bool dino_collides(int jump_height, bool ducking, const Obstacle& o) {
    int dino_left = kDinoX;
    int dino_right = kDinoX + (ducking ? kDinoDuckWidth : kDinoWidth);
    int dino_top = ducking ? kDinoDuckY : (kDinoY - jump_height);
    int dino_bottom = dino_top + (ducking ? kDinoDuckHeight : kDinoHeight);

    /* 5px inset on every side, for fairness — exactly upstream. */
    dino_left += 5;
    dino_right -= 5;
    dino_top += 5;
    dino_bottom -= 5;

    const int obs_top = kGroundY - o.height;
    const int obs_bottom = kGroundY;

    return dino_right > o.x &&
           dino_left < o.x + o.width &&
           dino_bottom > obs_top &&
           dino_top < obs_bottom;
}

}  // namespace dino_logic

/* ---- view --------------------------------------------------------------- */

class DinoGameView : public ui::View {
   public:
    DinoGameView();

    std::string title() const override { return "Dino Game"; }

    void on_show() override;
    void focus() override;
    void on_frame_sync() override;
    void paint(ui::Painter& painter) override;
    bool on_key(const ui::KeyEvent key) override;

   private:
    enum class State : uint8_t { MENU, PLAYING, GAME_OVER };

    void new_game();
    void game_loop();
    void update_obstacle();
    void manage_bird();
    void check_collision();

    void draw_playfield(ui::Painter& painter, ui::Point origin);
    void draw_sprite(ui::Painter& painter, ui::Point origin, int x, int y,
                     const uint16_t* sprite, int w, int h, bool bird);
    void draw_obstacle(ui::Painter& painter, ui::Point origin);
    static std::string score_to_string(uint32_t score);

    State state_{State::MENU};

    dino_logic::Jump jump_{};
    dino_logic::Obstacle obstacle_{};

    /* Pterodactyl */
    bool bird_in_game_{false};
    bool bird_up_{false};       /* vertical band: true=UP, false=DOWN */
    int16_t bird_x_{dino_logic::kScreenWidth};
    int16_t bird_y_offset_{0};
    int8_t bird_y_velocity_{0};
    bool bird_flop_{false};

    bool ducking_{false};
    uint8_t duck_timer_{0};
    bool run_frame_{false};
    bool collided_{false};

    int speed_modifier_{0};
    uint32_t steps_{0};
    uint32_t score_{0};
    uint32_t high_score_{0};
    int32_t obstacle_spawn_timer_{100};
    uint8_t ground_offset_{0};

    bool easy_mode_{false};

    /* menu animation */
    uint32_t blink_counter_{0};
    bool blink_state_{true};
};

}  // namespace app

#endif /*__MB200_UI_DINOGAME_H__*/
