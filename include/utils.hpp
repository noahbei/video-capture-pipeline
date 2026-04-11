#pragma once

#include "config.hpp"

#include <cstdio>
#include <ctime>
#include <string>

namespace vcp::utils {

// Returns current UTC time as ISO-8601 string: "2026-04-10T14:22:05Z"
inline std::string utc_timestamp_str() {
    std::time_t now = std::time(nullptr);
    struct tm tm_buf{};
    gmtime_r(&now, &tm_buf);
    char buf[32];
    std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", &tm_buf);
    return buf;
}

// Returns the full path for segment index, e.g. "/var/capture/seg_00042.mp4"
inline std::string segment_filename(const Config& cfg, uint32_t index) {
    const char* ext = (cfg.output.container == "mkv") ? ".mkv" : ".mp4";
    char namebuf[256];
    // filename_pattern is a printf format string with one %...d conversion
    std::snprintf(namebuf, sizeof(namebuf),
                  cfg.output.filename_pattern.c_str(), index);
    return cfg.output.directory + "/" + namebuf + ext;
}

// Returns the encrypted counterpart path: same as segment_filename + ".vcpenc"
inline std::string encrypted_filename(const Config& cfg, uint32_t index) {
    return segment_filename(cfg, index) + ".vcpenc";
}

} // namespace vcp::utils
