/*
 * mayhem-b200 — Blackjack game.
 *
 * Copyright (C) 2024 RocketGod (original game)
 * Copyright (C) 2026 mayhem-b200 contributors (host implementation)
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "ui_blackjack.hpp"

#include "app_context.hpp"
#include "theme.hpp"
#include "ui_navigation.hpp"

#include <cstdlib>
#include <ctime>
#include <string>

namespace app {

namespace blackjack_game {

int calculate_hand_value(const uint8_t* cards, uint8_t count) {
    int value = 0;
    int aces = 0;

    for (uint8_t i = 0; i < count; i++) {
        int v = card_value(cards[i]);
        if (v == 1) {
            aces++;
            value += 1;
        } else {
            value += v;
        }
    }

    while (aces > 0 && value + 10 <= 21) {
        value += 10;
        aces--;
    }
    return value;
}

bool is_blackjack(const uint8_t* cards, uint8_t count) {
    return count == 2 && calculate_hand_value(cards, count) == 21;
}

bool dealer_should_hit(int dealer_value) {
    return dealer_value < 17;
}

Outcome resolve(int player_value, int dealer_value) {
    if (player_value > 21) return Outcome::PlayerBust;
    if (dealer_value > 21) return Outcome::DealerBust;
    if (player_value > dealer_value) return Outcome::PlayerWin;
    if (player_value < dealer_value) return Outcome::DealerWin;
    return Outcome::Push;
}

void init_ordered_deck(uint8_t deck[52]) {
    for (int i = 0; i < 52; i++) deck[i] = static_cast<uint8_t>(i);
}

int dealer_play(uint8_t* cards, uint8_t& count, const uint8_t* deck, uint8_t& pos) {
    int value = calculate_hand_value(cards, count);
    while (dealer_should_hit(value) && count < MAX_CARDS_IN_HAND) {
        cards[count++] = deck[pos++];
        value = calculate_hand_value(cards, count);
    }
    return value;
}

void apply_outcome(Bankroll& b, Outcome outcome, uint32_t bet) {
    switch (outcome) {
        case Outcome::PlayerBust:
        case Outcome::DealerWin:
            b.cash = (b.cash >= bet) ? b.cash - bet : 0;
            b.losses++;
            break;
        case Outcome::DealerBust:
        case Outcome::PlayerWin:
            b.cash += bet;
            b.wins++;
            break;
        case Outcome::Push:
            break;
    }
    if (b.cash > b.high_score) b.high_score = b.cash;
}

}  // namespace blackjack_game

/* ---- BlackjackView -------------------------------------------------------- */

using namespace blackjack_game;

BlackjackView::BlackjackView() {
    add_children({&dummy_});
}

void BlackjackView::focus() {
    dummy_.focus();
}

void BlackjackView::ensure_ready() {
    if (!dims_ready_) {
        const auto r = screen_rect();
        screen_w_ = r.width() > 0 ? r.width() : ui::screen_width;
        screen_h_ = r.height() > 0
                        ? r.height()
                        : (ui::screen_height - ui::SystemStatusView::status_height);
        if (screen_w_ <= 240) {
            card_width_ = 50;
            card_height_ = 65;
        } else if (screen_w_ >= 400) {
            card_width_ = 70;
            card_height_ = 90;
        } else {
            card_width_ = 60;
            card_height_ = 80;
        }
        dims_ready_ = true;
    }
    if (!deck_ready_) {
        std::srand(static_cast<unsigned>(std::time(nullptr)));
        init_ordered_deck(deck_);
        shuffle_deck();
        deck_ready_ = true;
    }
}

void BlackjackView::shuffle_deck() {
    for (int i = 51; i > 0; i--) {
        int j = std::rand() % (i + 1);
        uint8_t tmp = deck_[i];
        deck_[i] = deck_[j];
        deck_[j] = tmp;
    }
    deck_position_ = 0;
}

uint8_t BlackjackView::draw_from_deck() {
    if (deck_position_ >= 52) shuffle_deck();
    return deck_[deck_position_++];
}

void BlackjackView::on_show() {
    ui::View::on_show();
    ensure_ready();
    game_state_ = GameState::Menu;
    blink_state_ = true;
    blink_counter_ = 0;
    set_dirty();
}

