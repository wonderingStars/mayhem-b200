/*
 * mayhem-b200 — Random password generator (host port of Mayhem's
 * random_password app).
 *
 * SHA-512 modified from https://github.com/ulwanski/sha512 (@ulwanski).
 * Copyright (C) 2014 Jared Boone, ShareBrained Technology, Inc. (original)
 * Copyright (C) 2017 Furrtek / (C) 2024 zxkmm / (C) 2024 HTotoo
 * Copyright (C) 2026 mayhem-b200 contributors (host port)
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "ui_random_password.hpp"

#include "app_context.hpp"
#include "ui_navigation.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <random>

namespace app {
namespace rndpw {

namespace {

/* --- SHA-512 (self-contained, internal linkage) ------------------------------
 * Faithful transcription of the upstream sha512.{h,cpp}. Kept in an anonymous
 * namespace so it cannot clash with any other translation unit. */
class SHA512 {
   public:
    static const unsigned int DIGEST_SIZE = 512 / 8;

    void init();
    void update(const unsigned char* message, unsigned int len);
    void final(unsigned char* digest);

   private:
    using u64 = unsigned long long;
    using u8 = unsigned char;
    static const unsigned int BLOCK_SIZE = 1024 / 8;
    static const u64 k[80];

    void transform(const unsigned char* message, unsigned int block_nb);
    unsigned int m_tot_len{0};
    unsigned int m_len{0};
    unsigned char m_block[2 * BLOCK_SIZE]{};
    u64 m_h[8]{};
};

#define SHFR(x, n) ((x) >> (n))
#define ROTR(x, n) (((x) >> (n)) | ((x) << ((sizeof(x) << 3) - (n))))
#define CH(x, y, z) (((x) & (y)) ^ (~(x) & (z)))
#define MAJ(x, y, z) (((x) & (y)) ^ ((x) & (z)) ^ ((y) & (z)))
#define F1(x) (ROTR(x, 28) ^ ROTR(x, 34) ^ ROTR(x, 39))
#define F2(x) (ROTR(x, 14) ^ ROTR(x, 18) ^ ROTR(x, 41))
#define F3(x) (ROTR(x, 1) ^ ROTR(x, 8) ^ SHFR(x, 7))
#define F4(x) (ROTR(x, 19) ^ ROTR(x, 61) ^ SHFR(x, 6))
#define UNPACK32(x, str)                    \
    {                                       \
        *((str) + 3) = (u8)((x));           \
        *((str) + 2) = (u8)((x) >> 8);      \
        *((str) + 1) = (u8)((x) >> 16);     \
        *((str) + 0) = (u8)((x) >> 24);     \
    }
#define UNPACK64(x, str)                    \
    {                                       \
        *((str) + 7) = (u8)((x));           \
        *((str) + 6) = (u8)((x) >> 8);      \
        *((str) + 5) = (u8)((x) >> 16);     \
        *((str) + 4) = (u8)((x) >> 24);     \
        *((str) + 3) = (u8)((x) >> 32);     \
        *((str) + 2) = (u8)((x) >> 40);     \
        *((str) + 1) = (u8)((x) >> 48);     \
        *((str) + 0) = (u8)((x) >> 56);     \
    }
#define PACK64(str, x)                                                          \
    {                                                                          \
        *(x) = ((u64) * ((str) + 7)) | ((u64) * ((str) + 6) << 8) |            \
               ((u64) * ((str) + 5) << 16) | ((u64) * ((str) + 4) << 24) |     \
               ((u64) * ((str) + 3) << 32) | ((u64) * ((str) + 2) << 40) |     \
               ((u64) * ((str) + 1) << 48) | ((u64) * ((str) + 0) << 56);      \
    }

