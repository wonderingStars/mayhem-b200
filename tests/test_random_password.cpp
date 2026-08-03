/*
 * mayhem-b200 — tests for the random password generator (ui_random_password).
 *
 * SHA-512 is checked against the canonical FIPS 180-4 test vectors. Charsets are
 * checked against the exact alphabets upstream builds. Generation is exercised
 * with a fixed (deterministic) seed vector, checking that output honours the
 * requested length and stays within the selected alphabet, and is reproducible.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "test_main.hpp"
#include "ui_random_password.hpp"

#include <vector>

using namespace mb200test;
using namespace app::rndpw;

/* A fixed, deterministic seed vector: two seeds per output character. */
static std::vector<unsigned int> fixed_seeds(int count) {
    std::vector<unsigned int> v(static_cast<size_t>(count));
    unsigned int x = 0x12345678u;
    for (auto& s : v) {
        x = x * 1103515245u + 12345u;  // a plain LCG, just to vary the seeds
        s = x;
    }
    return v;
}

static bool all_in(const std::string& s, const std::string& charset) {
    for (char c : s)
        if (charset.find(c) == std::string::npos) return false;
    return true;
}

TEST(rndpw_sha512_empty) {
    CHECK_STR_EQ(sha512_hex(""),
                 "cf83e1357eefb8bdf1542850d66d8007d620e4050b5715dc83f4a921d36ce9ce"
                 "47d0d13c5d85f2b0ff8318d2877eec2f63b931bd47417a81a538327af927da3e");
}

TEST(rndpw_sha512_abc) {
    CHECK_STR_EQ(sha512_hex("abc"),
                 "ddaf35a193617abacc417349ae20413112e6fa4e89a97ea20a9eeee64b55d39a"
                 "2192992a274fc1a836ba3c23a3feebbd454d4423643ce80e2a9ac94fa54ca49f");
}

TEST(rndpw_sha512_long) {
    /* > 128 bytes so more than one 1024-bit block and the length padding are
     * exercised. FIPS 180-4 two-block message. */
    CHECK_STR_EQ(
        sha512_hex("abcdefghbcdefghicdefghijdefghijkefghijklfghijklmghijklmn"
                   "hijklmnoijklmnopjklmnopqklmnopqrlmnopqrsmnopqrstnopqrstu"),
        "8e959b75dae313da8cf4f72814fc143f8f7779c6eb9f7fa17299aeadb6889018"
        "501d289e4900f7e4331b99dec4b5433ac7d329eeb6dd26545e96e55b874be909");
}

TEST(rndpw_charset_digits_only) {
    CharsetOptions o{};
    o.digits = true;
    o.latin_lower = o.latin_upper = o.punctuation = o.allow_confusable = false;
    CHECK_STR_EQ(build_charset(o), "23456789");
}

TEST(rndpw_charset_digits_confusable) {
    CharsetOptions o{};
    o.digits = true;
    o.latin_lower = o.latin_upper = o.punctuation = false;
    o.allow_confusable = true;
    CHECK_STR_EQ(build_charset(o), "2345678901");
}

TEST(rndpw_charset_lower) {
    CharsetOptions o{};
    o.latin_lower = true;
    o.digits = o.latin_upper = o.punctuation = o.allow_confusable = false;
    CHECK_STR_EQ(build_charset(o), "abcdefghijkmnpqrstuvwxyz");  // no l, no o
    CHECK_EQ(build_charset(o).length(), (size_t)24);
}

TEST(rndpw_charset_upper) {
    CharsetOptions o{};
    o.latin_upper = true;
    o.digits = o.latin_lower = o.punctuation = o.allow_confusable = false;
    CHECK_STR_EQ(build_charset(o), "ABCDEFGHIJKLMNPQRSTUVWXYZ");  // no O
    CHECK_EQ(build_charset(o).length(), (size_t)25);
}

TEST(rndpw_charset_punct) {
    CharsetOptions o{};
    o.punctuation = true;
    o.digits = o.latin_lower = o.latin_upper = o.allow_confusable = false;
    CHECK_STR_EQ(build_charset(o), ".,-!?");
}

