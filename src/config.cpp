#include "config.hpp"

#include <cerrno>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <stdexcept>
#include <string>

#include <fcntl.h>
#include <linux/videodev2.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include <toml++/toml.hpp>

// ---------------------------------------------------------------------------
// V4L2 camera format validation helpers (file-scope, no external linkage)
// ---------------------------------------------------------------------------

namespace {

uint32_t pixel_format_to_v4l2(const std::string& fmt) {
    if (fmt == "MJPG")                    return V4L2_PIX_FMT_MJPEG;
    if (fmt == "YUY2" || fmt == "YUYV")   return V4L2_PIX_FMT_YUYV;
    if (fmt == "NV12")                    return V4L2_PIX_FMT_NV12;
    return 0; // unrecognised — skip validation
}

// Human-readable fps list for a format+size: "30fps 24fps 15fps"
std::string list_framerates(int fd, uint32_t pixfmt, uint32_t w, uint32_t h) {
    std::string out;
    struct v4l2_frmivalenum fival{};
    fival.pixel_format = pixfmt;
    fival.width  = w;
    fival.height = h;
    for (fival.index = 0; ioctl(fd, VIDIOC_ENUM_FRAMEINTERVALS, &fival) == 0; ++fival.index) {
        if (fival.type != V4L2_FRMIVAL_TYPE_DISCRETE) { out += "variable"; break; }
        if (!out.empty()) out += ' ';
        const uint32_t num = fival.discrete.numerator;
        const uint32_t den = fival.discrete.denominator;
        char buf[16];
        if (num && den % num == 0)
            std::snprintf(buf, sizeof(buf), "%ufps", den / num);
        else
            std::snprintf(buf, sizeof(buf), "%.1ffps", num ? (double)den / num : 0.0);
        out += buf;
    }
    return out.empty() ? "unknown" : out;
}

// True if integer fps matches any discrete interval (or any stepwise/continuous range).
bool framerate_supported(int fd, uint32_t pixfmt, uint32_t w, uint32_t h, int fps) {
    struct v4l2_frmivalenum fival{};
    fival.pixel_format = pixfmt;
    fival.width  = w;
    fival.height = h;
    for (fival.index = 0; ioctl(fd, VIDIOC_ENUM_FRAMEINTERVALS, &fival) == 0; ++fival.index) {
        if (fival.type != V4L2_FRMIVAL_TYPE_DISCRETE) return true;
        const uint32_t num = fival.discrete.numerator;
        const uint32_t den = fival.discrete.denominator;
        if (!num) continue;
        // Exact: den/num == fps  (e.g. 30/1 == 30)
        if (den == num * static_cast<uint32_t>(fps)) return true;
        // Near-integer: handles 30000/1001 ≈ 29.97 when user writes framerate=30
        if (std::abs(static_cast<double>(den) / num - fps) < 0.5) return true;
    }
    return false;
}

} // anonymous namespace

