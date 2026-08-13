/*
 * mayhem-b200 — OOK Editor tests.
 *
 * The hardware-free, load-bearing parts of the OOK Editor are the .OOK file
 * format (which must stay byte-compatible with the PortaPack so a waveform saved
 * on either side loads on the other) and the MSB-first payload packing. The
 * format is asserted against the layout documented in ook_file.cpp:
 *
 *     Frequency SampleRate SymbolRate Repeat PauseSymbolDuration Payload
 *
 * RF replay itself needs a USRP B200 and is not covered here.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "test_main.hpp"

#include <process.h> /* _getpid: fixture dirs must be unique PER PROCESS */

#include "modulate.hpp"       /* dsp::bit_at */
#include "ui_ook_editor.hpp"  /* app::ook_editor::* */

#include <atomic>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

using namespace app::ook_editor;

namespace {

class TempDir {
   public:
    explicit TempDir(const char* tag) {
        static std::atomic<int> counter{0};
        path_ = std::filesystem::temp_directory_path() /
                ("mb200_ook_" + std::string{tag} + "_" +
                 std::to_string(_getpid()) + "_" +
                 std::to_string(counter.fetch_add(1)));
        std::error_code ec;
        std::filesystem::remove_all(path_, ec);
        std::filesystem::create_directories(path_, ec);
    }
    ~TempDir() {
        std::error_code ec;
        std::filesystem::remove_all(path_, ec);
    }
    TempDir(const TempDir&) = delete;
    TempDir& operator=(const TempDir&) = delete;

    std::string file(const std::string& name) const {
        return (path_ / name).string();
    }

   private:
    std::filesystem::path path_;
};

void write_text(const std::string& path, const std::string& content) {
    std::ofstream f{path, std::ios::binary | std::ios::trunc};
    f.write(content.data(), static_cast<std::streamsize>(content.size()));
}

std::string read_text(const std::string& path) {
    std::ifstream f{path, std::ios::binary};
    std::stringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

}  // namespace

/* --- sample-rate token mapping --------------------------------------------- */

TEST(ook_editor_sample_rate_tokens) {
    CHECK_STR_EQ(sample_rate_token(250000), "250k");
    CHECK_STR_EQ(sample_rate_token(1000000), "1M");
    CHECK_STR_EQ(sample_rate_token(2000000), "2M");
    CHECK_STR_EQ(sample_rate_token(5000000), "5M");
    CHECK_STR_EQ(sample_rate_token(10000000), "10M");
    CHECK_STR_EQ(sample_rate_token(20000000), "20M");
    CHECK_STR_EQ(sample_rate_token(3000000), "");  // unsupported

    bool ok = false;
    CHECK_EQ(sample_rate_from_token("2M", ok), uint32_t{2000000});
    CHECK(ok);
    sample_rate_from_token("3M", ok);
    CHECK(!ok);
}

/* --- reading an upstream-format file ---------------------------------------- */

TEST(ook_editor_reads_upstream_line) {
    TempDir dir{"read"};
    const std::string path = dir.file("TEST.OOK");
    write_text(path, "433920000 2M 100 4 200 10110010\n");

    OokFileData d{};
    CHECK(read_ook_file(path, d));
    CHECK_EQ(d.frequency, uint64_t{433920000});
    CHECK_EQ(d.sample_rate, uint32_t{2000000});
    CHECK_EQ(d.symbol_rate, uint16_t{100});
    CHECK_EQ(d.repeat, uint16_t{4});
    CHECK_EQ(d.pause_symbol_duration, uint16_t{200});
    CHECK_STR_EQ(d.payload, "10110010");
}

TEST(ook_editor_read_rejects_unknown_sample_rate) {
    TempDir dir{"badrate"};
    const std::string path = dir.file("BAD.OOK");
    write_text(path, "433920000 3M 100 4 200 10110010\n");

    OokFileData d{};
    CHECK(!read_ook_file(path, d));
}

TEST(ook_editor_read_rejects_malformed_line) {
    TempDir dir{"malformed"};
    const std::string path = dir.file("SHORT.OOK");
    write_text(path, "433920000 2M 100 4\n");  // only four spaces, no payload split

    OokFileData d{};
    CHECK(!read_ook_file(path, d));
}

TEST(ook_editor_read_missing_file) {
    OokFileData d{};
    CHECK(!read_ook_file("Z:/does/not/exist_9f3.OOK", d));
}

/* --- saving is byte-compatible --------------------------------------------- */

TEST(ook_editor_save_writes_exact_line) {
    TempDir dir{"save"};
    const std::string path = dir.file("OUT.OOK");

    OokFileData d{};
    d.frequency = 315000000;
    d.sample_rate = 1000000;
    d.symbol_rate = 200;
    d.repeat = 8;
    d.pause_symbol_duration = 50;
    d.payload = "111000101";

    CHECK(save_ook_file(d, path));
    CHECK_STR_EQ(read_text(path), "315000000 1M 200 8 50 111000101\n");
}

TEST(ook_editor_save_rejects_unsupported_rate) {
    TempDir dir{"saverate"};
    const std::string path = dir.file("BAD.OOK");
    OokFileData d{};
    d.sample_rate = 3000000;  // not a valid token
    d.payload = "101";
    CHECK(!save_ook_file(d, path));
}

TEST(ook_editor_round_trip) {
    TempDir dir{"round"};
    const std::string path = dir.file("RT.OOK");

    OokFileData in{};
    in.frequency = 868000000;
    in.sample_rate = 5000000;
    in.symbol_rate = 333;
    in.repeat = 3;
    in.pause_symbol_duration = 1234;
    in.payload = "0101110001110001";

    CHECK(save_ook_file(in, path));

    OokFileData out{};
    CHECK(read_ook_file(path, out));
    CHECK_EQ(out.frequency, in.frequency);
    CHECK_EQ(out.sample_rate, in.sample_rate);
    CHECK_EQ(out.symbol_rate, in.symbol_rate);
    CHECK_EQ(out.repeat, in.repeat);
    CHECK_EQ(out.pause_symbol_duration, in.pause_symbol_duration);
    CHECK_STR_EQ(out.payload, in.payload);
}

/* --- payload packing -------------------------------------------------------- */

TEST(ook_editor_pack_payload_matches_msb_first) {
    std::vector<uint8_t> out;
    CHECK_EQ(pack_payload("10110010", out), size_t{8});
    CHECK_EQ(out[0], uint8_t{0xB2});

    CHECK_EQ(pack_payload("", out), size_t{0});
    CHECK_EQ(out.size(), size_t{0});

    const std::string frags = "0101110001110001";
    const size_t bits = pack_payload(frags, out);
    for (size_t i = 0; i < bits; i++)
        CHECK_EQ(dsp::bit_at(out.data(), i), frags[i] == '1');
}
