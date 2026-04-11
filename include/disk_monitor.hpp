#pragma once

#include <cstdint>
#include <string>

namespace vcp {

class DiskMonitor {
public:
    // path: directory to monitor (checked via statvfs)
    // min_free_bytes: threshold below which is_disk_full() returns true
    DiskMonitor(const std::string& path, uint64_t min_free_bytes);

    // Returns true if free space on the filesystem containing path_ is below
    // min_free_bytes_. Safe to call frequently (single statvfs syscall).
    bool is_disk_full() const;

    // Returns the current free bytes, or 0 on error.
    uint64_t free_bytes() const;

private:
    std::string path_;
    uint64_t    min_free_bytes_;
};

} // namespace vcp