TEST(rndpw_charset_all) {
    CharsetOptions o{};  // digits/lower/upper/punct default true, confusable false
    CHECK_EQ(build_charset(o).length(), (size_t)62);
}

TEST(rndpw_charset_all_confusable) {
    CharsetOptions o{};
    o.allow_confusable = true;
    CHECK_EQ(build_charset(o).length(), (size_t)67);  // +2 +2 +1
}

TEST(rndpw_charset_none_empty) {
    CharsetOptions o{};
    o.digits = o.latin_lower = o.latin_upper = o.punctuation = o.allow_confusable = false;
    CHECK(build_charset(o).empty());
}

TEST(rndpw_generate_length_and_alphabet_roll) {
    CharsetOptions o{};  // all default types
    const int len = 16;
    auto seeds = fixed_seeds(len * 2);
    auto pw = generate_password(seeds, o, len, Method::RollLCG);
    CHECK_EQ(pw.length(), (size_t)len);
    CHECK(all_in(pw, build_charset(o)));
}

TEST(rndpw_generate_length_and_alphabet_hash) {
    CharsetOptions o{};
    const int len = 16;
    auto seeds = fixed_seeds(len * 2);
    auto pw = generate_password(seeds, o, len, Method::RollLCGHash);
    CHECK_EQ(pw.length(), (size_t)len);
    CHECK(all_in(pw, build_charset(o)));
}

TEST(rndpw_generate_digits_only_alphabet) {
    CharsetOptions o{};
    o.digits = true;
    o.latin_lower = o.latin_upper = o.punctuation = o.allow_confusable = false;
    const int len = 20;
    auto seeds = fixed_seeds(len * 2);
    auto pw = generate_password(seeds, o, len, Method::RollLCGHash);
    CHECK_EQ(pw.length(), (size_t)len);
    CHECK(all_in(pw, "23456789"));
}

TEST(rndpw_generate_deterministic) {
    CharsetOptions o{};
    const int len = 12;
    auto seeds = fixed_seeds(len * 2);
    auto a = generate_password(seeds, o, len, Method::RollLCGHash);
    auto b = generate_password(seeds, o, len, Method::RollLCGHash);
    CHECK_STR_EQ(a, b);
    auto c = generate_password(seeds, o, len, Method::RollLCG);
    auto d = generate_password(seeds, o, len, Method::RollLCG);
    CHECK_STR_EQ(c, d);
}

TEST(rndpw_generate_boundary_length_one) {
    CharsetOptions o{};
    auto seeds = fixed_seeds(2);
    auto pw = generate_password(seeds, o, 1, Method::RollLCGHash);
    CHECK_EQ(pw.length(), (size_t)1);
    CHECK(all_in(pw, build_charset(o)));
}

TEST(rndpw_generate_max_length) {
    CharsetOptions o{};
    const int len = kMaxDigits;  // 30
    auto seeds = fixed_seeds(len * 2);
    auto pw = generate_password(seeds, o, len, Method::RollLCGHash);
    CHECK_EQ(pw.length(), (size_t)len);
    CHECK(all_in(pw, build_charset(o)));
}

TEST(rndpw_generate_empty_charset_fails) {
    CharsetOptions o{};
    o.digits = o.latin_lower = o.latin_upper = o.punctuation = o.allow_confusable = false;
    auto seeds = fixed_seeds(32);
    CHECK(generate_password(seeds, o, 16, Method::RollLCG).empty());
}

TEST(rndpw_generate_insufficient_seeds_fails) {
    CharsetOptions o{};
    auto seeds = fixed_seeds(10);  // need 32 for length 16
    CHECK(generate_password(seeds, o, 16, Method::RollLCG).empty());
}

TEST(rndpw_generate_zero_length_fails) {
    CharsetOptions o{};
    auto seeds = fixed_seeds(4);
    CHECK(generate_password(seeds, o, 0, Method::RollLCG).empty());
}
