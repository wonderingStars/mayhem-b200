/*
 * mayhem-b200 — Blackjack card-logic tests.
 *
 * Expected values follow the rules upstream implements: hand totals with each
 * ace counted 1 or 11 (soft hands), two-card 21 = blackjack (paid 1:1, no
 * natural bonus), the dealer hitting below 17 and standing on 17 (soft 17
 * included), and win/lose/push resolution in upstream's precedence order. Cards
 * are the 0..51 indices upstream uses (rank = card%13+1, suit = card/13).
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "test_main.hpp"

#include "ui_blackjack.hpp"

using namespace app::blackjack_game;
using namespace mb200test;

/* ---- card decoding ------------------------------------------------------- */

TEST(bj_card_rank_value_suit) {
    CHECK_EQ(card_rank(card_of(1)), 1);       /* Ace   */
    CHECK_EQ(card_value(card_of(1)), 1);
    CHECK_EQ(card_value(card_of(10)), 10);
    CHECK_EQ(card_value(card_of(11)), 10);    /* Jack  */
    CHECK_EQ(card_value(card_of(12)), 10);    /* Queen */
    CHECK_EQ(card_value(card_of(13)), 10);    /* King  */
    CHECK_EQ(card_value(card_of(9)), 9);

    CHECK_EQ(static_cast<int>(card_suit(card_of(1, 0))), 0);
    CHECK_EQ(static_cast<int>(card_suit(card_of(1, 1))), 1);
    CHECK_EQ(static_cast<int>(card_suit(card_of(1, 3))), 3);
}

/* ---- hand value: aces as 1 or 11 ----------------------------------------- */

TEST(bj_value_no_aces) {
    const uint8_t h[] = {card_of(13), card_of(12)};  /* K + Q */
    CHECK_EQ(calculate_hand_value(h, 2), 20);
}

TEST(bj_value_ace_as_eleven) {
    const uint8_t h[] = {card_of(1), card_of(13)};   /* A + K = 21 */
    CHECK_EQ(calculate_hand_value(h, 2), 21);

    const uint8_t h2[] = {card_of(1), card_of(9)};   /* A + 9 = 20 */
    CHECK_EQ(calculate_hand_value(h2, 2), 20);
}

TEST(bj_value_multiple_aces) {
    const uint8_t aa[] = {card_of(1), card_of(1)};             /* 11 + 1 = 12 */
    CHECK_EQ(calculate_hand_value(aa, 2), 12);

    const uint8_t aa9[] = {card_of(1), card_of(1), card_of(9)};/* 11 + 1 + 9 = 21 */
    CHECK_EQ(calculate_hand_value(aa9, 3), 21);
}

TEST(bj_value_ace_demoted_when_needed) {
    const uint8_t soft[] = {card_of(1), card_of(6)};              /* soft 17 */
    CHECK_EQ(calculate_hand_value(soft, 2), 17);

    const uint8_t hard[] = {card_of(1), card_of(6), card_of(13)}; /* A+6+K = 17 hard */
    CHECK_EQ(calculate_hand_value(hard, 3), 17);

    const uint8_t a77[] = {card_of(1), card_of(7), card_of(7)};   /* 1+7+7 = 15 */
    CHECK_EQ(calculate_hand_value(a77, 3), 15);
}

TEST(bj_value_bust_and_exact) {
    const uint8_t bust[] = {card_of(13), card_of(12), card_of(5)};      /* 25 */
    CHECK_EQ(calculate_hand_value(bust, 3), 25);

    const uint8_t five5[] = {card_of(5), card_of(5), card_of(5), card_of(6)};  /* 21 */
    CHECK_EQ(calculate_hand_value(five5, 4), 21);
}

/* ---- blackjack detection ------------------------------------------------- */

TEST(bj_is_blackjack) {
    const uint8_t ak[] = {card_of(1), card_of(13)};
    CHECK(is_blackjack(ak, 2));

    const uint8_t aq[] = {card_of(1), card_of(12)};
    CHECK(is_blackjack(aq, 2));

    const uint8_t a9[] = {card_of(1), card_of(9)};   /* 20, not blackjack */
    CHECK(!is_blackjack(a9, 2));

    const uint8_t three21[] = {card_of(5), card_of(5), card_of(5), card_of(6)};  /* 21 in 4 */
    CHECK(!is_blackjack(three21, 4));
}

/* ---- dealer stands on 17 ------------------------------------------------- */

TEST(bj_dealer_should_hit_boundary) {
    CHECK(dealer_should_hit(16));
    CHECK(!dealer_should_hit(17));
    CHECK(!dealer_should_hit(21));
    CHECK(dealer_should_hit(0));
}