void BlackjackView::deal_cards() {
    player_card_count_ = 0;
    dealer_card_count_ = 0;
    player_cards_[player_card_count_++] = draw_from_deck();
    dealer_cards_[dealer_card_count_++] = draw_from_deck();
    player_cards_[player_card_count_++] = draw_from_deck();
    dealer_cards_[dealer_card_count_++] = draw_from_deck();
    dealer_hidden_ = true;
    dealer_timer_ = 0;
    game_state_ = GameState::Playing;
    set_dirty();
}

void BlackjackView::player_hit() {
    if (player_card_count_ < MAX_CARDS_IN_HAND) {
        player_cards_[player_card_count_++] = draw_from_deck();
        int value = calculate_hand_value(player_cards_, player_card_count_);
        if (value > 21) {
            apply_outcome(bank_, Outcome::PlayerBust, bet_);
            game_state_ = GameState::GameOver;
        }
    }
    set_dirty();
}

void BlackjackView::player_stay() {
    dealer_hidden_ = false;
    game_state_ = GameState::DealerTurn;
    dealer_timer_ = 0;
    set_dirty();
}

void BlackjackView::dealer_step() {
    int dealer_value = calculate_hand_value(dealer_cards_, dealer_card_count_);
    if (dealer_should_hit(dealer_value) && dealer_card_count_ < MAX_CARDS_IN_HAND) {
        dealer_cards_[dealer_card_count_++] = draw_from_deck();
    } else {
        resolve_and_settle();
    }
    set_dirty();
}

void BlackjackView::resolve_and_settle() {
    int player_value = calculate_hand_value(player_cards_, player_card_count_);
    int dealer_value = calculate_hand_value(dealer_cards_, dealer_card_count_);
    Outcome outcome = resolve(player_value, dealer_value);
    apply_outcome(bank_, outcome, bet_);
    game_state_ = GameState::GameOver;
}

void BlackjackView::on_frame_sync() {
    ensure_ready();

    switch (game_state_) {
        case GameState::Menu:
            if (++blink_counter_ >= 30) {
                blink_counter_ = 0;
                blink_state_ = !blink_state_;
                set_dirty();
            }
            break;
        case GameState::DealerTurn:
            if (++dealer_timer_ >= 60) {
                dealer_timer_ = 0;
                dealer_step();
            }
            break;
        default:
            break;
    }

    ui::View::on_frame_sync();
}

bool BlackjackView::on_key(const ui::KeyEvent key) {
    if (key == ui::KeyEvent::Select) {
        switch (game_state_) {
            case GameState::Menu:
                if (bank_.cash < 10) {
                    bank_.cash = 100;
                    bank_.wins = 0;
                    bank_.losses = 0;
                }
                game_state_ = GameState::Betting;
                set_dirty();
                break;
            case GameState::Betting:
                deal_cards();
                break;
            case GameState::Playing:
                player_hit();
                break;
            case GameState::GameOver:
                if (bank_.cash >= bet_) {
                    deal_cards();
                } else if (bank_.cash >= 10) {
                    bet_ = 10;
                    deal_cards();
                } else {
                    bank_.cash = 100;
                    bank_.wins = 0;
                    bank_.losses = 0;
                    game_state_ = GameState::Menu;
                    set_dirty();
                }
                break;
            case GameState::Stats:
                game_state_ = GameState::Menu;
                set_dirty();
                break;
            default:
                break;
        }
        return true;
    }

    if (key == ui::KeyEvent::Left) {
        if (game_state_ == GameState::Menu) {
            game_state_ = GameState::Stats;
            set_dirty();
        }
        return true;
    }

    if (key == ui::KeyEvent::Right) {
        if (game_state_ == GameState::Menu) {
            if (auto* nav = globals().nav) nav->pop();
            return true;
        }
        if (game_state_ == GameState::Playing) {
            player_stay();
            return true;
        }
    }

    return false;
}

bool BlackjackView::on_encoder(const ui::EncoderEvent delta) {
    if (game_state_ == GameState::Betting || game_state_ == GameState::GameOver) {
        if (delta > 0 && bet_ + 10 <= bank_.cash) {
            bet_ += 10;
        } else if (delta < 0 && bet_ >= 20) {
            bet_ -= 10;
        }
        set_dirty();
        return true;
    }
    return false;
}

/* ---- rendering ------------------------------------------------------------ */

