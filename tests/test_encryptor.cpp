// Unit tests for the encryptor module.
// No hardware, no GStreamer, no root required.
//
// Build: part of the CMake test suite (add_vcp_test)
// Run:   ./test_encryptor  (exit 0 = pass)

#include "encryptor.hpp"

#include <array>
#include <cassert>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <random>
#include <stdexcept>
#include <vector>

namespace fs = std::filesystem;

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

static std::array<uint8_t, 32> make_test_key() {
    std::array<uint8_t, 32> key{};
    for (int i = 0; i < 32; ++i) key[i] = static_cast<uint8_t>(i + 1);
    return key;
}

static fs::path tmp_path(const std::string& name) {
    return fs::temp_directory_path() / ("vcptest_" + name);
}

static void write_file(const fs::path& p, const std::vector<uint8_t>& data) {
    std::ofstream f(p, std::ios::binary | std::ios::trunc);
    f.write(reinterpret_cast<const char*>(data.data()),
            static_cast<std::streamsize>(data.size()));
}

static std::vector<uint8_t> read_file(const fs::path& p) {
    std::ifstream f(p, std::ios::binary);
    return {std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>()};
}

static void remove_if_exists(const fs::path& p) {
    std::error_code ec;
    fs::remove(p, ec);
}

// ---------------------------------------------------------------------------
// Test: round-trip (encrypt then decrypt produces identical bytes)
// ---------------------------------------------------------------------------

static void test_round_trip() {
    std::printf("test_round_trip... ");

    const auto key = make_test_key();
    const std::span<const uint8_t, 32> ks(key.data(), 32);

    // 4 KB of known data
    std::vector<uint8_t> plaintext(4096);
    for (size_t i = 0; i < plaintext.size(); ++i)
        plaintext[i] = static_cast<uint8_t>(i & 0xFF);

    const fs::path src = tmp_path("roundtrip_src.bin");
    const fs::path enc = tmp_path("roundtrip_src.bin.vcpenc");
    const fs::path dec = tmp_path("roundtrip_dec.bin");

    write_file(src, plaintext);
    vcp::encrypt_file(src, enc, ks);
    vcp::decrypt_file(enc, dec, ks);

    const auto result = read_file(dec);
    assert(result == plaintext && "round-trip: decrypted output differs from original");

    remove_if_exists(src);
    remove_if_exists(enc);
    remove_if_exists(dec);
    std::printf("PASS\n");
}

// ---------------------------------------------------------------------------
// Test: different key produces authentication failure
// ---------------------------------------------------------------------------

static void test_wrong_key() {
    std::printf("test_wrong_key... ");

    const auto key = make_test_key();
    const std::span<const uint8_t, 32> ks(key.data(), 32);

    std::vector<uint8_t> plaintext(256, 0xAB);
    const fs::path src = tmp_path("wrongkey_src.bin");
    const fs::path enc = tmp_path("wrongkey_src.bin.vcpenc");
    const fs::path dec = tmp_path("wrongkey_dec.bin");

    write_file(src, plaintext);
    vcp::encrypt_file(src, enc, ks);

    std::array<uint8_t, 32> bad_key{};
    const std::span<const uint8_t, 32> bks(bad_key.data(), 32);

    bool threw_auth_error = false;
    try {
        vcp::decrypt_file(enc, dec, bks);
    } catch (const vcp::AuthenticationError&) {
        threw_auth_error = true;
    }
    assert(threw_auth_error && "wrong key: expected AuthenticationError");
    assert(!fs::exists(dec) && "wrong key: partial output file should have been removed");

    remove_if_exists(src);
    remove_if_exists(enc);
    std::printf("PASS\n");
}

// ---------------------------------------------------------------------------
// Test: flipped tag byte produces authentication failure
// ---------------------------------------------------------------------------

