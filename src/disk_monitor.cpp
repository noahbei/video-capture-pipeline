#include "disk_monitor.hpp"

#include <sys/statvfs.h>

namespace vcp {

DiskMonitor::DiskMonitor(const std::string& path, uint64_t min_free_bytes)
    : path_(path), min_free_bytes_(min_free_bytes) {}

uint64_t DiskMonitor::free_bytes() const {
    struct statvfs st{};
    if (statvfs(path_.c_str(), &st) != 0) {
        return 0;
    }
    return static_cast<uint64_t>(st.f_bavail) * static_cast<uint64_t>(st.f_frsize);
}

bool DiskMonitor::is_disk_full() const {
    return free_bytes() < min_free_bytes_;
}

} // namespace vcp