void BlackjackView::paint(ui::Painter& painter) {
    ensure_ready();
    const ui::Point o = screen_pos();
    painter.fill_rectangle({o.x(), o.y(), screen_w_, screen_h_}, ui::Color::black());

    switch (game_state_) {
        case GameState::Menu:       paint_menu(painter, o); break;
        case GameState::Stats:      paint_stats(painter, o); break;
        case GameState::Betting:    paint_betting(painter, o); break;
        case GameState::Playing:
        case GameState::DealerTurn:
        case GameState::GameOver:   paint_game(painter, o); break;
    }
}

void BlackjackView::paint_menu(ui::Painter& painter, ui::Point o) {
    const auto* title = ui::Theme::getInstance()->fg_green;
    const auto* text = ui::Theme::getInstance()->fg_light;
    const auto* rules = ui::Theme::getInstance()->fg_cyan;
    const auto* yellow = ui::Theme::getInstance()->fg_yellow;

    auto cx = [&](int chars) { return o.x() + (screen_w_ / 2) - (chars * 8 / 2); };

    painter.draw_string({cx(9), o.y() + 20}, *title, "BLACKJACK");

    int ry = (screen_h_ > 300) ? 55 : 45;
    painter.draw_string({cx(11), o.y() + ry}, *rules, "-- RULES --");
    painter.draw_string({cx(15), o.y() + ry + 20}, *text, "Get close to 21");
    painter.draw_string({cx(17), o.y() + ry + 35}, *text, "without going over");
    painter.draw_string({cx(17), o.y() + ry + 55}, *text, "Dealer hits on 16");
    painter.draw_string({cx(18), o.y() + ry + 70}, *text, "Dealer stays on 17");
    painter.draw_string({cx(18), o.y() + ry + 90}, *text, "Blackjack pays 1:1");

    int cy = (screen_h_ > 300) ? 175 : 155;
    painter.draw_string({cx(14), o.y() + cy}, *rules, "-- CONTROLS --");
    painter.draw_string({cx(17), o.y() + cy + 20}, *text, "SELECT: Start/Hit");
    painter.draw_string({cx(11), o.y() + cy + 35}, *text, "LEFT: Stats");
    painter.draw_string({cx(16), o.y() + cy + 50}, *text, "RIGHT: Exit/Stay");

    std::string hs = "High Score: $" + std::to_string(bank_.high_score);
    painter.draw_string({cx(static_cast<int>(hs.size())), o.y() + screen_h_ - 70}, *text, hs);

    if (blink_state_)
        painter.draw_string({cx(16), o.y() + screen_h_ - 40}, *yellow, "* PRESS SELECT *");
}

void BlackjackView::paint_stats(ui::Painter& painter, ui::Point o) {
    const auto* title = ui::Theme::getInstance()->fg_green;
    const auto* text = ui::Theme::getInstance()->fg_light;
    const auto* value = ui::Theme::getInstance()->fg_yellow;

    auto cx = [&](int chars) { return o.x() + (screen_w_ / 2) - (chars * 8 / 2); };
    int sx = o.x() + ((screen_w_ > 300) ? 50 : 30);

    painter.draw_string({cx(10), o.y() + 30}, *title, "STATISTICS");

    painter.draw_string({sx, o.y() + 80}, *text, "Wins:");
    painter.draw_string({cx(5), o.y() + 80}, *value, std::to_string(bank_.wins));

    painter.draw_string({sx, o.y() + 100}, *text, "Losses:");
    painter.draw_string({cx(5), o.y() + 100}, *value, std::to_string(bank_.losses));

    uint32_t total = bank_.wins + bank_.losses;
    if (total > 0) {
        uint32_t pct = (bank_.wins * 100) / total;
        painter.draw_string({sx, o.y() + 120}, *text, "Win %:");
        painter.draw_string({cx(5), o.y() + 120}, *value, std::to_string(pct) + "%");
    }

    painter.draw_string({sx, o.y() + 160}, *text, "High Score:");
    painter.draw_string({cx(5), o.y() + 160}, *value, "$" + std::to_string(bank_.high_score));

    painter.draw_string({sx, o.y() + 180}, *text, "Cash:");
    painter.draw_string({cx(5), o.y() + 180}, *value, "$" + std::to_string(bank_.cash));

    painter.draw_string({cx(11), o.y() + screen_h_ - 70}, *text, "SELECT: Back");
}