const SHA512::u64 SHA512::k[80] = {
    0x428a2f98d728ae22ULL, 0x7137449123ef65cdULL, 0xb5c0fbcfec4d3b2fULL, 0xe9b5dba58189dbbcULL,
    0x3956c25bf348b538ULL, 0x59f111f1b605d019ULL, 0x923f82a4af194f9bULL, 0xab1c5ed5da6d8118ULL,
    0xd807aa98a3030242ULL, 0x12835b0145706fbeULL, 0x243185be4ee4b28cULL, 0x550c7dc3d5ffb4e2ULL,
    0x72be5d74f27b896fULL, 0x80deb1fe3b1696b1ULL, 0x9bdc06a725c71235ULL, 0xc19bf174cf692694ULL,
    0xe49b69c19ef14ad2ULL, 0xefbe4786384f25e3ULL, 0x0fc19dc68b8cd5b5ULL, 0x240ca1cc77ac9c65ULL,
    0x2de92c6f592b0275ULL, 0x4a7484aa6ea6e483ULL, 0x5cb0a9dcbd41fbd4ULL, 0x76f988da831153b5ULL,
    0x983e5152ee66dfabULL, 0xa831c66d2db43210ULL, 0xb00327c898fb213fULL, 0xbf597fc7beef0ee4ULL,
    0xc6e00bf33da88fc2ULL, 0xd5a79147930aa725ULL, 0x06ca6351e003826fULL, 0x142929670a0e6e70ULL,
    0x27b70a8546d22ffcULL, 0x2e1b21385c26c926ULL, 0x4d2c6dfc5ac42aedULL, 0x53380d139d95b3dfULL,
    0x650a73548baf63deULL, 0x766a0abb3c77b2a8ULL, 0x81c2c92e47edaee6ULL, 0x92722c851482353bULL,
    0xa2bfe8a14cf10364ULL, 0xa81a664bbc423001ULL, 0xc24b8b70d0f89791ULL, 0xc76c51a30654be30ULL,
    0xd192e819d6ef5218ULL, 0xd69906245565a910ULL, 0xf40e35855771202aULL, 0x106aa07032bbd1b8ULL,
    0x19a4c116b8d2d0c8ULL, 0x1e376c085141ab53ULL, 0x2748774cdf8eeb99ULL, 0x34b0bcb5e19b48a8ULL,
    0x391c0cb3c5c95a63ULL, 0x4ed8aa4ae3418acbULL, 0x5b9cca4f7763e373ULL, 0x682e6ff3d6b2b8a3ULL,
    0x748f82ee5defb2fcULL, 0x78a5636f43172f60ULL, 0x84c87814a1f0ab72ULL, 0x8cc702081a6439ecULL,
    0x90befffa23631e28ULL, 0xa4506cebde82bde9ULL, 0xbef9a3f7b2c67915ULL, 0xc67178f2e372532bULL,
    0xca273eceea26619cULL, 0xd186b8c721c0c207ULL, 0xeada7dd6cde0eb1eULL, 0xf57d4f7fee6ed178ULL,
    0x06f067aa72176fbaULL, 0x0a637dc5a2c898a6ULL, 0x113f9804bef90daeULL, 0x1b710b35131c471bULL,
    0x28db77f523047d84ULL, 0x32caab7b40c72493ULL, 0x3c9ebe0a15c9bebcULL, 0x431d67c49c100d4cULL,
    0x4cc5d4becb3e42b6ULL, 0x597f299cfc657e2aULL, 0x5fcb6fab3ad6faecULL, 0x6c44198c4a475817ULL};

void SHA512::transform(const unsigned char* message, unsigned int block_nb) {
    u64 w[80];
    u64 wv[8];
    u64 t1, t2;
    const unsigned char* sub_block;
    for (int i = 0; i < (int)block_nb; i++) {
        sub_block = message + (i << 7);
        for (int j = 0; j < 16; j++) PACK64(&sub_block[j << 3], &w[j]);
        for (int j = 16; j < 80; j++)
            w[j] = F4(w[j - 2]) + w[j - 7] + F3(w[j - 15]) + w[j - 16];
        for (int j = 0; j < 8; j++) wv[j] = m_h[j];
        for (int j = 0; j < 80; j++) {
            t1 = wv[7] + F2(wv[4]) + CH(wv[4], wv[5], wv[6]) + k[j] + w[j];
            t2 = F1(wv[0]) + MAJ(wv[0], wv[1], wv[2]);
            wv[7] = wv[6];
            wv[6] = wv[5];
            wv[5] = wv[4];
            wv[4] = wv[3] + t1;
            wv[3] = wv[2];
            wv[2] = wv[1];
            wv[1] = wv[0];
            wv[0] = t1 + t2;
        }
        for (int j = 0; j < 8; j++) m_h[j] += wv[j];
    }
}

