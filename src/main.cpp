#include "config.hpp"
#include "health.hpp"
#include "pipeline.hpp"

#include <atomic>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <stdexcept>
#include <string>

#include <gst/gst.h>

namespace {

// Set by signal handler; pipeline checks this via request_quit()
std::atomic<bool> g_quit_requested{false};
vcp::Pipeline*    g_pipeline_ptr = nullptr;

void signal_handler(int /*sig*/) {
    g_quit_requested.store(true);
    if (g_pipeline_ptr) {
        g_pipeline_ptr->request_quit();
    }
}

void print_usage(const char* argv0) {
    std::fprintf(stderr,
        "Usage: %s --config <path>\n"
        "\n"
        "Options:\n"
        "  --config <path>   Path to TOML config file (required)\n"
        "  --version         Print version and exit\n"
        "  --help            Print this help\n",
        argv0);
}

} // namespace

int main(int argc, char* argv[]) {
    // Parse arguments
    std::string config_path;
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--help" || arg == "-h") {
            print_usage(argv[0]);
            return 0;
        }
        if (arg == "--version") {
            std::printf("vcpcapture 1.0.0\n");
            return 0;
        }
        if ((arg == "--config" || arg == "-c") && i + 1 < argc) {
            config_path = argv[++i];
        }
    }

    if (config_path.empty()) {
        std::fprintf(stderr, "Error: --config is required\n\n");
        print_usage(argv[0]);
        return 1;
    }

    // Initialize GStreamer before any GStreamer calls
    gst_init(&argc, &argv);

    // Load configuration
    vcp::Config cfg;
    try {
        cfg = vcp::load_config(config_path);
    } catch (const std::exception& e) {
        std::fprintf(stderr, "Configuration error: %s\n", e.what());
        return 1;
    }

    // Set up health/logging
    vcp::Health health(cfg.health.status_file, vcp::parse_log_level(cfg.health.log_level));
    health.info("vcpcapture starting, config: " + config_path);

    // Ensure output directory exists
    try {
        std::filesystem::create_directories(cfg.output.directory);
    } catch (const std::exception& e) {
        health.error("Cannot create output directory: " + std::string(e.what()));
        return 1;
    }

    // Load encryption key
    uint8_t key[32]{};
    if (cfg.encryption.enabled) {
        try {
            vcp::load_key_file(cfg.encryption.key_file, key);
            health.info("Encryption key loaded from " + cfg.encryption.key_file);
        } catch (const std::exception& e) {
            health.error("Key file error: " + std::string(e.what()));
            return 1;
        }
    }

    // Install signal handlers (SIGTERM / SIGINT)
    {
        struct sigaction sa{};
        sa.sa_handler = signal_handler;
        sigemptyset(&sa.sa_mask);
        sa.sa_flags = SA_RESTART;
        sigaction(SIGTERM, &sa, nullptr);
        sigaction(SIGINT,  &sa, nullptr);
    }

    // Build and run the pipeline
    try {
        vcp::Pipeline pipeline(cfg, key, health);
        g_pipeline_ptr = &pipeline;

        pipeline.start();
        pipeline.run_loop(); // blocks until quit is requested or fatal error
    } catch (const std::exception& e) {
        health.error("Fatal pipeline error: " + std::string(e.what()));
        g_pipeline_ptr = nullptr;
        gst_deinit();
        return 1;
    }

    g_pipeline_ptr = nullptr;
    gst_deinit();
    health.info("vcpcapture exited cleanly");
    return 0;
}
