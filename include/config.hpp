#pragma once

#include <cstdint>
#include <string>

namespace vcp {

struct CameraConfig {
    std::string device        = "/dev/video0";
    int         width         = 1280;
    int         height        = 720;
    int         framerate     = 30;
    std::string pixel_format  = "YUY2";
    // Override the GStreamer source element factory name. Empty → "v4l2src".
    // Set to "videotestsrc" in unit/integration tests that must run without hardware.
    std::string src_element;
};

struct EncoderConfig {
    int         bitrate_kbps  = 2000;
    std::string speed_preset  = "ultrafast";
    std::string tune          = "zerolatency";
    int         key_int_max   = 30;
};

struct OutputConfig {
    std::string directory        = "/var/capture";
    std::string container        = "mp4";     // "mp4" or "mkv"
    std::string filename_pattern = "seg_%05d"; // printf index; extension appended
};

struct RotationConfig {
    std::string mode             = "duration"; // "duration" or "size"
    uint64_t    max_size_bytes   = 0;
    uint64_t    max_duration_sec = 60;
};

struct EncryptionConfig {
    bool        enabled          = true;
    std::string key_file;
    bool        delete_plaintext = true;
};

struct BackpressureConfig {
    int queue_max_buffers = 200;
};

struct DiskConfig {
    uint64_t min_free_bytes    = 524288000; // 500 MB
    int      poll_interval_sec = 5;
};

struct HealthConfig {
    std::string log_level   = "info";
    std::string status_file = "/run/vcpcapture/status.json";
};

struct Config {
    CameraConfig      camera;
    EncoderConfig     encoder;
    OutputConfig      output;
    RotationConfig    rotation;
    EncryptionConfig  encryption;
    BackpressureConfig backpressure;
    DiskConfig        disk;
    HealthConfig      health;
};

// Parse a TOML config file. Throws std::runtime_error on validation failure.
Config load_config(const std::string& path);

// Read exactly 32 bytes from a key file into out[32].
// Throws std::runtime_error if file is missing or not exactly 32 bytes.
void load_key_file(const std::string& path, uint8_t out[32]);

} // namespace vcp
