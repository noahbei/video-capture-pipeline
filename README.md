# vcpcapture

A GStreamer-based video capture daemon that records from V4L2 cameras into rotating, AES-256-GCM encrypted segments.

## Building

Install dependencies:

```bash
sudo apt install cmake build-essential pkg-config \
  libssl-dev libtomlplusplus-dev \
  libgstreamer1.0-dev libgstreamer-plugins-base1.0-dev \
  gstreamer1.0-plugins-good gstreamer1.0-plugins-bad \
  gstreamer1.0-plugins-ugly
```

Configure and build:

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build -j$(nproc)
```

Outputs:
- `build/vcpcapture` — capture daemon
- `build/vcpdecrypt` — standalone decrypt tool
- `build/tests/test_*` — test binaries

## Finding Your Camera's Device Info

Before editing `config.toml`, use `v4l2-ctl` (from the `v4l-utils` package) to find the correct values for your camera.

**Install v4l-utils if needed:**

```bash
sudo apt install v4l-utils
```

**List all connected video devices:**

```bash
v4l2-ctl --list-devices
```

Example output:
```
USB Camera: USB2.0 camera (usb-0000:01:00.3-4):
    /dev/video0
    /dev/video1
```

Use the first `/dev/videoN` path as the `device` value in `[camera]`.

**List supported formats, resolutions, and framerates for a device:**

```bash
v4l2-ctl --device=/dev/video0 --list-formats-ext
```

Example output:
```
[0]: 'MJPG' (Motion-JPEG, compressed)
    Size: Discrete 1280x720
        Interval: Discrete 0.033s (30.000 fps)
[1]: 'YUYV' (YUYV 4:2:2)
    Size: Discrete 1280x720
        Interval: Discrete 0.100s (10.000 fps)
    Size: Discrete 640x480
        Interval: Discrete 0.033s (30.000 fps)
```

Map these values to your `config.toml`:

| v4l2-ctl output | config.toml field | Example value |
|---|---|---|
| Device path | `[camera] device` | `/dev/video0` |
| Format code | `[camera] pixel_format` | `YUYV` or `MJPG` |
| Size | `[camera] width` / `height` | `1280` / `720` |
| Interval → fps | `[camera] framerate` | `30` |

> **Note:** `pixel_format` accepts both `YUYV` and `YUY2` as equivalent values.

## Running

**1. Generate an encryption key**

```bash
chmod +x ./scripts/gen_key.sh
./scripts/gen_key.sh enc.key
```

Store the key file on a separate, secure partition. Keeping it alongside recordings negates encryption protection.

**2. Create a config file**

Copy the example and adjust for your deployment:

```bash
cp config.toml.example config.toml
```

**3. Start the daemon**

```bash
./build/vcpcapture --config config.toml
```

Logs are written to stderr as JSON lines (compatible with `journald`). Monitor the health status file in a separate terminal:

```bash
watch cat /run/vcpcapture/status.json
```

## Keeping Plaintext Off Persistent Storage (tmpfs Staging)

By default segments are written as plaintext files and encrypted after each rotation. If your threat model requires that plaintext never reach the recording volume at all, point `output.temp_dir` at a `tmpfs` mount. Segments are written to RAM, the encrypted `.vcpenc` goes directly to `output.directory`, and the staging file is always deleted.

**1. Create and mount the tmpfs**

Size it to hold two segments simultaneously (one being written, one being encrypted).

```bash
sudo mkdir -p /run/vcpcapture/staging
sudo mount -t tmpfs -o size=256m tmpfs /run/vcpcapture/staging
```

To mount automatically on boot, add to `/etc/fstab`:
```
tmpfs /run/vcpcapture/staging tmpfs defaults,size=256m 0 0
```

General sizing formula:
```
(bitrate_kbps / 8) × max_duration_sec × 2 = bytes needed
```

**2. Set `temp_dir` in your config**

```toml
[output]
directory = "/var/capture"
temp_dir  = "/run/vcpcapture/staging"

[encryption]
enabled  = true          # required — temp_dir without encryption is rejected at startup
key_file = "/etc/vcpcapture/enc.key"
```

`delete_plaintext` has no effect when `temp_dir` is set — the staging file is always removed.

## Example Config

```toml
[camera]
device          = "/dev/video0"
width           = 1280
height          = 720
framerate       = 30
pixel_format    = "YUY2"

[encoder]
bitrate_kbps    = 2000
speed_preset    = "ultrafast"
tune            = "zerolatency"
key_int_max     = 30

[output]
directory       = "/var/capture"
container       = "mp4"
filename_pattern = "seg_%05d"

[rotation]
mode            = "duration"
max_duration_sec = 60

[encryption]
enabled         = true
key_file        = "/etc/vcpcapture/enc.key"
delete_plaintext = true

[backpressure]
queue_max_buffers = 200

[disk]
min_free_bytes  = 524288000
poll_interval_sec = 5

[health]
log_level       = "info"
status_file     = "/run/vcpcapture/status.json"
```

See `config.toml.example` for documentation on every field.

## Decrypting and Replaying Recordings

**Decrypt a single segment:**

```bash
./build/vcpdecrypt --key enc.key --in seg_00000.mp4.vcpenc --out seg_00000.mp4
```

**Decrypt an entire directory in bulk:**

```bash
./build/vcpdecrypt --key enc.key --dir /var/capture --out-dir /tmp/decrypted
```

**Play back a decrypted segment:**

```bash
gst-launch-1.0 filesrc location=seg_00000.mp4 ! qtdemux ! h264parse ! avdec_h264 ! autovideosink
# or with any standard player:
mpv seg_00000.mp4
vlc seg_00000.mp4
```

## Tests

```bash
# Unit tests (no hardware required)
ctest --test-dir build -R "test_encryptor|test_config" -V

# Backpressure integration test (GStreamer only, no camera)
ctest --test-dir build -R test_pipeline_backpressure -V

# Disk-full integration test (requires root)
sudo build/tests/test_disk_monitor

# Disconnect/reconnect state machine test (GStreamer only, no hardware)
ctest --test-dir build -R test_pipeline_reconnect -V
```
