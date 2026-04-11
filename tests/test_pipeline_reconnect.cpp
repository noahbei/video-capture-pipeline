// Integration test: disconnect/reconnect state machine using videotestsrc.
// No hardware, no v4l2loopback, no root required — GStreamer only.
//
// Exercises the same handle_error() → schedule_reconnect() → try_reconnect_cb()
// code path that a real camera disconnect would trigger, but uses:
//   - videotestsrc instead of v4l2src (no device needed)
//   - post_error_for_test() to inject a GST_RESOURCE_ERROR on the bus
//   - a temporary file as cfg.camera.device (so the access() check in
//     try_reconnect_cb passes immediately on reconnect)

#include "config.hpp"
#include "health.hpp"
#include "pipeline.hpp"

#include <cassert>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>
#include <thread>

#include <gst/gst.h>

namespace fs = std::filesystem;

static const char* OUT_DIR     = "/tmp/vcptest_reconnect";
static const char* DEVICE_FILE = "/tmp/vcptest_reconnect_device";  // fake "device" node

static bool wait_for_state(vcp::Pipeline& p, vcp::PipelineState want, int timeout_sec) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(timeout_sec);
    while (std::chrono::steady_clock::now() < deadline) {
        if (p.state() == want) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    return false;
}

static vcp::Config make_test_config() {
    vcp::Config cfg;
    // Use videotestsrc — no real camera needed.
    cfg.camera.src_element   = "videotestsrc";
    // device path is only used for the access() check in try_reconnect_cb.
    cfg.camera.device        = DEVICE_FILE;
    cfg.camera.width         = 640;
    cfg.camera.height        = 480;
    cfg.camera.framerate     = 15;
    // I420 is videotestsrc's native format — no conversion overhead.
    cfg.camera.pixel_format  = "I420";
    cfg.encoder.bitrate_kbps = 500;
    cfg.encoder.speed_preset = "ultrafast";
    cfg.encoder.tune         = "zerolatency";
    cfg.encoder.key_int_max  = 15;
    cfg.output.directory     = OUT_DIR;
    cfg.output.container     = "mp4";
    cfg.rotation.mode        = "duration";
    cfg.rotation.max_duration_sec = 30;
    cfg.encryption.enabled   = false;
    cfg.disk.min_free_bytes  = 10 * 1024 * 1024;
    cfg.disk.poll_interval_sec = 2;
    cfg.health.log_level     = "info";
    cfg.health.status_file   = "/tmp/vcptest_reconnect_status.json";
    return cfg;
}

int main(int argc, char* argv[]) {
    gst_init(&argc, &argv);
    fs::create_directories(OUT_DIR);

    // Create the fake device file so the access() guard in try_reconnect_cb passes.
    { std::ofstream f(DEVICE_FILE); }

    std::printf("=== test_pipeline_reconnect ===\n");

    const vcp::Config cfg = make_test_config();
    uint8_t key[32]{};
    vcp::Health health(cfg.health.status_file, vcp::LogLevel::Info);

    vcp::Pipeline pipeline(cfg, key, health);
    pipeline.start();

    std::thread pipeline_thread([&pipeline] { pipeline.run_loop(); });

    // --- Phase 1: verify Recording ---
    std::printf("Waiting for Recording...\n");
    if (!wait_for_state(pipeline, vcp::PipelineState::Recording, 5)) {
        std::fprintf(stderr, "FAIL: did not reach Recording within 5s\n");
        pipeline.request_quit();
        pipeline_thread.join();
        return 1;
    }
    std::printf("  PASS: reached Recording\n");

    // Let at least one frame flow so the pipeline is fully live.
    std::this_thread::sleep_for(std::chrono::milliseconds(500));

    // --- Phase 2: inject error, expect Reconnecting ---
    std::printf("Injecting simulated camera error...\n");
    pipeline.post_error_for_test();

    if (!wait_for_state(pipeline, vcp::PipelineState::Reconnecting, 5)) {
        std::fprintf(stderr, "FAIL: did not enter Reconnecting within 5s\n");
        pipeline.request_quit();
        pipeline_thread.join();
        return 1;
    }
    std::printf("  PASS: reached Reconnecting (attempts = %u)\n",
                pipeline.make_status().reconnect_attempts);

    // --- Phase 3: device stays available, expect automatic recovery ---
    // DEVICE_FILE still exists, so try_reconnect_cb's access() check passes
    // on the first timer fire (1 s backoff).
    std::printf("Waiting for recovery to Recording...\n");
    if (!wait_for_state(pipeline, vcp::PipelineState::Recording, 10)) {
        std::fprintf(stderr, "FAIL: did not recover to Recording within 10s\n");
        pipeline.request_quit();
        pipeline_thread.join();
        return 1;
    }

    const auto status = pipeline.make_status();
    assert(status.reconnect_attempts > 0);
    std::printf("  PASS: recovered to Recording (attempts = %u)\n",
                status.reconnect_attempts);

    // Verify status file was written.
    assert(fs::exists(cfg.health.status_file));
    std::printf("  PASS: status file exists\n");

    // --- Cleanup ---
    pipeline.stop();
    pipeline_thread.join();

    std::error_code ec;
    fs::remove(DEVICE_FILE, ec);

    std::printf("test_pipeline_reconnect PASSED\n");
    return 0;
}
