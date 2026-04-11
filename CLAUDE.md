# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Build

```bash
# Configure (first time or after CMakeLists.txt changes)
cmake -B build -DCMAKE_BUILD_TYPE=Debug

# Build all targets
cmake --build build -j$(nproc)

# Produces:
#   build/vcpcapture        — main capture daemon
#   build/vcpdecrypt        — standalone decrypt tool
#   build/tests/test_*      — test binaries
```

Required packages: `libssl-dev libtomlplusplus-dev libgstreamer1.0-dev libgstreamer-plugins-base1.0-dev gstreamer1.0-plugins-good gstreamer1.0-plugins-bad gstreamer1.0-plugins-ugly`

## Tests

```bash
# Unit tests only (no hardware, no root)
ctest --test-dir build -R "test_encryptor|test_config" -V

# Backpressure integration test (GStreamer only, no camera)
ctest --test-dir build -R test_pipeline_backpressure -V

# Disk-full integration test (requires root to mount tmpfs)
sudo build/tests/test_disk_monitor

# Camera disconnect test (requires v4l2loopback + root)
ctest --test-dir build -R test_pipeline_reconnect -V

# Run a single test binary directly
./build/tests/test_encryptor
```

Tests use plain `assert()` and return codes — no test framework dependency.

## Running vcpcapture

```bash
# Generate a key
./scripts/gen_key.sh enc.key

# Run (logs to stderr as JSON lines)
./build/vcpcapture --config config.toml

# Monitor health
watch cat /run/vcpcapture/status.json

# Decrypt a recording
./build/vcpdecrypt --key enc.key --in seg_00000.mp4.vcpenc --out seg_00000.mp4
./build/vcpdecrypt --key enc.key --dir /var/capture --out-dir /tmp/decrypted
```

## Architecture

All code lives in the `vcp` namespace. The build produces a static library `vcpcore` (everything except `main.cpp` and `vcpdecrypt.cpp`) so that test binaries can link individual modules without pulling in the full binary.

### Module map

| Module | Files | Responsibility |
|---|---|---|
| `Config` | `include/config.hpp`, `src/config.cpp` | TOML parsing via toml++, validation, key file loading |
| `Pipeline` | `include/pipeline.hpp`, `src/pipeline.cpp` | GStreamer element graph, state machine, all GLib callbacks |
| `Encryptor` | `include/encryptor.hpp`, `src/encryptor.cpp` | Stateless `encrypt_file` / `decrypt_file` free functions (AES-256-GCM via OpenSSL EVP) |
| `Health` | `include/health.hpp`, `src/health.cpp` | JSON-lines logging to stderr, atomic status file writes |
| `DiskMonitor` | `include/disk_monitor.hpp`, `src/disk_monitor.cpp` | `statvfs`-based free-space check, no background thread |
| `utils.hpp` | header-only | `utc_timestamp_str()`, `segment_filename()`, `encrypted_filename()`, `staging_filename()` |

### GStreamer pipeline graph

**Raw formats (YUY2, etc.):**
```
v4l2src → capsfilter → videoconvert → videoscale → capsfilter → queue → x264enc → h264parse → splitmuxsink
```

**MJPG pixel format** (adds a JPEG decoder between capsfilter and videoconvert):
```
v4l2src → capsfilter → jpegdec → videoconvert → videoscale → capsfilter → queue → x264enc → h264parse → splitmuxsink
```

- `queue` is configured with `leaky=2` (downstream = drop-oldest) and `max-size-buffers` from config. This is the entire backpressure implementation.
- `h264parse config-interval=-1` re-emits SPS/PPS with every IDR, making each segment independently decodable.
- `splitmuxsink async-finalize=FALSE` — the muxer writes the moov atom synchronously before posting EOS. This ensures the file is fully written before the encryption worker opens it.
- The pipeline is built programmatically with `gst_element_factory_make` (not `gst_parse_launch`) so the muxer (`mp4mux` vs `matroskamux`) can be selected at runtime from config.

