// Integration test: camera disconnect and reconnect.
// Requires: v4l2loopback kernel module and root privileges.
// Run scripts/create_loopback.sh before this test to set up the virtual device.
//
// The test starts a vcpcapture pipeline on /dev/video10, kills the feeder
// process (simulating a disconnect), verifies the reconnect state, then
// restarts the feeder and verifies recovery.
//
// Labeled "integration;requires_v4l2loopback" in CMake.

#include "config.hpp"
#include "health.hpp"
#include "pipeline.hpp"

#include <atomic>
#include <cassert>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>
#include <thread>

#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>

#include <gst/gst.h>

namespace fs = std::filesystem;

static const char* LOOPBACK_DEVICE = "/dev/video10";
static const char* OUT_DIR         = "/tmp/vcptest_disconnect";

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

// Start "gst-launch-1.0 videotestsrc ! v4l2sink device=/dev/video10" as a child process.
static pid_t start_feeder() {
    pid_t pid = fork();
    if (pid == 0) {
        // child
        execlp("gst-launch-1.0", "gst-launch-1.0",
               "videotestsrc", "is-live=true",
               "!", "video/x-raw,format=YUY2,width=640,height=480,framerate=15/1",
               "!", "v4l2sink", "device=/dev/video10",
               nullptr);
        std::_Exit(1); // exec failed
    }
    return pid;
}

static void stop_feeder(pid_t pid) {
    if (pid <= 0) return;
    kill(pid, SIGTERM);
    waitpid(pid, nullptr, 0);
}

// Poll until the pipeline state matches, or timeout_sec elapses.
static bool wait_for_state(vcp::Pipeline& pipeline, vcp::PipelineState want, int timeout_sec) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(timeout_sec);
    while (std::chrono::steady_clock::now() < deadline) {
        if (pipeline.state() == want) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }
    return false;
}

static vcp::Config make_test_config() {
    vcp::Config cfg;
    cfg.camera.device        = LOOPBACK_DEVICE;
    cfg.camera.width         = 640;
    cfg.camera.height        = 480;
    cfg.camera.framerate     = 15;
    cfg.camera.pixel_format  = "YUY2";
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
    cfg.health.status_file   = "/tmp/vcptest_disconnect_status.json";
    return cfg;
}

// ---------------------------------------------------------------------------
// main
// ---------------------------------------------------------------------------

int main(int argc, char* argv[]) {
    // Check prerequisites
    if (access(LOOPBACK_DEVICE, R_OK) != 0) {
        std::fprintf(stderr,
            "SKIP: %s not found.\n"
            "Run scripts/create_loopback.sh first (requires root + v4l2loopback).\n",
            LOOPBACK_DEVICE);
        return 0;
    }

    gst_init(&argc, &argv);
    fs::create_directories(OUT_DIR);

    std::printf("=== test_pipeline_disconnect ===\n");

    const vcp::Config cfg = make_test_config();
    uint8_t key[32]{};
    vcp::Health health(cfg.health.status_file, vcp::LogLevel::Info);

    // Start the loopback feeder
    pid_t feeder = start_feeder();
    std::this_thread::sleep_for(std::chrono::seconds(1)); // let feeder start up

    std::printf("Starting pipeline on %s...\n", LOOPBACK_DEVICE);

    // Run pipeline in a background thread (run_loop blocks)
    vcp::Pipeline pipeline(cfg, key, health);
    pipeline.start();

    std::thread pipeline_thread([&pipeline] { pipeline.run_loop(); });

    // --- Phase 1: verify pipeline reaches Recording ---
    std::printf("Waiting for Recording state...\n");
    bool reached_recording = wait_for_state(pipeline, vcp::PipelineState::Recording, 5);
    if (!reached_recording) {
        std::fprintf(stderr, "FAIL: pipeline did not reach Recording within 5s\n");
        pipeline.request_quit();
        pipeline_thread.join();
        stop_feeder(feeder);
        return 1;
    }
    std::printf("  PASS: reached Recording\n");
    std::this_thread::sleep_for(std::chrono::seconds(2));

    // --- Phase 2: kill feeder, expect Reconnecting ---
    std::printf("Killing feeder (simulating disconnect)...\n");
    stop_feeder(feeder);
    feeder = 0;

    bool reached_reconnecting = wait_for_state(pipeline, vcp::PipelineState::Reconnecting, 5);
    if (!reached_reconnecting) {
        std::fprintf(stderr, "FAIL: pipeline did not enter Reconnecting within 5s after disconnect\n");
        pipeline.request_quit();
        pipeline_thread.join();
        return 1;
    }
    std::printf("  PASS: reached Reconnecting (reconnect_attempts = %u)\n",
                pipeline.make_status().reconnect_attempts);

    // --- Phase 3: restart feeder, expect recovery ---
    std::printf("Restarting feeder...\n");
    feeder = start_feeder();
    std::this_thread::sleep_for(std::chrono::seconds(1));

    // Allow up to 30s for reconnect (backoff can be up to a few seconds)
    bool reached_recording2 = wait_for_state(pipeline, vcp::PipelineState::Recording, 30);
    if (!reached_recording2) {
        std::fprintf(stderr, "FAIL: pipeline did not recover to Recording within 30s\n");
        pipeline.request_quit();
        pipeline_thread.join();
        stop_feeder(feeder);
        return 1;
    }

    const auto status = pipeline.make_status();
    assert(status.reconnect_attempts > 0 && "reconnect_attempts should be > 0 after a disconnect");
    std::printf("  PASS: recovered to Recording (reconnect_attempts = %u)\n",
                status.reconnect_attempts);

    // Verify status file was written
    assert(fs::exists(cfg.health.status_file) && "status file should exist");
    std::printf("  PASS: status file exists at %s\n", cfg.health.status_file.c_str());

    // --- Cleanup ---
    pipeline.stop();
    pipeline_thread.join();
    stop_feeder(feeder);

    std::printf("test_pipeline_disconnect PASSED\n");
    return 0;
}