namespace vcp {

// Open the V4L2 device and verify that pixel_format, width×height, and framerate
// are all supported. Throws std::runtime_error with a descriptive message on mismatch.
// Silently skips if the format is unrecognised or the device cannot be opened
// (the pipeline will produce a clearer error when it tries to start).
static void validate_camera_format(const CameraConfig& cam) {
    const uint32_t want_pixfmt = pixel_format_to_v4l2(cam.pixel_format);
    if (!want_pixfmt) return;

    const int fd = open(cam.device.c_str(), O_RDONLY | O_NONBLOCK);
    if (fd < 0) return; // device absent — defer to pipeline

    // --- pixel format ---
    {
        struct v4l2_fmtdesc fmtdesc{};
        fmtdesc.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        bool found = false;
        for (fmtdesc.index = 0; ioctl(fd, VIDIOC_ENUM_FMT, &fmtdesc) == 0; ++fmtdesc.index)
            if (fmtdesc.pixelformat == want_pixfmt) { found = true; break; }
        if (!found) {
            close(fd);
            throw std::runtime_error(
                cam.device + " does not support pixel format " + cam.pixel_format);
        }
    }

    // --- resolution and framerate ---
    bool size_matched = false;
    bool rate_matched = false;
    std::string all_modes;
    std::string matched_fps_list;

    struct v4l2_frmsizeenum sz{};
    sz.pixel_format = want_pixfmt;
    for (sz.index = 0; ioctl(fd, VIDIOC_ENUM_FRAMESIZES, &sz) == 0; ++sz.index) {
        if (sz.type == V4L2_FRMSIZE_TYPE_DISCRETE) {
            const uint32_t w = sz.discrete.width;
            const uint32_t h = sz.discrete.height;
            const std::string fps_list = list_framerates(fd, want_pixfmt, w, h);
            all_modes += "  " + std::to_string(w) + "x" + std::to_string(h)
                       + ": " + fps_list + "\n";
            if (static_cast<int>(w) == cam.width && static_cast<int>(h) == cam.height) {
                size_matched     = true;
                matched_fps_list = fps_list;
                rate_matched     = framerate_supported(fd, want_pixfmt, w, h, cam.framerate);
            }
        } else {
            // Stepwise/continuous — accept any size and framerate in range
            size_matched = rate_matched = true;
        }
    }

    close(fd);

    if (!size_matched) {
        throw std::runtime_error(
            cam.device + " does not support " + cam.pixel_format
            + " at " + std::to_string(cam.width) + "x" + std::to_string(cam.height) + "\n"
            + "supported " + cam.pixel_format + " modes:\n" + all_modes);
    }
    if (!rate_matched) {
        throw std::runtime_error(
            cam.device + " does not support " + cam.pixel_format
            + " at " + std::to_string(cam.width) + "x" + std::to_string(cam.height)
            + "@" + std::to_string(cam.framerate) + "fps\n"
            + "supported framerates at this resolution: " + matched_fps_list);
    }
}

// Helper: throw if a required string field is empty after parsing.
static void require_nonempty(const std::string& val, const char* key) {
    if (val.empty()) {
        throw std::runtime_error(std::string("config: required key '") + key + "' is missing or empty");
    }
}

Config load_config(const std::string& path) {
    toml::table tbl;
    try {
        tbl = toml::parse_file(path);
    } catch (const toml::parse_error& e) {
        throw std::runtime_error(std::string("config parse error: ") + e.what());
    }

    Config cfg;

    // [camera]
    if (auto cam = tbl["camera"].as_table()) {
        if (auto v = (*cam)["device"].value<std::string>())        cfg.camera.device       = *v;
        if (auto v = (*cam)["width"].value<int64_t>())             cfg.camera.width        = static_cast<int>(*v);
        if (auto v = (*cam)["height"].value<int64_t>())            cfg.camera.height       = static_cast<int>(*v);
        if (auto v = (*cam)["framerate"].value<int64_t>())         cfg.camera.framerate    = static_cast<int>(*v);
        if (auto v = (*cam)["pixel_format"].value<std::string>())  cfg.camera.pixel_format = *v;
    }

    // [encoder]
    if (auto enc = tbl["encoder"].as_table()) {
        if (auto v = (*enc)["bitrate_kbps"].value<int64_t>())      cfg.encoder.bitrate_kbps = static_cast<int>(*v);
        if (auto v = (*enc)["speed_preset"].value<std::string>())  cfg.encoder.speed_preset = *v;
        if (auto v = (*enc)["tune"].value<std::string>())          cfg.encoder.tune         = *v;
        if (auto v = (*enc)["key_int_max"].value<int64_t>())       cfg.encoder.key_int_max  = static_cast<int>(*v);
    }

    // [output]
    if (auto out = tbl["output"].as_table()) {
        if (auto v = (*out)["directory"].value<std::string>())        cfg.output.directory        = *v;
        if (auto v = (*out)["container"].value<std::string>())        cfg.output.container        = *v;
        if (auto v = (*out)["filename_pattern"].value<std::string>()) cfg.output.filename_pattern = *v;
    }

    // [rotation]
    if (auto rot = tbl["rotation"].as_table()) {
        if (auto v = (*rot)["mode"].value<std::string>())           cfg.rotation.mode             = *v;
        if (auto v = (*rot)["max_size_bytes"].value<int64_t>())     cfg.rotation.max_size_bytes   = static_cast<uint64_t>(*v);
        if (auto v = (*rot)["max_duration_sec"].value<int64_t>())   cfg.rotation.max_duration_sec = static_cast<uint64_t>(*v);
    }

    // [encryption]
    if (auto encr = tbl["encryption"].as_table()) {
        if (auto v = (*encr)["enabled"].value<bool>())              cfg.encryption.enabled          = *v;
        if (auto v = (*encr)["key_file"].value<std::string>())      cfg.encryption.key_file         = *v;
        if (auto v = (*encr)["delete_plaintext"].value<bool>())     cfg.encryption.delete_plaintext = *v;
    }

    // [backpressure]
    if (auto bp = tbl["backpressure"].as_table()) {
        if (auto v = (*bp)["queue_max_buffers"].value<int64_t>())   cfg.backpressure.queue_max_buffers = static_cast<int>(*v);
    }

    // [disk]
    if (auto disk = tbl["disk"].as_table()) {
        if (auto v = (*disk)["min_free_bytes"].value<int64_t>())    cfg.disk.min_free_bytes    = static_cast<uint64_t>(*v);
        if (auto v = (*disk)["poll_interval_sec"].value<int64_t>()) cfg.disk.poll_interval_sec = static_cast<int>(*v);
    }

    // [health]
    if (auto h = tbl["health"].as_table()) {
        if (auto v = (*h)["log_level"].value<std::string>())        cfg.health.log_level   = *v;
        if (auto v = (*h)["status_file"].value<std::string>())      cfg.health.status_file = *v;
    }

    // Validation
    require_nonempty(cfg.camera.device, "camera.device");
    require_nonempty(cfg.output.directory, "output.directory");

    if (cfg.output.container != "mp4" && cfg.output.container != "mkv") {
        throw std::runtime_error("config: output.container must be 'mp4' or 'mkv'");
    }
    if (cfg.rotation.mode != "duration" && cfg.rotation.mode != "size") {
        throw std::runtime_error("config: rotation.mode must be 'duration' or 'size'");
    }
    if (cfg.rotation.mode == "duration" && cfg.rotation.max_duration_sec == 0) {
        throw std::runtime_error("config: rotation.max_duration_sec must be > 0 when mode is 'duration'");
    }
    if (cfg.rotation.mode == "size" && cfg.rotation.max_size_bytes == 0) {
        throw std::runtime_error("config: rotation.max_size_bytes must be > 0 when mode is 'size'");
    }
    if (cfg.encryption.enabled) {
        require_nonempty(cfg.encryption.key_file, "encryption.key_file");
    }
    if (cfg.camera.width <= 0 || cfg.camera.height <= 0) {
        throw std::runtime_error("config: camera width and height must be positive");
    }
    if (cfg.camera.framerate <= 0) {
        throw std::runtime_error("config: camera.framerate must be positive");
    }

    validate_camera_format(cfg.camera);

    return cfg;
}

void load_key_file(const std::string& path, uint8_t out[32]) {
    std::ifstream f(path, std::ios::binary);
    if (!f.is_open()) {
        throw std::runtime_error("key file not found: " + path);
    }
    f.read(reinterpret_cast<char*>(out), 32);
    if (f.gcount() != 32) {
        throw std::runtime_error("key file must be exactly 32 bytes: " + path);
    }
    // Ensure there are no extra bytes
    char extra;
    if (f.read(&extra, 1) && f.gcount() > 0) {
        throw std::runtime_error("key file must be exactly 32 bytes (found more): " + path);
    }
}

} // namespace vcp