void SHA512::init() {
    m_h[0] = 0x6a09e667f3bcc908ULL;
    m_h[1] = 0xbb67ae8584caa73bULL;
    m_h[2] = 0x3c6ef372fe94f82bULL;
    m_h[3] = 0xa54ff53a5f1d36f1ULL;
    m_h[4] = 0x510e527fade682d1ULL;
    m_h[5] = 0x9b05688c2b3e6c1fULL;
    m_h[6] = 0x1f83d9abfb41bd6bULL;
    m_h[7] = 0x5be0cd19137e2179ULL;
    m_len = 0;
    m_tot_len = 0;
}

void SHA512::update(const unsigned char* message, unsigned int len) {
    unsigned int block_nb;
    unsigned int new_len, rem_len, tmp_len;
    const unsigned char* shifted_message;
    tmp_len = BLOCK_SIZE - m_len;
    rem_len = len < tmp_len ? len : tmp_len;
    memcpy(&m_block[m_len], message, rem_len);
    if (m_len + len < BLOCK_SIZE) {
        m_len += len;
        return;
    }
    new_len = len - rem_len;
    block_nb = new_len / BLOCK_SIZE;
    shifted_message = message + rem_len;
    transform(m_block, 1);
    transform(shifted_message, block_nb);
    rem_len = new_len % BLOCK_SIZE;
    memcpy(m_block, &shifted_message[block_nb << 7], rem_len);
    m_len = rem_len;
    m_tot_len += (block_nb + 1) << 7;
}

void SHA512::final(unsigned char* digest) {
    unsigned int block_nb;
    unsigned int pm_len;
    unsigned int len_b;
    block_nb = 1 + ((BLOCK_SIZE - 17) < (m_len % BLOCK_SIZE));
    len_b = (m_tot_len + m_len) << 3;
    pm_len = block_nb << 7;
    memset(m_block + m_len, 0, pm_len - m_len);
    m_block[m_len] = 0x80;
    UNPACK32(len_b, m_block + pm_len - 4);
    transform(m_block, block_nb);
    for (int i = 0; i < 8; i++) UNPACK64(m_h[i], &digest[i << 3]);
}

#undef SHFR
#undef ROTR
#undef CH
#undef MAJ
#undef F1
#undef F2
#undef F3
#undef F4
#undef UNPACK32
#undef UNPACK64
#undef PACK64

}  // namespace

std::string sha512_hex(const std::string& input) {
    unsigned char digest[SHA512::DIGEST_SIZE];
    memset(digest, 0, SHA512::DIGEST_SIZE);
    SHA512 ctx;
    ctx.init();
    ctx.update(reinterpret_cast<const unsigned char*>(input.data()),
               static_cast<unsigned int>(input.length()));
    ctx.final(digest);

    char buf[2 * SHA512::DIGEST_SIZE + 1];
    buf[2 * SHA512::DIGEST_SIZE] = 0;
    for (unsigned int i = 0; i < SHA512::DIGEST_SIZE; i++)
        std::snprintf(buf + i * 2, 3, "%02x", digest[i]);
    return std::string(buf);
}

std::string build_charset(const CharsetOptions& opts) {
    std::string charset;
    if (opts.digits) {
        charset += "23456789";
        if (opts.allow_confusable) charset += "01";
    }
    if (opts.latin_lower) {
        charset += "abcdefghijkmnpqrstuvwxyz";
        if (opts.allow_confusable) charset += "ol";
    }
    if (opts.latin_upper) {
        charset += "ABCDEFGHIJKLMNPQRSTUVWXYZ";
        if (opts.allow_confusable) charset += "O";
    }
    if (opts.punctuation) charset += ".,-!?";
    return charset;
}

