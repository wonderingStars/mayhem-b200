/*
 * mayhem-b200 — Blackjack game.
 *
 * Host port of PortaPack Mayhem's external/blackjack app (ported / enhanced by
 * RocketGod, https://betaskynet.com; based on BlackJack 83 for the TI calculator
 * by Harper Maddox). The firmware drew the felt, cards and pip art from a 60 Hz
 * Ticker callback with a file-scope Painter; the host repaints from state in
 * paint() and steps the (single) dealer-turn delay in on_frame_sync(). All of
 * the card maths — hand value with aces counted as 1 or 11, dealer-stands-on-17,
 * bust/blackjack, and the win/lose/push resolution and bankroll — lives in the
 * render-free `blackjack_game` namespace and is unit tested against known hands.
 *
 * Copyright (C) 2024 RocketGod (original game)
 * Copyright (C) 2026 mayhem-b200 contributors (host implementation)
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef __MB200_UI_BLACKJACK_H__
#define __MB200_UI_BLACKJACK_H__

#include "ui.hpp"
#include "ui_widget.hpp"

#include <cstdint>
#include <string>

namespace app {

/* ---- pure, render-free card logic (tested in test_blackjack.cpp) ---------- */
namespace blackjack_game {

/* Cards are 0..51: rank = card%13 + 1 (1=Ace .. 13=King), suit = card/13
 * (0=Spades, 1=Hearts, 2=Diamonds, 3=Clubs). This is exactly upstream's deck. */
constexpr uint8_t MAX_CARDS_IN_HAND = 11;  /* most cards before an unavoidable bust */

inline int card_rank(uint8_t card) { return (card % 13) + 1; }          /* 1..13     */
inline int card_value(uint8_t card) {                                   /* face = 10 */
    int v = card_rank(card);
    return v > 10 ? 10 : v;
}
inline uint8_t card_suit(uint8_t card) { return static_cast<uint8_t>(card / 13); }

/* Build a card index from a 1..13 rank and 0..3 suit (handy for tests). */
inline uint8_t card_of(int rank, int suit = 0) {
    return static_cast<uint8_t>(suit * 13 + (rank - 1));
}

/* Best hand total: each ace counts 1, then promoted to 11 while it fits under 21
 * (soft hand). Verbatim from upstream calculate_hand_value(). */
int calculate_hand_value(const uint8_t* cards, uint8_t count);

/* A two-card 21. Upstream pays it 1:1 like any other win — no natural bonus. */
bool is_blackjack(const uint8_t* cards, uint8_t count);

/* The dealer hits below 17 and stands on 17 or more (soft 17 included, matching
 * upstream, which uses the same soft-aware total for the decision). */
bool dealer_should_hit(int dealer_value);

enum class Outcome : uint8_t { PlayerBust, DealerBust, PlayerWin, DealerWin, Push };

/* Resolve a finished hand from the two totals, in upstream's precedence order. */
Outcome resolve(int player_value, int dealer_value);

/* Fill deck[0..51] = 0..51 (unshuffled), for deterministic tests. */
void init_ordered_deck(uint8_t deck[52]);

/* Draw for the dealer from deck[pos++] until standing; returns the final total
 * and advances count/pos. Mirrors repeated calls to upstream dealer_turn(). */
int dealer_play(uint8_t* cards, uint8_t& count, const uint8_t* deck, uint8_t& pos);

/* Bankroll after a resolved hand. Cash floors at 0 on a loss; high_score only
 * ever rises. Matches upstream player_hit()/check_game_over() accounting. */
struct Bankroll {
    uint32_t cash = 100;
    uint32_t wins = 0;
    uint32_t losses = 0;
    uint32_t high_score = 100;
};
void apply_outcome(Bankroll& b, Outcome outcome, uint32_t bet);

}  // namespace blackjack_game

/* ---- view ----------------------------------------------------------------- */

class BlackjackView : public ui::View {
   public:
    enum class GameState : uint8_t { Menu, Betting, Playing, DealerTurn, GameOver, Stats };

    BlackjackView();

    std::string title() const override { return "Blackjack"; }

    void focus() override;
    void on_show() override;
    void on_frame_sync() override;
    void paint(ui::Painter& painter) override;
    bool on_key(const ui::KeyEvent key) override;
    bool on_encoder(const ui::EncoderEvent delta) override;

   private:
    void deal_cards();
    void player_hit();
    void player_stay();
    void dealer_step();       /* one dealer draw-or-stand, then maybe resolve */
    void resolve_and_settle();

    /* rendering (offset by the view origin) */
    void paint_menu(ui::Painter& painter, ui::Point o);
    void paint_stats(ui::Painter& painter, ui::Point o);
    void paint_betting(ui::Painter& painter, ui::Point o);
    void paint_game(ui::Painter& painter, ui::Point o);
    void draw_card(ui::Painter& painter, int x, int y, uint8_t card, bool hidden);
    void draw_hand(ui::Painter& painter, int x, int y, const uint8_t* cards,
                   uint8_t count, bool is_dealer);

    GameState game_state_ = GameState::Menu;

    int screen_w_ = 240;
    int screen_h_ = 304;
    int card_width_ = 50;
    int card_height_ = 65;

    uint8_t deck_[52]{};
    uint8_t deck_position_ = 0;

    uint8_t player_cards_[blackjack_game::MAX_CARDS_IN_HAND]{};
    uint8_t player_card_count_ = 0;
    uint8_t dealer_cards_[blackjack_game::MAX_CARDS_IN_HAND]{};
    uint8_t dealer_card_count_ = 0;
    bool dealer_hidden_ = true;

    uint32_t bet_ = 10;
    blackjack_game::Bankroll bank_{};

    bool blink_state_ = true;
    uint32_t blink_counter_ = 0;
    uint32_t dealer_timer_ = 0;
    bool dims_ready_ = false;
    bool deck_ready_ = false;

    void ensure_ready();
    void shuffle_deck();
    uint8_t draw_from_deck();

    /* Off-screen focus holder; no on_select so Select reaches the view. */
    ui::Button dummy_{{240, 0, 0, 0}, ""};
};

}  // namespace app

#endif /*__MB200_UI_BLACKJACK_H__*/
