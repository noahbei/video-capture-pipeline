// Unit tests for the config module.
// No hardware, no GStreamer, no root required.

#include "config.hpp"

#include <cassert>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>

namespace fs = std::filesystem;

// Write a string to a temp file and return its path.
static fs::path write_toml(const std::string& content) {
    fs::path p = fs::temp_directory_path() / "vcptest_config.toml";
    std::ofstream f(p, std::ios::trunc);
    f << content;
    return p;
}

// ---------------------------------------------------------------------------
// Test: all fields parsed correctly
// ---------------------------------------------------------------------------

static void test_full_config() {
    std::printf("test_full_config... ");

    const std::string toml = R"toml(
[camera]
device        = "/dev/video99"
width         = 1920
height        = 1080
framerate     = 25
pixel_format  = "NV12"

[encoder]
bitrate_kbps  = 4000
speed_preset  = "fast"
tune          = "film"
key_int_max   = 50

[output]
directory        = "/tmp/vcpout"
container        = "mkv"
filename_pattern = "cam_%06d"

[rotation]
mode             = "size"
max_size_bytes   = 1073741824
max_duration_sec = 0

[encryption]
enabled          = true
key_file         = "/tmp/test.key"
delete_plaintext = false

[backpressure]
queue_max_buffers = 500

[disk]
min_free_bytes    = 209715200
poll_interval_sec = 10

[health]
log_level   = "debug"
status_file = "/tmp/vcpstatus.json"
)toml";

    const fs::path p = write_toml(toml);
    const vcp::Config cfg = vcp::load_config(p.string());

    assert(cfg.camera.device        == "/dev/video99");
    assert(cfg.camera.width         == 1920);
    assert(cfg.camera.height        == 1080);
    assert(cfg.camera.framerate     == 25);
    assert(cfg.camera.pixel_format  == "NV12");

    assert(cfg.encoder.bitrate_kbps == 4000);
    assert(cfg.encoder.speed_preset == "fast");
    assert(cfg.encoder.tune         == "film");
    assert(cfg.encoder.key_int_max  == 50);

    assert(cfg.output.directory        == "/tmp/vcpout");
    assert(cfg.output.container        == "mkv");
    assert(cfg.output.filename_pattern == "cam_%06d");

    assert(cfg.rotation.mode           == "size");
    assert(cfg.rotation.max_size_bytes == 1073741824ULL);

    assert(cfg.encryption.enabled          == true);
    assert(cfg.encryption.key_file         == "/tmp/test.key");
    assert(cfg.encryption.delete_plaintext == false);

    assert(cfg.backpressure.queue_max_buffers == 500);
    assert(cfg.disk.min_free_bytes    == 209715200ULL);
    assert(cfg.disk.poll_interval_sec == 10);
    assert(cfg.health.log_level       == "debug");
    assert(cfg.health.status_file     == "/tmp/vcpstatus.json");

    std::printf("PASS\n");
}

// ---------------------------------------------------------------------------
// Test: defaults are applied for missing optional fields
// ---------------------------------------------------------------------------

static void test_minimal_config() {
    std::printf("test_minimal_config... ");

    const std::string toml = R"toml(
[camera]
device = "/dev/video99"

[output]
directory = "/var/capture"

[rotation]
mode             = "duration"
max_duration_sec = 60

[encryption]
enabled  = false
key_file = ""
)toml";

    const fs::path p = write_toml(toml);
    const vcp::Config cfg = vcp::load_config(p.string());

    // Check defaults
    assert(cfg.camera.width         == 1280);
    assert(cfg.camera.height        == 720);
    assert(cfg.camera.framerate     == 30);
    assert(cfg.camera.pixel_format  == "YUY2");
    assert(cfg.output.container     == "mp4");
    assert(cfg.encoder.bitrate_kbps == 2000);
    assert(cfg.backpressure.queue_max_buffers == 200);

    std::printf("PASS\n");
}

// ---------------------------------------------------------------------------
// Test: bad container value throws
// ---------------------------------------------------------------------------

static void test_bad_container() {
    std::printf("test_bad_container... ");

    const std::string toml = R"toml(
[camera]
device = "/dev/video99"

[output]
directory = "/var/capture"
container = "avi"

[rotation]
mode             = "duration"
max_duration_sec = 60

[encryption]
enabled  = false
key_file = ""
)toml";

    const fs::path p = write_toml(toml);
    bool threw = false;
    try {
        vcp::load_config(p.string());
    } catch (const std::runtime_error& e) {
        threw = true;
        std::string msg = e.what();
        assert(msg.find("container") != std::string::npos && "error should mention 'container'");
    }
    assert(threw && "bad container value should throw");
    std::printf("PASS\n");
}

