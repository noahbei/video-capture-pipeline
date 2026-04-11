#include "health.hpp"
#include "utils.hpp"

#include <algorithm>
#include <cinttypes>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>

namespace vcp {

namespace fs = std::filesystem;

Health::Health(const std::string& status_file_path, LogLevel min_level)
    : status_file_(status_file_path), min_level_(min_level) {}

LogLevel parse_log_level(const std::string& s) {
    std::string lower = s;
    std::transform(lower.begin(), lower.end(), lower.begin(), ::tolower);
    if (lower == "debug") return LogLevel::Debug;
    if (lower == "warn")  return LogLevel::Warn;
    if (lower == "error") return LogLevel::Error;
    return LogLevel::Info;
}

static const char* level_str(LogLevel l) {
    switch (l) {
        case LogLevel::Debug: return "debug";
        case LogLevel::Info:  return "info";
        case LogLevel::Warn:  return "warn";
        case LogLevel::Error: return "error";
    }
    return "info";
}

void Health::log(LogLevel level, const std::string& msg) const {
    if (level < min_level_) return;

    // Escape any double-quotes in msg to keep JSON valid
    std::string escaped;
    escaped.reserve(msg.size());
    for (char c : msg) {
        if (c == '"')       escaped += "\\\"";
        else if (c == '\\') escaped += "\\\\";
        else if (c == '\n') escaped += "\\n";
        else                escaped += c;
    }

    std::fprintf(stderr, "{\"ts\":\"%s\",\"level\":\"%s\",\"msg\":\"%s\"}\n",
                 utils::utc_timestamp_str().c_str(),
                 level_str(level),
                 escaped.c_str());
}

void Health::write_status(const HealthStatus& s) const {
    if (status_file_.empty()) return;

    // Build JSON into a stack buffer
    char buf[1024];
    int n = std::snprintf(buf, sizeof(buf),
        "{\n"
        "  \"state\": \"%s\",\n"
        "  \"uptime_sec\": %" PRIu64 ",\n"
        "  \"segments_written\": %" PRIu64 ",\n"
        "  \"bytes_written\": %" PRIu64 ",\n"
        "  \"last_segment\": \"%s\",\n"
        "  \"last_segment_time\": \"%s\",\n"
        "  \"disk_free_bytes\": %" PRIu64 ",\n"
        "  \"reconnect_attempts\": %" PRIu32 ",\n"
        "  \"enc_queue_depth\": %" PRIu32 "\n"
        "}\n",
        s.state.c_str(),
        s.uptime_sec,
        s.segments_written,
        s.bytes_written,
        s.last_segment.c_str(),
        s.last_segment_time.c_str(),
        s.disk_free_bytes,
        s.reconnect_attempts,
        s.enc_queue_depth);

    if (n < 0 || static_cast<size_t>(n) >= sizeof(buf)) {
        // Fallback: write a minimal error status
        std::snprintf(buf, sizeof(buf),
            "{\"state\":\"error\",\"msg\":\"status buffer overflow\"}\n");
    }

    // Ensure parent directory exists
    try {
        fs::create_directories(fs::path(status_file_).parent_path());
    } catch (...) {
        // Non-fatal: if we can't create the dir, the write below will fail and we just skip
    }

    std::string tmp_path = status_file_ + ".tmp";
    {
        std::ofstream f(tmp_path, std::ios::trunc);
        if (!f.is_open()) return; // Non-fatal: skip status update
        f << buf;
    }

    // Atomic replace
    std::error_code ec;
    fs::rename(tmp_path, status_file_, ec);
    // If rename fails (e.g. cross-device), fall back to overwrite
    if (ec) {
        fs::copy_file(tmp_path, status_file_,
                      fs::copy_options::overwrite_existing, ec);
        fs::remove(tmp_path, ec);
    }
}

} // namespace vcp
