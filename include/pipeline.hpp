#pragma once

#include "config.hpp"
#include "disk_monitor.hpp"
#include "encryptor.hpp"
#include "health.hpp"

#include <array>
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <queue>
#include <string>
#include <thread>

#include <gst/gst.h>

namespace vcp {

// Pipeline state visible to the health monitor and main loop.
enum class PipelineState {
    Starting,
    Recording,
    PausedDiskFull,
    Reconnecting,
    Stopping,
    Stopped,
};

const char* pipeline_state_str(PipelineState s);

class Pipeline {
public:
    // key must remain valid for the lifetime of the Pipeline object.
    Pipeline(const Config& cfg, const uint8_t key[32], Health& health);
    ~Pipeline();

    Pipeline(const Pipeline&) = delete;
    Pipeline& operator=(const Pipeline&) = delete;

    // Build the GStreamer element graph and set it to PLAYING.
    // Throws std::runtime_error if any element cannot be created.
    void start();

    // Gracefully stop: send EOS, wait for it to propagate, then NULL the pipeline.
    // Encrypts the final segment before returning.
    void stop();

    // Run the GLib main loop. Blocks until stop() is called (via signal handler
    // or internal error). Call from the main thread after start().
    void run_loop();

    // Request the loop to quit (async-signal-safe via g_main_loop_quit).
    void request_quit();

    // Post a fake GST_RESOURCE_ERROR on the pipeline bus to exercise the
    // disconnect/reconnect state machine without real hardware.
    // Only call this from tests while the pipeline is in Recording state.
    void post_error_for_test();

    PipelineState state() const { return state_.load(); }

    // Access the current health status snapshot (called by main loop / disk timer).
    HealthStatus make_status() const;

private:
    // ---- GStreamer elements ------------------------------------------------
    struct Elements {
        GstElement* pipeline  = nullptr;
        GstElement* src       = nullptr;
        GstElement* src_caps  = nullptr;
        GstElement* jpeg_dec  = nullptr;  // non-null only when pixel_format == "MJPG"
        GstElement* convert   = nullptr;
        GstElement* scale     = nullptr;
        GstElement* enc_caps  = nullptr;
        GstElement* queue     = nullptr;
        GstElement* encoder   = nullptr;
        GstElement* parser    = nullptr;
        GstElement* mux_sink  = nullptr;
    } el_;

    GMainLoop* loop_ = nullptr;
    guint      disk_timer_src_ = 0;
    guint      bus_watch_id_   = 0;

    // ---- Configuration + dependencies -------------------------------------
    const Config&   cfg_;
    std::array<uint8_t, 32> key_;
    Health&         health_;
    DiskMonitor     disk_monitor_;

    // ---- State machine ----------------------------------------------------
    std::atomic<PipelineState> state_{PipelineState::Stopped};
    std::atomic<uint32_t>      reconnect_attempts_{0};
    std::atomic<bool>          eos_received_{false};  // true once GST_MESSAGE_EOS arrives on bus
    uint32_t                   reconnect_backoff_sec_ = 1;
    std::time_t                start_time_            = 0;

    // ---- Segment tracking -------------------------------------------------
    std::atomic<uint32_t> fragment_index_{0};       // absolute index of the next segment to open
    uint32_t              fragment_index_start_ = 0; // offset added on each pipeline rebuild
    std::atomic<uint64_t> segments_written_{0};
    std::atomic<uint64_t> bytes_written_{0};
    std::string           last_segment_;
    std::string           last_segment_time_;
    mutable std::mutex    segment_mutex_; // protects last_segment_ and last_segment_time_

    // ---- Async encryption worker ------------------------------------------
    std::thread             enc_thread_;
    std::queue<std::string> enc_queue_;  // paths of plaintext segments to encrypt
    std::mutex              enc_mutex_;
    std::condition_variable enc_cv_;
    std::atomic<bool>       enc_stop_{false};
    std::atomic<uint32_t>   enc_queue_depth_{0};

    void encryption_worker();
    void enqueue_encryption(const std::string& plaintext_path);

    // ---- Internal helpers -------------------------------------------------
    bool build_pipeline();
    void destroy_pipeline();
    void set_state(PipelineState s);

    void handle_error(GstMessage* msg);
    void handle_eos();
    void schedule_reconnect();
    void schedule_retry();  // re-queue a reconnect timer without resetting state
    static gboolean try_reconnect_cb(gpointer user_data);
    static gboolean disk_poll_cb(gpointer user_data);

    // GStreamer signal/callback statics
    static gchar* on_format_location_full(GstElement* splitmux,
                                          guint fragment_id,
                                          GstSample* first_sample,
                                          gpointer user_data);

    static GstBusSyncReply bus_sync_handler(GstBus* bus,
                                            GstMessage* msg,
                                            gpointer user_data);
};

} // namespace vcp