TEST(bj_dealer_stands_on_hard_17) {
    uint8_t hand[MAX_CARDS_IN_HAND] = {card_of(10), card_of(7)};  /* 17 */
    uint8_t count = 2;
    uint8_t deck[52];
    init_ordered_deck(deck);
    uint8_t pos = 0;

    const int final_value = dealer_play(hand, count, deck, pos);
    CHECK_EQ(final_value, 17);
    CHECK_EQ(static_cast<int>(count), 2);  /* drew nothing */
    CHECK_EQ(static_cast<int>(pos), 0);
}

TEST(bj_dealer_stands_on_soft_17) {
    uint8_t hand[MAX_CARDS_IN_HAND] = {card_of(1), card_of(6)};   /* soft 17 */
    uint8_t count = 2;
    uint8_t deck[52];
    init_ordered_deck(deck);
    uint8_t pos = 0;

    const int final_value = dealer_play(hand, count, deck, pos);
    CHECK_EQ(final_value, 17);
    CHECK_EQ(static_cast<int>(count), 2);  /* stands on soft 17, no hit */
}

TEST(bj_dealer_hits_16_then_stands) {
    uint8_t hand[MAX_CARDS_IN_HAND] = {card_of(10), card_of(6)};  /* 16 */
    uint8_t count = 2;
    uint8_t deck[3] = {card_of(5), card_of(9), card_of(9)};       /* next card = 5 -> 21 */
    uint8_t pos = 0;

    const int final_value = dealer_play(hand, count, deck, pos);
    CHECK_EQ(final_value, 21);
    CHECK_EQ(static_cast<int>(count), 3);  /* drew exactly one */
    CHECK_EQ(static_cast<int>(pos), 1);
}

TEST(bj_dealer_hits_multiple_times) {
    uint8_t hand[MAX_CARDS_IN_HAND] = {card_of(2), card_of(3)};   /* 5 */
    uint8_t count = 2;
    uint8_t deck[3] = {card_of(4), card_of(5), card_of(6)};       /* 5->9->14->20 */
    uint8_t pos = 0;

    const int final_value = dealer_play(hand, count, deck, pos);
    CHECK_EQ(final_value, 20);
    CHECK_EQ(static_cast<int>(count), 5);
    CHECK_EQ(static_cast<int>(pos), 3);
}

/* ---- outcome resolution -------------------------------------------------- */

TEST(bj_resolve_outcomes) {
    CHECK(resolve(25, 18) == Outcome::PlayerBust);
    CHECK(resolve(20, 25) == Outcome::DealerBust);
    CHECK(resolve(20, 18) == Outcome::PlayerWin);
    CHECK(resolve(18, 20) == Outcome::DealerWin);
    CHECK(resolve(20, 20) == Outcome::Push);
    CHECK(resolve(21, 21) == Outcome::Push);       /* no natural bonus */
    CHECK(resolve(25, 25) == Outcome::PlayerBust); /* player checked first */
}

/* ---- bankroll accounting ------------------------------------------------- */

TEST(bj_bankroll_win_and_loss) {
    Bankroll b;  /* {100, 0, 0, 100} */
    apply_outcome(b, Outcome::PlayerWin, 10);
    CHECK_EQ(b.cash, 110u);
    CHECK_EQ(b.wins, 1u);
    CHECK_EQ(b.high_score, 110u);

    Bankroll d;
    apply_outcome(d, Outcome::DealerWin, 10);
    CHECK_EQ(d.cash, 90u);
    CHECK_EQ(d.losses, 1u);
    CHECK_EQ(d.high_score, 100u);  /* only rises */
}

TEST(bj_bankroll_bust_dealerbust_push) {
    Bankroll bust;
    apply_outcome(bust, Outcome::PlayerBust, 25);
    CHECK_EQ(bust.cash, 75u);
    CHECK_EQ(bust.losses, 1u);

    Bankroll db;
    apply_outcome(db, Outcome::DealerBust, 50);
    CHECK_EQ(db.cash, 150u);
    CHECK_EQ(db.wins, 1u);
    CHECK_EQ(db.high_score, 150u);

    Bankroll push;
    apply_outcome(push, Outcome::Push, 30);
    CHECK_EQ(push.cash, 100u);
    CHECK_EQ(push.wins, 0u);
    CHECK_EQ(push.losses, 0u);
}

TEST(bj_bankroll_cash_floors_at_zero) {
    Bankroll b;
    b.cash = 5;
    apply_outcome(b, Outcome::DealerWin, 10);  /* can't afford the bet */
    CHECK_EQ(b.cash, 0u);
    CHECK_EQ(b.losses, 1u);
}
