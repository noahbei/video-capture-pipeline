// Integration test for DiskMonitor.
// Requires: CAP_SYS_ADMIN (to mount tmpfs) or run as root.
// Labeled "integration" in CMake — excluded from plain `ctest` runs.
//
// The test mounts a small tmpfs, verifies is_disk_full() behavior,
// then unmounts it.

#include "disk_monitor.hpp"

#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>

#include <sys/mount.h>
#include <unistd.h>

namespace fs = std::filesystem;

static const char* MOUNT_POINT = "/tmp/vcptest_diskmon";
static const size_t TMPFS_SIZE = 10 * 1024 * 1024; // 10 MB

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

static bool setup_tmpfs() {
    fs::create_directories(MOUNT_POINT);
    // "size=10m" limits the tmpfs to 10 MiB
    if (mount("tmpfs", MOUNT_POINT, "tmpfs", 0, "size=10m") != 0) {
        std::perror("mount tmpfs");
        return false;
    }
    return true;
}

static void teardown_tmpfs() {
    if (umount(MOUNT_POINT) != 0) {
        std::perror("umount tmpfs");
    }
    std::error_code ec;
    fs::remove(MOUNT_POINT, ec);
}

static void fill_file(const fs::path& p, size_t bytes) {
    std::ofstream f(p, std::ios::binary | std::ios::trunc);
    std::string block(4096, '\xAA');
    size_t written = 0;
    while (written < bytes) {
        size_t chunk = std::min(block.size(), bytes - written);
        f.write(block.data(), static_cast<std::streamsize>(chunk));
        written += chunk;
    }
}

// ---------------------------------------------------------------------------
// Tests
// ---------------------------------------------------------------------------

static void test_not_full_initially() {
    std::printf("test_not_full_initially... ");
    // With threshold = 9 MB and 10 MB tmpfs, initially we are NOT full
    vcp::DiskMonitor dm(MOUNT_POINT, 9 * 1024 * 1024);
    assert(!dm.is_disk_full() && "should not be full initially");
    assert(dm.free_bytes() > 0);
    std::printf("PASS\n");
}

static void test_becomes_full() {
    std::printf("test_becomes_full... ");
    // threshold = 2 MB; fill 9 MB → only ~1 MB remains → should be full
    vcp::DiskMonitor dm(MOUNT_POINT, 2 * 1024 * 1024);
    fill_file(fs::path(MOUNT_POINT) / "fill.bin", 9 * 1024 * 1024);
    assert(dm.is_disk_full() && "should be full after writing 9 MB");
    std::printf("PASS\n");
}

static void test_recovers_after_delete() {
    std::printf("test_recovers_after_delete... ");
    // Remove the big file; should no longer be full
    vcp::DiskMonitor dm(MOUNT_POINT, 2 * 1024 * 1024);
    std::error_code ec;
    fs::remove(fs::path(MOUNT_POINT) / "fill.bin", ec);
    assert(!dm.is_disk_full() && "should recover after deleting fill file");
    std::printf("PASS\n");
}

// ---------------------------------------------------------------------------
// main
// ---------------------------------------------------------------------------

int main() {
    if (geteuid() != 0) {
        std::fprintf(stderr,
            "SKIP: test_disk_monitor requires root (or CAP_SYS_ADMIN) to mount tmpfs.\n"
            "Run with: sudo ./test_disk_monitor\n");
        // Return 0 so CTest doesn't mark it as a failure when skipped
        return 0;
    }

    std::printf("=== test_disk_monitor ===\n");

    if (!setup_tmpfs()) {
        std::fprintf(stderr, "FAIL: could not set up tmpfs mount at %s\n", MOUNT_POINT);
        return 1;
    }

    try {
        test_not_full_initially();
        test_becomes_full();
        test_recovers_after_delete();
    } catch (...) {
        teardown_tmpfs();
        throw;
    }

    teardown_tmpfs();
    std::printf("All tests PASSED\n");
    return 0;
}
