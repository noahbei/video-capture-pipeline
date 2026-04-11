// Integration test: backpressure / drop-oldest behavior.
// Uses videotestsrc (no camera hardware required) and a slow fakesink.
// Verifies that the queue never exceeds its max-size-buffers limit,
// and that frames are still received (i.e. pipeline does not deadlock).
//
// Requires: GStreamer with gst-plugins-base (videotestsrc, fakesink, queue)
// Labeled "integration" in CMake.

#include <cassert>
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <string>

#include <gst/gst.h>

static const int   QUEUE_MAX_BUFFERS = 10;
static const int   TEST_DURATION_SEC = 3;
static const guint64 FAKE_SLEEP_NS   = 50 * 1000 * 1000; // 50 ms per frame (= ~20 fps max drain)
// Source will produce 30 fps; consumer drains ~20 fps → should drop ~1/3 of frames

// ---------------------------------------------------------------------------
// Per-frame callback on fakesink to count received frames and simulate slowness
// ---------------------------------------------------------------------------

struct TestCtx {
    GstElement* pipeline      = nullptr;
    GstElement* queue_elem    = nullptr;
    GstElement* fakesink      = nullptr;
    GMainLoop*  loop          = nullptr;
    guint64     frames_received = 0;
    int         max_queue_seen  = 0;
};

static GstPadProbeReturn on_fakesink_probe(GstPad* /*pad*/,
                                           GstPadProbeInfo* /*info*/,
                                           gpointer user_data) {
    auto* ctx = static_cast<TestCtx*>(user_data);
    ctx->frames_received++;

    // Check current queue depth
    guint cur = 0;
    g_object_get(ctx->queue_elem, "current-level-buffers", &cur, nullptr);
    if (static_cast<int>(cur) > ctx->max_queue_seen)
        ctx->max_queue_seen = static_cast<int>(cur);

    // Simulate slow consumer
    g_usleep(FAKE_SLEEP_NS / 1000); // g_usleep takes microseconds

    return GST_PAD_PROBE_OK;
}

static gboolean stop_pipeline_cb(gpointer user_data) {
    auto* ctx = static_cast<TestCtx*>(user_data);
    gst_element_send_event(ctx->pipeline, gst_event_new_eos());
    return FALSE; // one-shot
}

static gboolean bus_cb(GstBus* /*bus*/, GstMessage* msg, gpointer user_data) {
    auto* ctx = static_cast<TestCtx*>(user_data);
    if (GST_MESSAGE_TYPE(msg) == GST_MESSAGE_EOS ||
        GST_MESSAGE_TYPE(msg) == GST_MESSAGE_ERROR) {
        g_main_loop_quit(ctx->loop);
    }
    return TRUE;
}

// ---------------------------------------------------------------------------
// main
// ---------------------------------------------------------------------------

int main(int argc, char* argv[]) {
    gst_init(&argc, &argv);

    TestCtx ctx;
    ctx.loop = g_main_loop_new(nullptr, FALSE);

    // Build pipeline:
    // videotestsrc is-live=true framerate=30/1
    //   → capsfilter video/x-raw,framerate=30/1
    //   → queue max-size-buffers=10 leaky=2
    //   → fakesink sync=false async=false
    ctx.pipeline = gst_pipeline_new("backpressure_test");
    GstElement* src      = gst_element_factory_make("videotestsrc", "src");
    GstElement* caps_f   = gst_element_factory_make("capsfilter",   "caps");
    ctx.queue_elem       = gst_element_factory_make("queue",        "queue");
    ctx.fakesink         = gst_element_factory_make("fakesink",     "sink");

    if (!src || !caps_f || !ctx.queue_elem || !ctx.fakesink) {
        std::fprintf(stderr, "FAIL: could not create pipeline elements.\n"
                             "Ensure gst-plugins-base is installed.\n");
        return 1;
    }

    g_object_set(src, "is-live", TRUE, "pattern", 0, nullptr);

    GstCaps* caps = gst_caps_from_string("video/x-raw,framerate=30/1");
    g_object_set(caps_f, "caps", caps, nullptr);
    gst_caps_unref(caps);

    g_object_set(ctx.queue_elem,
                 "max-size-buffers", (guint)QUEUE_MAX_BUFFERS,
                 "leaky",            (guint)2,   // downstream = drop oldest
                 "max-size-bytes",   (guint64)0,
                 "max-size-time",    (guint64)0,
                 nullptr);

    g_object_set(ctx.fakesink, "sync", FALSE, "async", FALSE, nullptr);

    gst_bin_add_many(GST_BIN(ctx.pipeline), src, caps_f, ctx.queue_elem, ctx.fakesink, nullptr);
    if (!gst_element_link_many(src, caps_f, ctx.queue_elem, ctx.fakesink, nullptr)) {
        std::fprintf(stderr, "FAIL: could not link elements\n");
        return 1;
    }

    // Install probe on fakesink sink pad to count frames + slow down
    GstPad* sinkpad = gst_element_get_static_pad(ctx.fakesink, "sink");
    gst_pad_add_probe(sinkpad, GST_PAD_PROBE_TYPE_BUFFER, on_fakesink_probe, &ctx, nullptr);
    gst_object_unref(sinkpad);

    // Bus watch
    GstBus* bus = gst_element_get_bus(ctx.pipeline);
    gst_bus_add_watch(bus, bus_cb, &ctx);
    gst_object_unref(bus);

    // Timer to stop after TEST_DURATION_SEC
    g_timeout_add_seconds(TEST_DURATION_SEC, stop_pipeline_cb, &ctx);

    // Start
    gst_element_set_state(ctx.pipeline, GST_STATE_PLAYING);
    g_main_loop_run(ctx.loop);

    gst_element_set_state(ctx.pipeline, GST_STATE_NULL);
    gst_object_unref(ctx.pipeline);
    g_main_loop_unref(ctx.loop);

    std::printf("Frames received:   %" G_GUINT64_FORMAT "\n", ctx.frames_received);
    std::printf("Max queue depth:   %d (limit: %d)\n", ctx.max_queue_seen, QUEUE_MAX_BUFFERS);

    // Assertions
    assert(ctx.frames_received > 0 && "pipeline deadlocked: no frames received");
    assert(ctx.max_queue_seen <= QUEUE_MAX_BUFFERS &&
           "queue exceeded max-size-buffers — leaky not working");

    // At 30 fps source and ~20 fps drain over 3 seconds, expect at least 50 frames received
    assert(ctx.frames_received >= 50 &&
           "too few frames received — possible deadlock or misconfiguration");

    std::printf("test_pipeline_backpressure PASSED\n");
    return 0;
}
