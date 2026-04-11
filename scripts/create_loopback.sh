#!/usr/bin/env bash
# create_loopback.sh — set up a v4l2loopback virtual camera for integration testing
#
# Creates /dev/video10 and feeds it with a videotestsrc pattern.
# Requires: v4l2loopback-dkms and root privileges.
#
# Usage: sudo ./scripts/create_loopback.sh
# Stop:  kill the gst-launch process (PID is printed) or press Ctrl+C

set -euo pipefail

DEVICE_NR=10
CARD_LABEL="vcptest"
DEVICE="/dev/video${DEVICE_NR}"
FORMAT="YUY2"
WIDTH=640
HEIGHT=480
FRAMERATE=15

# Load v4l2loopback if not already loaded
if ! lsmod | grep -q v4l2loopback; then
    echo "Loading v4l2loopback module..."
    modprobe v4l2loopback \
        devices=1 \
        video_nr="${DEVICE_NR}" \
        card_label="${CARD_LABEL}"
    sleep 1
fi

if [[ ! -e "$DEVICE" ]]; then
    echo "ERROR: $DEVICE not found after modprobe"
    exit 1
fi

echo "v4l2loopback device ready: $DEVICE"
echo ""
echo "Starting videotestsrc feeder (PID will be printed)..."
echo "Press Ctrl+C to stop."
echo ""

# Feed the loopback device with a colour bar pattern
gst-launch-1.0 -v \
    videotestsrc is-live=true pattern=smpte \
    ! "video/x-raw,format=${FORMAT},width=${WIDTH},height=${HEIGHT},framerate=${FRAMERATE}/1" \
    ! v4l2sink device="${DEVICE}" &

FEEDER_PID=$!
echo "Feeder PID: $FEEDER_PID"
echo "To stop:   kill $FEEDER_PID"

wait "$FEEDER_PID"