void BlackjackView::paint_betting(ui::Painter& painter, ui::Point o) {
    const auto* title = ui::Theme::getInstance()->fg_green;
    const auto* text = ui::Theme::getInstance()->fg_light;
    const auto* yellow = ui::Theme::getInstance()->fg_yellow;

    auto cx = [&](int chars) { return o.x() + (screen_w_ / 2) - (chars * 8 / 2); };

    painter.draw_string({cx(9), o.y() + 40}, *title, "PLACE BET");
    painter.draw_string({o.x() + 30, o.y() + 80}, *text, "Cash: $" + std::to_string(bank_.cash));
    painter.draw_string({o.x() + 30, o.y() + 110}, *yellow, "Bet: $" + std::to_string(bet_));
    painter.draw_string({o.x() + 30, o.y() + 140}, *text, "ENCODER: +/- $10");
    painter.draw_string({o.x() + 30, o.y() + 160}, *text, "SELECT: Deal");
}

void BlackjackView::paint_game(ui::Painter& painter, ui::Point o) {
    const auto* green = ui::Theme::getInstance()->fg_green;
    const auto* light = ui::Theme::getInstance()->fg_light;
    const auto* yellow = ui::Theme::getInstance()->fg_yellow;

    painter.draw_string({o.x() + 10, o.y() + 8}, *green, "Cash: $" + std::to_string(bank_.cash));
    std::string bettxt = "Bet: $" + std::to_string(bet_);
    painter.draw_string({o.x() + screen_w_ - 80, o.y() + 8}, *green, bettxt);

    /* Dealer */
    painter.draw_string({o.x() + 10, o.y() + 40}, *light, "Dealer:");
    if (!dealer_hidden_ || game_state_ == GameState::GameOver) {
        int dv = calculate_hand_value(dealer_cards_, dealer_card_count_);
        painter.draw_string({o.x() + 70, o.y() + 40}, *yellow, "(" + std::to_string(dv) + ")");
    }
    draw_hand(painter, o.x() + 10, o.y() + 60, dealer_cards_, dealer_card_count_, true);

    /* Player */
    int py = (screen_h_ > 300) ? 165 : 145;
    painter.draw_string({o.x() + 10, o.y() + py}, *light, "You:");
    int pv = calculate_hand_value(player_cards_, player_card_count_);
    painter.draw_string({o.x() + 50, o.y() + py}, *yellow, "(" + std::to_string(pv) + ")");
    draw_hand(painter, o.x() + 10, o.y() + py + 20, player_cards_, player_card_count_, false);

    int cy = o.y() + screen_h_ - 50;
    auto cx = [&](int chars) { return o.x() + (screen_w_ / 2) - (chars * 8 / 2); };

    if (game_state_ == GameState::Playing) {
        painter.draw_string({o.x() + 30, cy}, *light, "SELECT: Hit");
        painter.draw_string({o.x() + screen_w_ - 100, cy}, *light, "RIGHT: Stay");
    } else if (game_state_ == GameState::GameOver) {
        int dv = calculate_hand_value(dealer_cards_, dealer_card_count_);
        const ui::Style* rs = ui::Theme::getInstance()->fg_yellow;
        std::string result;
        if (pv > 21) {
            result = "BUST! You Lose";
            rs = ui::Theme::getInstance()->fg_red;
        } else if (dv > 21) {
            result = "Dealer Bust! Win!";
            rs = ui::Theme::getInstance()->fg_green;
        } else if (pv > dv) {
            result = "You Win!";
            rs = ui::Theme::getInstance()->fg_green;
        } else if (pv < dv) {
            result = "You Lose";
            rs = ui::Theme::getInstance()->fg_red;
        } else {
            result = "Push (Tie)";
        }
        painter.draw_string({cx(static_cast<int>(result.size())), cy - 20}, *rs, result);
        painter.draw_string({o.x() + screen_w_ - 100, o.y() + 24},
                            *ui::Theme::getInstance()->fg_cyan, "Next: $" + std::to_string(bet_));
        painter.draw_string({o.x() + 10, cy}, *light, "SELECT: Deal  ENC:+/-");
    }
}