static void test_tampered_tag() {
    std::printf("test_tampered_tag... ");

    const auto key = make_test_key();
    const std::span<const uint8_t, 32> ks(key.data(), 32);

    std::vector<uint8_t> plaintext(512, 0x55);
    const fs::path src = tmp_path("tamper_src.bin");
    const fs::path enc = tmp_path("tamper_src.bin.vcpenc");
    const fs::path dec = tmp_path("tamper_dec.bin");

    write_file(src, plaintext);
    vcp::encrypt_file(src, enc, ks);

    // Flip the last byte of the file (GCM tag)
    {
        std::fstream f(enc, std::ios::binary | std::ios::in | std::ios::out);
        f.seekp(-1, std::ios::end);
        char c;
        f.seekg(-1, std::ios::end);
        f.read(&c, 1);
        c ^= 0xFF;
        f.seekp(-1, std::ios::end);
        f.write(&c, 1);
    }

    bool threw = false;
    try {
        vcp::decrypt_file(enc, dec, ks);
    } catch (const vcp::AuthenticationError&) {
        threw = true;
    }
    assert(threw && "tampered tag: expected AuthenticationError");
    assert(!fs::exists(dec) && "tampered tag: partial output should have been removed");

    remove_if_exists(src);
    remove_if_exists(enc);
    std::printf("PASS\n");
}

// ---------------------------------------------------------------------------
// Test: zero-length plaintext (empty file)
// ---------------------------------------------------------------------------

static void test_empty_file() {
    std::printf("test_empty_file... ");

    const auto key = make_test_key();
    const std::span<const uint8_t, 32> ks(key.data(), 32);

    const fs::path src = tmp_path("empty_src.bin");
    const fs::path enc = tmp_path("empty_src.bin.vcpenc");
    const fs::path dec = tmp_path("empty_dec.bin");

    // Create a truly empty file
    { std::ofstream f(src, std::ios::trunc); }

    vcp::encrypt_file(src, enc, ks);
    vcp::decrypt_file(enc, dec, ks);

    const auto result = read_file(dec);
    assert(result.empty() && "empty file round-trip: output should be empty");

    remove_if_exists(src);
    remove_if_exists(enc);
    remove_if_exists(dec);
    std::printf("PASS\n");
}

// ---------------------------------------------------------------------------
// Test: large file (multi-chunk, exercises streaming code path)
// ---------------------------------------------------------------------------

static void test_large_file() {
    std::printf("test_large_file (10 MB)... ");

    const auto key = make_test_key();
    const std::span<const uint8_t, 32> ks(key.data(), 32);

    // 10 MB of pseudo-random data
    std::mt19937 rng(42);
    std::vector<uint8_t> plaintext(10 * 1024 * 1024);
    for (auto& b : plaintext) b = static_cast<uint8_t>(rng() & 0xFF);

    const fs::path src = tmp_path("large_src.bin");
    const fs::path enc = tmp_path("large_src.bin.vcpenc");
    const fs::path dec = tmp_path("large_dec.bin");

    write_file(src, plaintext);
    vcp::encrypt_file(src, enc, ks);
    vcp::decrypt_file(enc, dec, ks);

    const auto result = read_file(dec);
    assert(result == plaintext && "large file: decrypted output differs from original");

    remove_if_exists(src);
    remove_if_exists(enc);
    remove_if_exists(dec);
    std::printf("PASS\n");
}

// ---------------------------------------------------------------------------
// Test: truncated ciphertext produces EncryptorError (not auth error)
// ---------------------------------------------------------------------------

static void test_truncated_ciphertext() {
    std::printf("test_truncated_ciphertext... ");

    const auto key = make_test_key();
    const std::span<const uint8_t, 32> ks(key.data(), 32);

    std::vector<uint8_t> plaintext(1024, 0xCC);
    const fs::path src = tmp_path("trunc_src.bin");
    const fs::path enc = tmp_path("trunc_src.bin.vcpenc");
    const fs::path dec = tmp_path("trunc_dec.bin");

    write_file(src, plaintext);
    vcp::encrypt_file(src, enc, ks);

    // Truncate to just the header (30 bytes) — no ciphertext or tag
    fs::resize_file(enc, 30);

    bool threw = false;
    try {
        vcp::decrypt_file(enc, dec, ks);
    } catch (const vcp::EncryptorError&) {
        threw = true;
    } catch (const vcp::AuthenticationError&) {
        threw = true; // also acceptable
    }
    assert(threw && "truncated file: expected an error");
    assert(!fs::exists(dec) && "truncated file: output should have been cleaned up");

    remove_if_exists(src);
    remove_if_exists(enc);
    std::printf("PASS\n");
}

// ---------------------------------------------------------------------------
// main
// ---------------------------------------------------------------------------

int main() {
    std::printf("=== test_encryptor ===\n");
    test_round_trip();
    test_wrong_key();
    test_tampered_tag();
    test_empty_file();
    test_large_file();
    test_truncated_ciphertext();
    std::printf("All tests PASSED\n");
    return 0;
}