### Rotation and encryption flow

`splitmuxsink` fires the `"format-location-full"` GObject signal just before opening fragment N. At that moment, fragment N-1 is fully closed and flushed. The static callback `Pipeline::on_format_location_full` enqueues the just-closed file for encryption and returns the new filename. A dedicated `std::thread` (`encryption_worker`) drains a mutex + condvar queue of `{src, dst}` path pairs and calls `encryptor::encrypt_file`. The plaintext file is deleted after successful encryption if `encryption.delete_plaintext = true`.

When `output.temp_dir` is set, `splitmuxsink` writes to that directory (intended to be a `tmpfs` mount) instead of `output.directory`. The `src` of each encryption job is the staging path; `dst` is always in `output.directory`. The staging file is always deleted after encryption regardless of `delete_plaintext`. `staging_filename()` in `utils.hpp` returns the correct write path — it delegates to `segment_filename()` when `temp_dir` is empty.

The final segment (the one open when `stop()` is called) is never triggered by another `format-location-full` call, so `run_loop()` manually enqueues it after the GLib loop exits.

### State machine (`PipelineState`)

```
Starting → Recording ↔ PausedDiskFull
Recording → Reconnecting → Recording  (on camera error)
any → Stopping → Stopped
```

State transitions happen on the GLib main loop thread (bus watch callback and GLib timer callbacks). `state_` is an `std::atomic` so it can be read safely from `test_pipeline_disconnect`.

- **Camera disconnect**: `handle_error()` detects `GST_RESOURCE_ERROR` (recoverable) and calls `schedule_reconnect()`. Stream errors (`GST_STREAM_ERROR`, e.g. caps negotiation failures) are treated as fatal and quit the loop. On reconnect: the code waits for splitmuxsink to post EOS (finalizing the current segment) with a 3s fallback timeout, then tears down the pipeline and starts a `try_reconnect_cb` timer that polls `access(device, R_OK)` with exponential backoff (1s → 60s).
- **Disk full**: `disk_poll_cb` (GLib timer) calls `DiskMonitor::is_disk_full()`. On full: sends EOS to flush the current segment cleanly, then pauses pipeline after EOS arrives. Resumes automatically when space frees up.

### Encrypted file format (`.vcpenc`)

```
Bytes 0–3:   Magic "VCPE" (0x56435045)
Bytes 4–7:   version(1), reserved, header_len=30 (big-endian u16)
Bytes 8–19:  IV / nonce (12 bytes, random per file)
Bytes 20–27: plaintext length (big-endian u64)
Bytes 28–29: reserved
Bytes 30…N+30: ciphertext
Bytes N+30…N+46: GCM auth tag (16 bytes)
```

`decrypt_file` verifies the tag via `EVP_DecryptFinal_ex` and removes the partial output file before throwing `AuthenticationError` if it fails.

### Health status file

Written atomically (write to `<path>.tmp`, then `rename`). Fields: `state`, `uptime_sec`, `segments_written`, `bytes_written`, `last_segment`, `last_segment_time`, `disk_free_bytes`, `reconnect_attempts`, `enc_queue_depth`. Hand-formatted JSON via `snprintf` — no JSON library dependency.

## Key constraints

- `Pipeline` is not copyable. It owns the GStreamer element graph and the encryption thread.
- All GStreamer callbacks (bus watch, `format-location-full` signal, GLib timer callbacks) are static member functions receiving `gpointer user_data = this`.
- `bus_sync_handler` is declared in `pipeline.hpp` but the actual bus watch is installed as an inline lambda in `build_pipeline()`. The declared static is unused — do not remove `bus_sync_handler` without checking if it was added back.
- The encryption key is copied into `Pipeline::key_` (a `std::array<uint8_t, 32>`) at construction — the caller's buffer does not need to outlive the `Pipeline`.
- `test_pipeline_disconnect` links against `vcpcore` and directly instantiates `Pipeline` — it is both a functional test and an integration test for the public API.