// ---------------------------------------------------------------------------
// Test: bad rotation mode throws
// ---------------------------------------------------------------------------

static void test_bad_rotation_mode() {
    std::printf("test_bad_rotation_mode... ");

    const std::string toml = R"toml(
[camera]
device = "/dev/video99"

[output]
directory = "/var/capture"

[rotation]
mode = "frames"

[encryption]
enabled  = false
key_file = ""
)toml";

    const fs::path p = write_toml(toml);
    bool threw = false;
    try {
        vcp::load_config(p.string());
    } catch (const std::runtime_error&) {
        threw = true;
    }
    assert(threw && "bad rotation.mode should throw");
    std::printf("PASS\n");
}

// ---------------------------------------------------------------------------
// Test: encryption enabled but key_file empty throws
// ---------------------------------------------------------------------------

static void test_encryption_no_key() {
    std::printf("test_encryption_no_key... ");

    const std::string toml = R"toml(
[camera]
device = "/dev/video99"

[output]
directory = "/var/capture"

[rotation]
mode             = "duration"
max_duration_sec = 30

[encryption]
enabled  = true
key_file = ""
)toml";

    const fs::path p = write_toml(toml);
    bool threw = false;
    try {
        vcp::load_config(p.string());
    } catch (const std::runtime_error& e) {
        threw = true;
        std::string msg = e.what();
        assert(msg.find("key_file") != std::string::npos && "error should mention 'key_file'");
    }
    assert(threw && "encryption.enabled with empty key_file should throw");
    std::printf("PASS\n");
}

// ---------------------------------------------------------------------------
// Test: unknown keys are silently ignored
// ---------------------------------------------------------------------------

static void test_unknown_keys_ignored() {
    std::printf("test_unknown_keys_ignored... ");

    const std::string toml = R"toml(
[camera]
device = "/dev/video99"
future_option = "something"

[output]
directory = "/var/capture"

[rotation]
mode             = "duration"
max_duration_sec = 60

[encryption]
enabled  = false
key_file = ""

[new_future_section]
foo = "bar"
)toml";

    const fs::path p = write_toml(toml);
    // Should not throw
    vcp::Config cfg = vcp::load_config(p.string());
    assert(cfg.camera.device == "/dev/video99");
    std::printf("PASS\n");
}

// ---------------------------------------------------------------------------
// Test: load_key_file requires exactly 32 bytes
// ---------------------------------------------------------------------------

static void test_key_file_size() {
    std::printf("test_key_file_size... ");

    const fs::path p31 = fs::temp_directory_path() / "vcptest_key31.bin";
    const fs::path p32 = fs::temp_directory_path() / "vcptest_key32.bin";
    const fs::path p33 = fs::temp_directory_path() / "vcptest_key33.bin";

    // 31 bytes
    { std::ofstream f(p31, std::ios::binary); f << std::string(31, '\x42'); }
    // 32 bytes
    { std::ofstream f(p32, std::ios::binary); f << std::string(32, '\x42'); }
    // 33 bytes
    { std::ofstream f(p33, std::ios::binary); f << std::string(33, '\x42'); }

    uint8_t key[32]{};

    bool threw31 = false;
    try { vcp::load_key_file(p31.string(), key); } catch (...) { threw31 = true; }
    assert(threw31 && "31-byte key file should throw");

    bool threw33 = false;
    try { vcp::load_key_file(p33.string(), key); } catch (...) { threw33 = true; }
    assert(threw33 && "33-byte key file should throw");

    bool threw32 = false;
    try { vcp::load_key_file(p32.string(), key); } catch (...) { threw32 = true; }
    assert(!threw32 && "32-byte key file should not throw");
    assert(key[0] == 0x42);

    std::error_code ec;
    fs::remove(p31, ec); fs::remove(p32, ec); fs::remove(p33, ec);
    std::printf("PASS\n");
}

// ---------------------------------------------------------------------------
// main
// ---------------------------------------------------------------------------

int main() {
    std::printf("=== test_config ===\n");
    test_full_config();
    test_minimal_config();
    test_bad_container();
    test_bad_rotation_mode();
    test_encryption_no_key();
    test_unknown_keys_ignored();
    test_key_file_size();
    std::printf("All tests PASSED\n");
    return 0;
}
