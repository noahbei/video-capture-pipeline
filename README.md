# vcpcapture

A GStreamer-based video capture daemon that records from V4L2 cameras into rotating, AES-256-GCM encrypted segments.

## Building

Install dependencies:

```bash
sudo apt install libssl-dev libtomlplusplus-dev \
  libgstreamer1.0-dev libgstreamer-plugins-base1.0-dev \
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

## Running

**1. Generate an encryption key**

```bash
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
