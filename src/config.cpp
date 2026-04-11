#include "config.hpp"

#include <cerrno>
#include <cstring>
#include <fstream>
#include <stdexcept>

#include <toml++/toml.hpp>

namespace vcp {

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