std::string generate_password(const std::vector<unsigned int>& seeds,
                              const CharsetOptions& opts,
                              int length,
                              Method method) {
    if (length <= 0) return "";
    if (static_cast<int>(seeds.size()) < length * 2) return "";  // seed buffer not full

    const std::string charset = build_charset(opts);
    if (charset.empty()) return "";

    std::string initial_password;
    for (int i = 0; i < length * 2; i += 2) {
        unsigned int seed = seeds[i];
        std::srand(seed);
        uint8_t rollnum = static_cast<uint8_t>(seeds[i + 1] % 128);
        for (uint8_t o = 0; o < rollnum; ++o) (void)std::rand();
        char c = charset[std::rand() % charset.length()];
        initial_password += c;
    }

    if (method == Method::RollLCG) return initial_password;

    // SHA-512 whitening.
    const std::string hashed = sha512_hex(initial_password);
    std::string out(static_cast<size_t>(length), '?');
    for (int i = 0; i < length; i++) {
        unsigned int index =
            static_cast<unsigned int>(std::stoul(hashed.substr(i * 2, 2), nullptr, 16)) %
            static_cast<unsigned int>(charset.length());
        out[i] = charset[index];
    }
    return out;
}

}  // namespace rndpw

rndpw::CharsetOptions RandomPasswordView::current_options() const {
    rndpw::CharsetOptions opts;
    opts.digits = check_digits.value();
    opts.latin_lower = check_latin_lower.value();
    opts.latin_upper = check_latin_upper.value();
    opts.punctuation = check_punctuation.value();
    opts.allow_confusable = check_allow_confusable.value();
    return opts;
}

std::vector<unsigned int> RandomPasswordView::collect_seeds(int count) {
    /* Host entropy stand-in for the upstream off-air AFSK seed buffer. */
    std::random_device rd;
    std::vector<unsigned int> seeds(static_cast<size_t>(count));
    for (auto& s : seeds) s = rd();
    return seeds;
}

void RandomPasswordView::new_password() {
    const int length = field_digits.value();
    const auto opts = current_options();
    const auto method = static_cast<rndpw::Method>(field_method.selected_index_value());

    if (rndpw::build_charset(opts).empty()) {
        text_password.set("select at least 1 type");
        password_.clear();
        return;
    }

    auto seeds = collect_seeds(length * 2);
    password_ = rndpw::generate_password(seeds, opts, length, method);
    text_password.set(password_);
}

RandomPasswordView::RandomPasswordView() {
    add_children({&labels,
                  &text_note,
                  &field_digits,
                  &field_method,
                  &check_digits,
                  &check_latin_lower,
                  &check_latin_upper,
                  &check_punctuation,
                  &check_allow_confusable,
                  &label_result,
                  &text_password,
                  &button_generate,
                  &button_exit});

    check_digits.set_value(true);
    check_latin_lower.set_value(true);
    check_latin_upper.set_value(true);
    check_punctuation.set_value(true);
    check_allow_confusable.set_value(false);
    field_digits.set_value(16);
    field_method.set_by_value(static_cast<int32_t>(rndpw::Method::RollLCGHash));

    check_digits.on_select = [this](ui::Checkbox&, bool) { this->new_password(); };
    check_latin_lower.on_select = [this](ui::Checkbox&, bool) { this->new_password(); };
    check_latin_upper.on_select = [this](ui::Checkbox&, bool) { this->new_password(); };
    check_punctuation.on_select = [this](ui::Checkbox&, bool) { this->new_password(); };
    check_allow_confusable.on_select = [this](ui::Checkbox&, bool) { this->new_password(); };
    field_digits.on_change = [this](int32_t) { this->new_password(); };
    field_method.on_change = [this](size_t, ui::OptionsField::value_t) { this->new_password(); };

    button_generate.on_select = [this](ui::Button&) { this->new_password(); };
    button_exit.on_select = [](ui::Button&) {
        if (auto* nav = globals().nav) nav->pop();
    };
}

void RandomPasswordView::on_show() {
    View::on_show();
    button_generate.focus();
    new_password();
}

}  // namespace app

#include "app_registry.hpp"
#include "bitmaps.hpp"

namespace {
const app::Registrar reg_random_password{{"random_password", "Rand Pwd",
                                          app::Category::Utilities, ui::Color::yellow(),
                                          nullptr,
                                          [] { return std::make_unique<app::RandomPasswordView>(); }}};
}  // namespace