void BlackjackView::draw_hand(ui::Painter& painter, int x, int y, const uint8_t* cards,
                              uint8_t count, bool is_dealer) {
    const int overlap = card_width_ * 2 / 3;
    const int max_width = (screen_pos().x() + screen_w_) - 10 - x;

    int spacing;
    if (count <= 1) {
        spacing = 0;
    } else if (count == 2) {
        spacing = card_width_ + 5;
    } else {
        int total = card_width_ + (count - 1) * overlap;
        spacing = (total <= max_width) ? overlap : (max_width - card_width_) / (count - 1);
    }

    for (uint8_t i = 0; i < count; i++) {
        bool hide = is_dealer && dealer_hidden_ && i == 1;
        draw_card(painter, x + i * spacing, y, cards[i], hide);
    }
}

void BlackjackView::draw_card(ui::Painter& painter, int x, int y, uint8_t card, bool hidden) {
    painter.fill_rectangle({x, y, card_width_, card_height_}, ui::Color::white());
    painter.draw_rectangle({x, y, card_width_, card_height_}, ui::Color::black());
    painter.draw_rectangle({x + 1, y + 1, card_width_ - 2, card_height_ - 2}, ui::Color::grey());

    if (hidden) {
        for (int i = 4; i < card_width_ - 4; i += 6)
            for (int j = 4; j < card_height_ - 4; j += 6) {
                painter.fill_rectangle({x + i, y + j, 3, 3}, ui::Color::blue());
                painter.fill_rectangle({x + i + 3, y + j + 3, 3, 3}, ui::Color::red());
            }
        return;
    }

    uint8_t suit = card_suit(card);
    ui::Color suit_color = (suit == 1 || suit == 2) ? ui::Color::red() : ui::Color::black();

    const auto* base = ui::Theme::getInstance()->fg_light;
    ui::Style card_style{
        .font = base->font,
        .background = ui::Color::white(),
        .foreground = suit_color};

    int rank = card_rank(card);
    std::string vs = (rank == 1)    ? "A"
                     : (rank == 11) ? "J"
                     : (rank == 12) ? "Q"
                     : (rank == 13) ? "K"
                     : (rank == 10) ? "10"
                                    : std::to_string(rank);

    painter.draw_string({x + 4, y + 4}, card_style, vs);

    /* Small suit pip next to the top-left value. */
    int sx = (vs == "10") ? x + 20 : x + 12;
    auto pip = [&](int px, int py) {
        ui::Color c = suit_color;
        switch (suit) {
            case 0:  /* Spades */
                painter.fill_rectangle({px + 2, py + 1, 3, 3}, c);
                painter.fill_rectangle({px + 1, py + 3, 5, 2}, c);
                painter.fill_rectangle({px + 3, py + 5, 1, 2}, c);
                break;
            case 1:  /* Hearts */
                painter.fill_rectangle({px + 1, py + 1, 2, 2}, c);
                painter.fill_rectangle({px + 4, py + 1, 2, 2}, c);
                painter.fill_rectangle({px + 1, py + 2, 5, 2}, c);
                painter.fill_rectangle({px + 2, py + 4, 3, 1}, c);
                painter.fill_rectangle({px + 3, py + 5, 1, 1}, c);
                break;
            case 2:  /* Diamonds */
                painter.fill_rectangle({px + 3, py + 1, 1, 1}, c);
                painter.fill_rectangle({px + 2, py + 2, 3, 1}, c);
                painter.fill_rectangle({px + 1, py + 3, 5, 1}, c);
                painter.fill_rectangle({px + 2, py + 4, 3, 1}, c);
                painter.fill_rectangle({px + 3, py + 5, 1, 1}, c);
                break;
            default:  /* Clubs */
                painter.fill_rectangle({px + 3, py + 1, 2, 2}, c);
                painter.fill_rectangle({px + 1, py + 3, 2, 2}, c);
                painter.fill_rectangle({px + 4, py + 3, 2, 2}, c);
                painter.fill_rectangle({px + 3, py + 5, 2, 2}, c);
                break;
        }
    };
    pip(sx, y + 4);

    int bottom_x = (vs == "10") ? x + card_width_ - 24 : x + card_width_ - 16;
    painter.draw_string({bottom_x, y + card_height_ - 18}, card_style, vs);
    pip(x + card_width_ - 10, y + card_height_ - 16);
}

}  // namespace app

#include "app_registry.hpp"
#include "bitmaps.hpp"

namespace {
const app::Registrar reg_blackjack{{"blackjack", "Blackjack",
                                    app::Category::Games, ui::Color::green(),
                                    &ui::bitmap_icon_games,
                                    [] { return std::make_unique<app::BlackjackView>(); },
                                    false}};
}  // namespace
