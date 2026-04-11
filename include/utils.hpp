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
// If session_id is non-empty the filename is prefixed: "20260412T142205_seg_00042.mp4"
inline std::string segment_filename(const Config& cfg,
                                    const std::string& session_id,
                                    uint32_t index) {
    const char* ext = (cfg.output.container == "mkv") ? ".mkv" : ".mp4";
    char namebuf[256];
    // filename_pattern is a printf format string with one %...d conversion
    std::snprintf(namebuf, sizeof(namebuf),
                  cfg.output.filename_pattern.c_str(), index);
    const std::string prefix = session_id.empty() ? "" : session_id + "_";
    return cfg.output.directory + "/" + prefix + namebuf + ext;
}

// Returns the encrypted counterpart path: always in output.directory, never in temp_dir.
inline std::string encrypted_filename(const Config& cfg,
                                      const std::string& session_id,
                                      uint32_t index) {
    return segment_filename(cfg, session_id, index) + ".vcpenc";
}

// Returns the path where the muxer should write a segment.
// When output.temp_dir is set the file lands there (typically a tmpfs mount);
// otherwise falls back to segment_filename (output.directory).
inline std::string staging_filename(const Config& cfg,
                                    const std::string& session_id,
                                    uint32_t index) {
    if (cfg.output.temp_dir.empty())
        return segment_filename(cfg, session_id, index);
    const char* ext = (cfg.output.container == "mkv") ? ".mkv" : ".mp4";
    char namebuf[256];
    std::snprintf(namebuf, sizeof(namebuf),
                  cfg.output.filename_pattern.c_str(), index);
    const std::string prefix = session_id.empty() ? "" : session_id + "_";
    return cfg.output.temp_dir + "/" + prefix + namebuf + ext;
}

} // namespace vcp::utils
