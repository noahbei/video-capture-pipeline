#pragma once

#include <atomic>
#include <cstdint>
#include <string>

namespace vcp {

enum class LogLevel { Debug = 0, Info, Warn, Error };

struct HealthStatus {
    std::string state;              // "starting"|"recording"|"paused_disk_full"|"reconnecting"|"stopping"|"stopped"
    uint64_t    uptime_sec         = 0;
    uint64_t    segments_written   = 0;
    uint64_t    bytes_written      = 0;
    std::string last_segment;
    std::string last_segment_time;
    uint64_t    disk_free_bytes    = 0;
    uint32_t    reconnect_attempts = 0;
    uint32_t    enc_queue_depth    = 0;
};

class Health {
public:
    explicit Health(const std::string& status_file_path, LogLevel min_level = LogLevel::Info);

    void log(LogLevel level, const std::string& msg) const;
    void write_status(const HealthStatus& s) const;

    // Convenience wrappers
    void debug(const std::string& msg) const { log(LogLevel::Debug, msg); }
    void info(const std::string& msg)  const { log(LogLevel::Info,  msg); }
    void warn(const std::string& msg)  const { log(LogLevel::Warn,  msg); }
    void error(const std::string& msg) const { log(LogLevel::Error, msg); }

private:
    std::string status_file_;
    LogLevel    min_level_;
};

// Parse a log level string (case-insensitive). Defaults to Info on unknown input.
LogLevel parse_log_level(const std::string& s);

} // namespace vcp
