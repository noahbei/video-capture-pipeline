#include "pipeline.hpp"
#include "utils.hpp"

#include <cassert>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <stdexcept>
#include <string>

#include <unistd.h>

#include <gst/gst.h>
#include <gst/video/video.h>

namespace vcp {

namespace fs = std::filesystem;

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

const char* pipeline_state_str(PipelineState s) {
    switch (s) {
        case PipelineState::Starting:       return "starting";
        case PipelineState::Recording:      return "recording";
        case PipelineState::PausedDiskFull: return "paused_disk_full";
        case PipelineState::Reconnecting:   return "reconnecting";
        case PipelineState::Stopping:       return "stopping";
        case PipelineState::Stopped:        return "stopped";
    }
    return "unknown";
}

// Map x264enc speed-preset string → GStreamer enum value.
// Falls back to 1 (ultrafast) on unknown string.
static guint x264_speed_preset(const std::string& s) {
    // GStreamer x264enc speed-preset enum: ultrafast=1 … placebo=9
    static const std::pair<const char*, guint> table[] = {
        {"ultrafast", 1}, {"superfast", 2}, {"veryfast", 3},
        {"faster",    4}, {"fast",      5}, {"medium",   6},
        {"slow",      7}, {"slower",    8}, {"veryslow", 9}, {"placebo", 10},
    };
    for (auto& [name, val] : table)
        if (s == name) return val;
    return 1; // default: ultrafast
}

// ---------------------------------------------------------------------------
// Constructor / Destructor
// ---------------------------------------------------------------------------

Pipeline::Pipeline(const Config& cfg, const uint8_t key[32], Health& health)
    : cfg_(cfg)
    , health_(health)
    , disk_monitor_(cfg.output.directory, cfg.disk.min_free_bytes)
{
    std::memcpy(key_.data(), key, 32);

    // Generate a session ID for this process lifetime so that segments from
    // different runs never share filenames (e.g. "20260412T142205").
    std::time_t now = std::time(nullptr);
    struct tm tm_buf{};
    gmtime_r(&now, &tm_buf);
    char sid[16];
    std::strftime(sid, sizeof(sid), "%Y%m%dT%H%M%S", &tm_buf);
    session_id_ = sid;
}

Pipeline::~Pipeline() {
    // Ensure the encryption thread is stopped
    {
        std::lock_guard<std::mutex> lk(enc_mutex_);
        enc_stop_ = true;
    }
    enc_cv_.notify_all();
    if (enc_thread_.joinable()) enc_thread_.join();

    destroy_pipeline();
}

// ---------------------------------------------------------------------------
// start / stop / run_loop
// ---------------------------------------------------------------------------

void Pipeline::start() {
    set_state(PipelineState::Starting);
    start_time_ = std::time(nullptr);

    // Start encryption worker thread
    enc_thread_ = std::thread(&Pipeline::encryption_worker, this);

    if (!build_pipeline()) {
        throw std::runtime_error("Pipeline::start: failed to build GStreamer pipeline");
    }

    // Register disk poll timer on the main loop
    // (loop_ is created in build_pipeline)
    disk_timer_src_ = g_timeout_add_seconds(
        static_cast<guint>(cfg_.disk.poll_interval_sec), disk_poll_cb, this);

    GstStateChangeReturn ret = gst_element_set_state(el_.pipeline, GST_STATE_PLAYING);
    if (ret == GST_STATE_CHANGE_FAILURE) {
        // If this is a v4l2src pipeline and the device node is not accessible,
        // treat startup failure the same as a mid-session disconnect: tear down
        // and enter the reconnect loop so the daemon waits for the camera rather
        // than exiting.
        const bool is_v4l2 = cfg_.camera.src_element.empty() ||
                             cfg_.camera.src_element == "v4l2src";
        if (is_v4l2 && access(cfg_.camera.device.c_str(), R_OK) != 0) {
            health_.info("Camera device not available at startup, entering reconnect loop");
            schedule_reconnect();
            return;
        }
        throw std::runtime_error("Pipeline::start: failed to set pipeline to PLAYING");
    }

    set_state(PipelineState::Recording);
    health_.info("Pipeline started, recording to " + cfg_.output.directory);

    HealthStatus st = make_status();
    health_.write_status(st);
}

void Pipeline::stop() {
    set_state(PipelineState::Stopping);
    health_.info("Pipeline stopping");

    if (el_.pipeline) {
        // Send EOS; handle_eos() will call g_main_loop_quit() once EOS propagates
        // through splitmuxsink (which writes the MP4 moov atom before posting EOS).
        gst_element_send_event(el_.pipeline, gst_event_new_eos());
        // Safety: force quit if EOS doesn't arrive within 5 s.
        g_timeout_add_seconds(5, [](gpointer udata) -> gboolean {
            auto* self = static_cast<Pipeline*>(udata);
            if (self->state_.load() == PipelineState::Stopping &&
                self->loop_ && g_main_loop_is_running(self->loop_)) {
                self->health_.warn("EOS timeout, forcing loop quit");
                g_main_loop_quit(self->loop_);
            }
            return FALSE;
        }, this);
    } else {
        // No active pipeline (e.g., Reconnecting state); quit immediately.
        if (loop_ && g_main_loop_is_running(loop_))
            g_main_loop_quit(loop_);
    }
}

void Pipeline::run_loop() {
    if (!loop_) return;
    g_main_loop_run(loop_); // blocks

    // After the loop exits, finalize the current segment before touching it on disk.
    //
    // The loop can exit in three ways:
    //   1. handle_eos() with state==Stopping  — EOS already propagated; muxer is finalized.
    //   2. request_quit() from signal handler — no EOS was ever sent; muxer is NOT finalized.
    //   3. handle_error() non-recoverable    — no EOS; pipeline may be in error state.
    //
    // For cases 2 and 3 we send EOS now and wait for splitmuxsink to write its trailer
    // (the MP4 moov atom) before we NULL the pipeline.  Without this, the recorded
    // file has no moov atom and cannot be played back.
    //
    // el_.pipeline is null when we were in the Reconnecting state (pipeline torn down
    // but not yet rebuilt) — nothing to finalize in that case.
    if (el_.pipeline) {
        if (!eos_received_.load()) {
            // EOS was not received through the normal bus path; send it now.
            gst_element_send_event(el_.pipeline, gst_event_new_eos());
            // Poll the bus directly — the GLib main loop is no longer running.
            GstBus* bus = gst_element_get_bus(el_.pipeline);
            GstMessage* msg = gst_bus_timed_pop_filtered(
                bus, 5 * GST_SECOND,
                static_cast<GstMessageType>(GST_MESSAGE_EOS | GST_MESSAGE_ERROR));
            if (msg)
                gst_message_unref(msg);
            else
                health_.warn("Shutdown: EOS did not propagate within 5 s; segment may be incomplete");
            gst_object_unref(bus);
        }
        // Set to NULL and wait for the state change to complete so the file is
        // fully written to disk before the encryption worker opens it.
        gst_element_set_state(el_.pipeline, GST_STATE_NULL);
        gst_element_get_state(el_.pipeline, nullptr, nullptr, 5 * GST_SECOND);
    }

    // Encrypt the last open segment. fragment_index_ always reflects the
    // absolute index of the next-to-be-opened segment, so the currently-open
    // (or last-open) segment is at fragment_index_ - 1. Do this regardless of
    // whether el_.pipeline is set so a partial segment from a mid-reconnect
    // shutdown is still encrypted.
    if (cfg_.encryption.enabled) {
        const uint32_t last_idx = fragment_index_.load();
        if (last_idx > 0) {
            const std::string path = utils::segment_filename(cfg_, session_id_, last_idx - 1);
            if (fs::exists(path)) {
                enqueue_encryption(path);
            }
        }
    }

    // Drain encryption queue
    {
        std::lock_guard<std::mutex> lk(enc_mutex_);
        enc_stop_ = true;
    }
    enc_cv_.notify_all();
    if (enc_thread_.joinable()) enc_thread_.join();

    set_state(PipelineState::Stopped);
    health_.info("Pipeline stopped");
    health_.write_status(make_status());
}

void Pipeline::request_quit() {
    if (loop_ && g_main_loop_is_running(loop_)) {
        g_main_loop_quit(loop_);
    }
}

// ---------------------------------------------------------------------------
// build_pipeline
// ---------------------------------------------------------------------------

bool Pipeline::build_pipeline() {
    // On reconnect the GLib main loop is already running; reuse it.
    if (!loop_) {
        loop_ = g_main_loop_new(nullptr, FALSE);
    }

    // Create all elements
    const char* src_factory = cfg_.camera.src_element.empty()
                              ? "v4l2src"
                              : cfg_.camera.src_element.c_str();
    el_.pipeline = gst_pipeline_new("vcpcapture");
    el_.src      = gst_element_factory_make(src_factory,    "src");
    el_.src_caps = gst_element_factory_make("capsfilter",   "src_caps");
    el_.convert  = gst_element_factory_make("videoconvert", "convert");
    el_.scale    = gst_element_factory_make("videoscale",   "scale");
    el_.enc_caps = gst_element_factory_make("capsfilter",   "enc_caps");
    el_.queue    = gst_element_factory_make("queue",        "enc_queue");
    el_.encoder  = gst_element_factory_make("x264enc",      "encoder");
    el_.parser   = gst_element_factory_make("h264parse",    "parser");
    el_.mux_sink = gst_element_factory_make("splitmuxsink", "muxsink");

    const bool is_mjpg = (cfg_.camera.pixel_format == "MJPG");

    if (is_mjpg)
        el_.jpeg_dec = gst_element_factory_make("jpegdec", "jpeg_dec");

    // Validate all elements were created
    if (!el_.pipeline || !el_.src || !el_.src_caps || !el_.convert ||
        !el_.scale || !el_.enc_caps || !el_.queue || !el_.encoder ||
        !el_.parser || !el_.mux_sink || (is_mjpg && !el_.jpeg_dec)) {
        health_.error("Failed to create one or more GStreamer elements. "
                      "Check that gst-plugins-ugly (x264enc) is installed.");
        return false;
    }

    // --- Set properties ---

    // v4l2src (or test source)
    if (cfg_.camera.src_element.empty() || cfg_.camera.src_element == "v4l2src") {
        g_object_set(el_.src,
                     "device",       cfg_.camera.device.c_str(),
                     "do-timestamp", TRUE,
                     nullptr);
    } else {
        // Test source (e.g. videotestsrc): make it behave like a live source.
        g_object_set(el_.src, "is-live", TRUE, nullptr);
    }

    // capsfilter: source caps lock V4L2 format
    GstCaps* src_caps;
    if (is_mjpg) {
        // MJPG is compressed — V4L2 delivers it as image/jpeg, not video/x-raw
        src_caps = gst_caps_new_simple(
            "image/jpeg",
            "width",     G_TYPE_INT,        cfg_.camera.width,
            "height",    G_TYPE_INT,        cfg_.camera.height,
            "framerate", GST_TYPE_FRACTION, cfg_.camera.framerate, 1,
            nullptr);
    } else {
        src_caps = gst_caps_new_simple(
            "video/x-raw",
            "format",    G_TYPE_STRING,     cfg_.camera.pixel_format.c_str(),
            "width",     G_TYPE_INT,        cfg_.camera.width,
            "height",    G_TYPE_INT,        cfg_.camera.height,
            "framerate", GST_TYPE_FRACTION, cfg_.camera.framerate, 1,
            nullptr);
    }
    g_object_set(el_.src_caps, "caps", src_caps, nullptr);
    gst_caps_unref(src_caps);

    // capsfilter: encoder input (I420 after videoconvert)
    GstCaps* enc_caps = gst_caps_new_simple(
        "video/x-raw",
        "format", G_TYPE_STRING, "I420",
        "width",  G_TYPE_INT,    cfg_.camera.width,
        "height", G_TYPE_INT,    cfg_.camera.height,
        nullptr);
    g_object_set(el_.enc_caps, "caps", enc_caps, nullptr);
    gst_caps_unref(enc_caps);

    // queue: drop-oldest backpressure
    g_object_set(el_.queue,
                 "max-size-buffers", static_cast<guint>(cfg_.backpressure.queue_max_buffers),
                 "leaky",            (guint)2,    // GST_QUEUE_LEAK_DOWNSTREAM = drop oldest
                 "max-size-bytes",   (guint64)0,
                 "max-size-time",    (guint64)0,
                 nullptr);

    // x264enc
    g_object_set(el_.encoder,
                 "bitrate",      static_cast<guint>(cfg_.encoder.bitrate_kbps),
                 "speed-preset", x264_speed_preset(cfg_.encoder.speed_preset),
                 "tune",         (guint)0x04,   // zerolatency flag
                 "key-int-max",  static_cast<guint>(cfg_.encoder.key_int_max),
                 nullptr);

    // h264parse: re-emit SPS/PPS with every IDR so each segment is self-contained
    g_object_set(el_.parser, "config-interval", -1, nullptr);

    // splitmuxsink
    const char* muxer_factory = (cfg_.output.container == "mkv") ? "matroskamux" : "mp4mux";
    guint64 max_time  = (cfg_.rotation.mode == "duration")
                        ? cfg_.rotation.max_duration_sec * GST_SECOND : 0;
    guint64 max_bytes = (cfg_.rotation.mode == "size")
                        ? cfg_.rotation.max_size_bytes : 0;

    // Use a placeholder location; format-location-full signal overrides it
    std::string location = cfg_.output.directory + "/seg_%05d." + cfg_.output.container;
    // async-finalize=FALSE: splitmuxsink writes the moov atom synchronously
    // before posting EOS on the bus. This guarantees that when our bus watch
    // sees the EOS message, the file is fully written and safe to encrypt.
    // With async-finalize=TRUE (the GStreamer 1.20+ default), the moov atom
    // is written in a separate internal pipeline after EOS is posted, creating
    // a window where we would encrypt an incomplete file.
    g_object_set(el_.mux_sink,
                 "location",        location.c_str(),
                 "muxer-factory",   muxer_factory,
                 "max-size-time",   max_time,
                 "max-size-bytes",  max_bytes,
                 "async-finalize",  FALSE,
                 nullptr);

    // Add all elements to the pipeline bin
    if (is_mjpg) {
        gst_bin_add_many(GST_BIN(el_.pipeline),
                         el_.src, el_.src_caps, el_.jpeg_dec, el_.convert, el_.scale,
                         el_.enc_caps, el_.queue, el_.encoder, el_.parser,
                         el_.mux_sink, nullptr);
    } else {
        gst_bin_add_many(GST_BIN(el_.pipeline),
                         el_.src, el_.src_caps, el_.convert, el_.scale,
                         el_.enc_caps, el_.queue, el_.encoder, el_.parser,
                         el_.mux_sink, nullptr);
    }

    // Link: src → src_caps → [jpegdec →] convert → scale → enc_caps → queue → encoder → parser → mux_sink
    bool linked;
    if (is_mjpg) {
        linked = gst_element_link_many(el_.src, el_.src_caps, el_.jpeg_dec, el_.convert,
                                       el_.scale, el_.enc_caps, el_.queue,
                                       el_.encoder, el_.parser, el_.mux_sink, nullptr);
    } else {
        linked = gst_element_link_many(el_.src, el_.src_caps, el_.convert,
                                       el_.scale, el_.enc_caps, el_.queue,
                                       el_.encoder, el_.parser, el_.mux_sink, nullptr);
    }
    if (!linked) {
        health_.error("Failed to link GStreamer pipeline elements");
        return false;
    }

    // Connect format-location-full signal on splitmuxsink
    g_signal_connect(el_.mux_sink, "format-location-full",
                     G_CALLBACK(on_format_location_full), this);

    // Install bus watch on the default GLib main context
    GstBus* bus = gst_element_get_bus(el_.pipeline);
    bus_watch_id_ = gst_bus_add_watch(bus, [](GstBus*, GstMessage* msg, gpointer udata) -> gboolean {
        auto* self = static_cast<Pipeline*>(udata);
        switch (GST_MESSAGE_TYPE(msg)) {
            case GST_MESSAGE_ERROR:
                self->handle_error(msg);
                break;
            case GST_MESSAGE_EOS:
                self->handle_eos();
                break;
            case GST_MESSAGE_WARNING: {
                GError* err = nullptr;
                gchar*  dbg = nullptr;
                gst_message_parse_warning(msg, &err, &dbg);
                self->health_.warn(std::string("GStreamer warning: ") + (err ? err->message : "?")
                                   + (dbg ? std::string(" [") + dbg + "]" : ""));
                g_clear_error(&err);
                g_free(dbg);
                break;
            }
            default:
                break;
        }
        return TRUE;
    }, this);
    gst_object_unref(bus);

    return true;
}

void Pipeline::destroy_pipeline() {
    if (disk_timer_src_) {
        g_source_remove(disk_timer_src_);
        disk_timer_src_ = 0;
    }
    if (bus_watch_id_) {
        g_source_remove(bus_watch_id_);
        bus_watch_id_ = 0;
    }
    if (el_.pipeline) {
        gst_element_set_state(el_.pipeline, GST_STATE_NULL);
        gst_object_unref(el_.pipeline);
        el_ = {};
    }
    if (loop_) {
        g_main_loop_unref(loop_);
        loop_ = nullptr;
    }
}

// ---------------------------------------------------------------------------
// State helpers
// ---------------------------------------------------------------------------

void Pipeline::set_state(PipelineState s) {
    state_.store(s);
    health_.info(std::string("State → ") + pipeline_state_str(s));
}

HealthStatus Pipeline::make_status() const {
    HealthStatus st;
    st.state             = pipeline_state_str(state_.load());
    st.uptime_sec        = static_cast<uint64_t>(std::time(nullptr) - start_time_);
    st.segments_written  = segments_written_.load();
    st.bytes_written     = bytes_written_.load();
    st.disk_free_bytes   = disk_monitor_.free_bytes();
    st.reconnect_attempts = reconnect_attempts_.load();
    st.enc_queue_depth   = enc_queue_depth_.load();
    {
        std::lock_guard<std::mutex> lk(segment_mutex_);
        st.last_segment      = last_segment_;
        st.last_segment_time = last_segment_time_;
    }
    return st;
}

// ---------------------------------------------------------------------------
// Bus message handlers
// ---------------------------------------------------------------------------

void Pipeline::handle_error(GstMessage* msg) {
    GError* err = nullptr;
    gchar*  dbg = nullptr;
    gst_message_parse_error(msg, &err, &dbg);

    const std::string err_msg = std::string(err ? err->message : "unknown error")
                              + (dbg ? std::string(" [") + dbg + "]" : "");
    health_.error("GStreamer error: " + err_msg);

    // Only resource errors (device gone, can't open, read failure) are recoverable
    // via reconnect. Stream errors (caps negotiation, unsupported format/framerate)
    // are configuration mistakes that retrying will never fix — treat them as fatal.
    bool is_recoverable = err && (err->domain == GST_RESOURCE_ERROR);
    g_clear_error(&err);
    g_free(dbg);

    if (is_recoverable && state_.load() == PipelineState::Recording) {
        // Camera disconnect or read failure — attempt reconnect
        schedule_reconnect();
    } else if (state_.load() == PipelineState::Reconnecting) {
        // v4l2src often fires multiple errors as it shuts down (e.g. poll
        // error followed by buffer allocation failure). We're already waiting
        // for EOS to finalize the segment; just ignore these follow-up errors.
        health_.info("Ignoring follow-up error while waiting for segment finalization");
    } else {
        // Non-recoverable error (format/caps mismatch, or error outside Recording state); quit
        if (loop_) g_main_loop_quit(loop_);
    }
}

void Pipeline::handle_eos() {
    health_.info("EOS received");
    eos_received_.store(true);
    const auto cur_state = state_.load();
    if (cur_state == PipelineState::PausedDiskFull) {
        // EOS was triggered by disk-full handler; now actually pause
        gst_element_set_state(el_.pipeline, GST_STATE_PAUSED);
        health_.write_status(make_status());
    } else if (cur_state == PipelineState::Stopping) {
        if (loop_) g_main_loop_quit(loop_);
    } else if (cur_state == PipelineState::Reconnecting) {
        // EOS from the muxer finalization we triggered in schedule_reconnect().
        do_reconnect_teardown(true);
    }
}

// ---------------------------------------------------------------------------
// Camera reconnect
// ---------------------------------------------------------------------------

void Pipeline::schedule_reconnect() {
    set_state(PipelineState::Reconnecting);
    reconnect_teardown_done_.store(false);
    reconnect_attempts_.fetch_add(1);

    // GStreamer's BaseSrc automatically pushes an EOS event downstream when
    // v4l2src's fill() returns GST_FLOW_ERROR (see gst_base_src_loop in
    // basesrc.c: any flow return ≤ GST_FLOW_EOS triggers a pad EOS push).
    // That EOS is already in flight through the pipeline toward splitmuxsink.
    //
    // With async-finalize=FALSE (set in build_pipeline), splitmuxsink writes
    // the moov atom synchronously before posting GST_MESSAGE_EOS on the bus.
    // So we just keep the bus watch alive and wait: handle_eos() will call
    // do_reconnect_teardown(true) once the file is fully written.
    //
    // A 3 s fallback timer covers the case where EOS never arrives (e.g. the
    // streaming thread was killed before pushing EOS, or the muxer hangs).
    health_.info("Waiting for muxer to finalize segment on disconnect");
    g_timeout_add_seconds(3, reconnect_eos_timeout_cb, this);
    health_.write_status(make_status());
    // Bus watch stays active; handle_eos() will call do_reconnect_teardown().
}

// Called by handle_eos() (good path) or reconnect_eos_timeout_cb() (fallback).
// Guards against double execution with reconnect_teardown_done_.
void Pipeline::do_reconnect_teardown(bool segment_finalized) {
    bool expected = false;
    if (!reconnect_teardown_done_.compare_exchange_strong(expected, true))
        return; // already ran (timeout fired after handle_eos, or vice-versa)

    if (segment_finalized)
        health_.info("Segment finalized cleanly on disconnect");
    else
        health_.warn("EOS did not propagate on disconnect; segment may be incomplete");

    if (bus_watch_id_) {
        g_source_remove(bus_watch_id_);
        bus_watch_id_ = 0;
    }
    if (el_.pipeline) {
        gst_element_set_state(el_.pipeline, GST_STATE_NULL);
        gst_element_get_state(el_.pipeline, nullptr, nullptr, GST_SECOND);
        gst_object_unref(el_.pipeline);
        el_ = {};
    }

    // Handle the segment that was open at disconnect time.
    const uint32_t partial_idx = fragment_index_.load();
    if (partial_idx > 0) {
        const std::string partial_path = utils::segment_filename(cfg_, session_id_, partial_idx - 1);
        std::error_code ec;
        if (fs::exists(partial_path, ec)) {
            if (cfg_.encryption.enabled) {
                enqueue_encryption(partial_path);
            } else if (!segment_finalized) {
                fs::remove(partial_path, ec);
                if (ec) health_.warn("Failed to remove incomplete segment: " + partial_path + ": " + ec.message());
                else    health_.info("Removed incomplete segment: " + partial_path);
            }
            // else: encryption disabled, file is a valid MP4 — keep it.
        }
    }

    const uint32_t backoff = reconnect_backoff_sec_;
    reconnect_backoff_sec_ = std::min(reconnect_backoff_sec_ * 2u, 60u);
    health_.info("Scheduling reconnect in " + std::to_string(backoff) + "s");
    health_.write_status(make_status());
    g_timeout_add_seconds(backoff, try_reconnect_cb, this);
}

gboolean Pipeline::reconnect_eos_timeout_cb(gpointer user_data) {
    auto* self = static_cast<Pipeline*>(user_data);
    if (self->state_.load() != PipelineState::Reconnecting) return FALSE;
    if (self->eos_received_.load()) return FALSE; // handle_eos already ran teardown
    self->health_.warn("EOS timeout on disconnect after 3 s");
    self->do_reconnect_teardown(false);
    return FALSE;
}

gboolean Pipeline::try_reconnect_cb(gpointer user_data) {
    auto* self = static_cast<Pipeline*>(user_data);
    if (self->state_.load() != PipelineState::Reconnecting) return FALSE;

    // Check if the device node is accessible
    if (access(self->cfg_.camera.device.c_str(), R_OK) != 0) {
        self->health_.info("Device not available yet, retrying...");
        self->schedule_retry();
        return FALSE; // one-shot; new timer added by schedule_retry
    }

    self->health_.info("Device available, reconnecting pipeline");

    // Build a fresh pipeline. Set fragment_index_start_ so the new splitmuxsink
    // (whose internal fragment_id resets to 0) continues numbering from where
    // we left off instead of overwriting existing segment files.
    self->fragment_index_start_ = self->fragment_index_.load();

    if (!self->build_pipeline()) {
        self->health_.error("Reconnect failed (build_pipeline failed), retrying");
        // build_pipeline may have partially constructed elements; clean up
        if (self->bus_watch_id_) { g_source_remove(self->bus_watch_id_); self->bus_watch_id_ = 0; }
        if (self->el_.pipeline) {
            gst_element_set_state(self->el_.pipeline, GST_STATE_NULL);
            gst_object_unref(self->el_.pipeline);
            self->el_ = {};
        }
        self->schedule_retry();
        return FALSE;
    }

    GstStateChangeReturn ret = gst_element_set_state(self->el_.pipeline, GST_STATE_PLAYING);
    if (ret == GST_STATE_CHANGE_FAILURE) {
        self->health_.error("Reconnect failed (set PLAYING returned FAILURE), retrying");
        if (self->bus_watch_id_) { g_source_remove(self->bus_watch_id_); self->bus_watch_id_ = 0; }
        gst_element_set_state(self->el_.pipeline, GST_STATE_NULL);
        gst_object_unref(self->el_.pipeline);
        self->el_ = {};
        self->schedule_retry();
    } else {
        self->reconnect_backoff_sec_ = 1; // reset backoff on success
        self->eos_received_.store(false);  // reset for the new pipeline
        self->set_state(PipelineState::Recording);
        self->health_.write_status(self->make_status());
    }
    return FALSE; // one-shot timer
}

void Pipeline::schedule_retry() {
    g_timeout_add_seconds(reconnect_backoff_sec_, try_reconnect_cb, this);
    reconnect_backoff_sec_ = std::min(reconnect_backoff_sec_ * 2u, 60u);
}

// ---------------------------------------------------------------------------
// Disk full poll
// ---------------------------------------------------------------------------

gboolean Pipeline::disk_poll_cb(gpointer user_data) {
    auto* self = static_cast<Pipeline*>(user_data);
    const auto cur_state = self->state_.load();

    if (cur_state == PipelineState::PausedDiskFull) {
        // Check if space has freed up
        if (!self->disk_monitor_.is_disk_full()) {
            self->health_.info("Disk space restored, resuming recording");
            GstStateChangeReturn ret = gst_element_set_state(self->el_.pipeline, GST_STATE_PLAYING);
            if (ret != GST_STATE_CHANGE_FAILURE) {
                self->eos_received_.store(false); // pipeline is live again
                self->set_state(PipelineState::Recording);
                self->health_.write_status(self->make_status());
            }
        }
        return TRUE; // keep timer running
    }

    if (cur_state != PipelineState::Recording) return TRUE;

    if (self->disk_monitor_.is_disk_full()) {
        self->health_.warn("Disk full (free < " +
                           std::to_string(self->cfg_.disk.min_free_bytes) +
                           " bytes), pausing recording");
        self->set_state(PipelineState::PausedDiskFull);
        // Send EOS to cleanly close the current segment; handle_eos() will pause the pipeline
        gst_element_send_event(self->el_.pipeline, gst_event_new_eos());
        self->health_.write_status(self->make_status());
    }

    return TRUE; // keep timer running
}

// ---------------------------------------------------------------------------
// format-location-full signal — called by splitmuxsink before opening fragment N
// At this point, fragment N-1 is fully closed.
// ---------------------------------------------------------------------------

gchar* Pipeline::on_format_location_full(GstElement* /*splitmux*/,
                                          guint fragment_id,
                                          GstSample* /*first_sample*/,
                                          gpointer user_data) {
    auto* self = static_cast<Pipeline*>(user_data);

    // splitmuxsink's internal fragment_id resets to 0 on each pipeline rebuild.
    // Offset it so segment filenames continue from where they left off.
    const uint32_t actual_idx = self->fragment_index_start_ + fragment_id;

    // Encrypt the previous (now-closed) segment. Use fragment_id (not actual_idx)
    // for the > 0 guard: when fragment_id==0 there is no prior segment from this
    // splitmuxsink instance; the partial segment from a prior disconnect (if any)
    // was already handled in schedule_reconnect().
    if (fragment_id > 0 && self->cfg_.encryption.enabled) {
        const std::string prev_path = utils::segment_filename(self->cfg_, self->session_id_, actual_idx - 1);
        if (fs::exists(prev_path)) {
            self->enqueue_encryption(prev_path);
        }
    }

    // Track segment stats
    const std::string new_path = utils::segment_filename(self->cfg_, self->session_id_, actual_idx);
    {
        std::lock_guard<std::mutex> lk(self->segment_mutex_);
        if (fragment_id > 0) {
            self->segments_written_.fetch_add(1);
            // Approximate bytes: get file size of just-closed segment
            std::error_code ec;
            const auto sz = fs::file_size(utils::segment_filename(self->cfg_, self->session_id_, actual_idx - 1), ec);
            if (!ec) self->bytes_written_.fetch_add(sz);
            self->last_segment_      = utils::segment_filename(self->cfg_, self->session_id_, actual_idx - 1);
            self->last_segment_time_ = utils::utc_timestamp_str();
        }
    }

    self->fragment_index_.store(actual_idx + 1);
    self->health_.debug("Opening segment " + new_path);
    self->health_.write_status(self->make_status());

    return g_strdup(new_path.c_str());
}

// ---------------------------------------------------------------------------
// Encryption worker
// ---------------------------------------------------------------------------

void Pipeline::enqueue_encryption(const std::string& plaintext_path) {
    {
        std::lock_guard<std::mutex> lk(enc_mutex_);
        enc_queue_.push(plaintext_path);
        enc_queue_depth_.store(static_cast<uint32_t>(enc_queue_.size()));
    }
    enc_cv_.notify_one();
}

void Pipeline::encryption_worker() {
    while (true) {
        std::string job;
        {
            std::unique_lock<std::mutex> lk(enc_mutex_);
            enc_cv_.wait(lk, [this] { return !enc_queue_.empty() || enc_stop_.load(); });
            if (enc_stop_.load() && enc_queue_.empty()) break;
            job = std::move(enc_queue_.front());
            enc_queue_.pop();
            enc_queue_depth_.store(static_cast<uint32_t>(enc_queue_.size()));
        }

        const fs::path src(job);
        const fs::path dst = fs::path(job).concat(".vcpenc");
        try {
            encrypt_file(src, dst, std::span<const uint8_t, 32>(key_.data(), 32));
            if (cfg_.encryption.delete_plaintext) {
                std::error_code ec;
                fs::remove(src, ec);
                if (ec) health_.warn("Failed to remove plaintext: " + job + ": " + ec.message());
            }
            health_.info("Encrypted: " + dst.string());
        } catch (const std::exception& e) {
            health_.error(std::string("Encryption failed for ") + job + ": " + e.what());
        }
    }
}

// ---------------------------------------------------------------------------
// Test hook
// ---------------------------------------------------------------------------

void Pipeline::post_error_for_test() {
    if (!el_.pipeline || !el_.src) return;
    GError* err = g_error_new(GST_RESOURCE_ERROR,
                              GST_RESOURCE_ERROR_READ,
                              "simulated camera disconnect (test)");
    gst_element_post_message(
        el_.src,
        gst_message_new_error(GST_OBJECT(el_.src), err, "injected by post_error_for_test"));
    g_error_free(err);
}

} // namespace vcp
