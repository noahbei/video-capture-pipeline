#!/usr/bin/env bash
# make_tmpfs.sh — mount a size-limited tmpfs for disk-full integration testing
#
# Usage: sudo ./scripts/make_tmpfs.sh [size] [mount_point]
#   size:        tmpfs size (default: 50m). Accepts: 10m, 100m, 1g, etc.
#   mount_point: where to mount (default: /tmp/vcptest_disk)
#
# To unmount: sudo umount /tmp/vcptest_disk

set -euo pipefail

SIZE="${1:-50m}"
MOUNT_POINT="${2:-/tmp/vcptest_disk}"

if mountpoint -q "$MOUNT_POINT" 2>/dev/null; then
    echo "Already mounted at $MOUNT_POINT, unmounting first..."
    umount "$MOUNT_POINT"
fi

mkdir -p "$MOUNT_POINT"
mount -t tmpfs -o "size=${SIZE}" tmpfs "$MOUNT_POINT"
echo "Mounted ${SIZE} tmpfs at $MOUNT_POINT"
echo ""
echo "Use this as output.directory in your config."
echo "To unmount: sudo umount $MOUNT_POINT"
echo ""
df -h "$MOUNT_POINT"
