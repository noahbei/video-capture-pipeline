# vcpcapture — Design Document

## Assumptions

- **Single camera, single process.** One `vcpcapture` instance owns one V4L2 device. Multi-camera deployments run multiple instances with separate configs.
- **Embedded / edge deployment.** The target is a Linux SBC or server with a USB camera. CPU budget is limited, so encoding defaults to `ultrafast` preset with `zerolatency` tuning.
- **Untrusted storage.** The recording volume is assumed to be physically accessible to an adversary. Encryption is on by default, and plaintext segments are deleted immediately after encryption.
- **Key lives elsewhere.** The AES key is expected on a separate secure partition or HSM. Storing it alongside recordings is explicitly warned against.
- **No frame loss is acceptable only up to a budget.** The design prioritises continuous recording over zero frame loss. When the encoder outpaces storage, the oldest buffered frames are dropped rather than blocking and stalling the entire pipeline.
- **Encrypt-after-write is the default, with an opt-in staging mode for stricter deployments.** By default segments are written to disk as plaintext first, then encrypted after each rotation completes. When `output.temp_dir` is set (intended to point at a `tmpfs` mount), segments are written to RAM instead; the encrypted `.vcpenc` is written directly to `output.directory` and the staging file is always deleted afterward. Plaintext never reaches the persistent recording volume in this mode.
- **The output directory stands in for a dedicated mount.** In a real deployment the recording directory would typically be a separate, dedicated filesystem mounted at a fixed path. For simplicity, a plain directory is used instead.
---

## Capture and Storage Strategy

The GStreamer pipeline is:

**Raw formats (YUY2, etc.):**
```
v4l2src → capsfilter → videoconvert → videoscale → capsfilter → queue → x264enc → h264parse → splitmuxsink
```

**MJPG pixel format:**
```
v4l2src → capsfilter → jpegdec → videoconvert → videoscale → capsfilter → queue → x264enc → h264parse → splitmuxsink
```

`splitmuxsink` rotates segments either by wall-clock duration or by file size, controlled by `[rotation]` config. Rotation is triggered by the `format-location-full` signal: just before segment N opens, segment N-1 is fully flushed and closed. The signal callback enqueues the closed path for encryption and returns the new filename for segment N.

Each segment is independently decodable. `h264parse config-interval=-1` re-emits SPS/PPS headers with every IDR frame, so any segment can be opened cold without prior context.

The muxer (`mp4mux` or `matroskamux`) is selected at runtime from the `[output] container` field. The pipeline is built programmatically with `gst_element_factory_make` rather than `gst_parse_launch` to allow this runtime selection.

---

## Encryption Design

**Algorithm:** AES-256-GCM (authenticated encryption). Implemented via OpenSSL EVP.

**Key:** 256-bit raw binary, loaded from a file at startup and copied into the `Pipeline` object. The caller's buffer does not need to outlive the pipeline.

**Per-file IV:** A fresh 12-byte nonce is generated with `RAND_bytes` for every segment. Reuse of IV + key would be catastrophic for GCM, so the nonce is never derived from a counter.

**File format (`.vcpenc`):**

| Offset | Size | Field |
|--------|------|-------|
| 0 | 4 B | Magic `VCPE` |
| 4 | 4 B | version (1), reserved, header\_len (30, big-endian u16) |
| 8 | 12 B | IV / nonce |
| 20 | 8 B | Plaintext length (big-endian u64) |
| 28 | 2 B | Reserved |
| 30 | N B | Ciphertext |
| 30+N | 16 B | GCM auth tag |

Decryption verifies the auth tag via `EVP_DecryptFinal_ex`. If verification fails, the partial output file is deleted before the error is raised (`AuthenticationError`), preventing truncated or tampered data from being silently accepted.

Encryption runs on a dedicated `std::thread` (`encryption_worker`) that drains a mutex-protected queue of `{src, dst}` path pairs. This decouples encryption latency from the GStreamer pipeline and keeps the recording thread non-blocking. The final open segment (not closed by another `format-location-full` event) is manually enqueued by `run_loop()` after the GLib main loop exits.

When `output.temp_dir` is configured, `src` is the staging path (inside the tmpfs) and `dst` is the `.vcpenc` path inside `output.directory`. The staging file is always removed after encryption regardless of `delete_plaintext`. When `temp_dir` is not set, `dst` is `src + ".vcpenc"` in the same directory and `delete_plaintext` governs whether `src` is removed.

---

## Failure Handling

### Camera Disconnect

The GStreamer bus watch (`handle_error`) detects errors. Only `GST_RESOURCE_ERROR` is treated as recoverable (device gone, read failure). `GST_STREAM_ERROR` (caps negotiation, unsupported format) is fatal — retrying will not fix a configuration mistake.

On a recoverable error:

1. `schedule_reconnect()` waits for splitmuxsink to post EOS, which finalizes and flushes the current segment. A 3 s fallback timer handles the case where EOS never arrives.
2. Once the segment is finalized (or the timeout fires), the pipeline is torn down and the partial segment is enqueued for encryption.
3. A one-shot GLib timer fires `try_reconnect_cb`, which polls `access(device, R_OK)`. On failure the backoff doubles (capped at 60 s) via `schedule_retry()`.
4. On success the pipeline is rebuilt and recording resumes from the next segment index.

The state machine reflects this as `Recording → Reconnecting → Recording`.

### Disk Full

A GLib timer fires `disk_poll_cb` every `poll_interval_sec` seconds. `DiskMonitor::is_disk_full()` calls `statvfs` — no background thread; the check is lightweight and synchronous.

On full:
1. EOS is sent into the pipeline to flush and close the current segment cleanly.
2. After EOS arrives on the bus, the pipeline is paused (`PausedDiskFull`).
3. The same poll timer continues; when free space rises above `min_free_bytes`, the pipeline resumes and recording continues.

This prevents partial / unclosed segments and avoids write errors mid-segment.

### Buffer Backpressure

The `queue` element between the encoder and the muxer is configured with `leaky=2` (drop-oldest) and `max-size-buffers` from `[backpressure] queue_max_buffers`. When the muxer or encryption thread falls behind, the queue absorbs the burst up to its limit, then silently drops the oldest encoded frames. The pipeline never stalls or blocks the capture source.

---

## Trade-offs in Buffering and Latency

| Decision | Benefit | Cost |
|----------|---------|------|
| `leaky=2` queue (drop-oldest) | Pipeline never stalls; capture continues through storage hiccups | Oldest frames in the buffer are lost under sustained overload |
| `zerolatency` encoder tune | Minimises encode latency; frames enter the muxer quickly | Slightly lower compression efficiency vs. default tuning |
| Async encryption thread | Encryption does not delay segment rotation | Peak memory usage includes one full plaintext segment in-flight |
| `splitmuxsink async-finalize=FALSE` | Moov atom is written synchronously before EOS is posted, so encryption never opens an incomplete file | Segment rotation blocks until the muxer finishes writing the trailer |
| Per-segment IV (random) | No IV reuse even if the process crashes and restarts | 12 bytes of overhead per file (negligible) |
| `delete_plaintext = true` by default | No plaintext copy remains on disk after encryption | If the process is killed mid-encryption, both files must be inspected on recovery |
| tmpfs staging (`output.temp_dir`) | Plaintext never reaches persistent storage; staging vanishes on reboot if process crashes | Consumes RAM proportional to one open segment plus one being encrypted; requires a tmpfs mount sized accordingly |

The dominant latency source in practice is the muxer flush at segment boundaries, not the encoder or encryption. Shorter `max_duration_sec` reduces the worst-case latency to access the latest recording but increases rotation overhead and the number of files to manage.

---

## Stretch Goals — Not Implemented

**systemd integration** — write a `.service` file that tells systemd to start `vcpcapture` on boot and restart it if it crashes. The main risk is that systemd and the built-in reconnect logic both try to recover from failures independently, so their retry timers need to be tuned to not conflict.

**Network streaming** — split the video stream so one copy goes to disk like we are doing now and another is sent over the network. The tricky part is making sure a slow or disconnected viewer can't slow down the recording, the two paths need to be independent. The live stream would also be unencrypted, unlike the stored segments.

**Yocto packaging** — write a build recipe so the app can be compiled and included in a custom Linux image. Most dependencies are already available in standard Yocto layers. The main concern is keeping the encryption key out of the base image, it should live on a separate, read-only partition.

**ML object detection** — run a detection model on each frame in a background thread and log results to the health output. It can't be allowed to slow down the recording if it falls behind.

**Metrics export** — most of the useful numbers (segments written, disk space, queue depth, reconnect count) are already tracked internally. The work is just exposing them over HTTP or another easy way for people to access them.

---

## AI Tooling

Claude Code was used throughout this project for planning the overall architecture, generating code across modules, and debugging. The `CLAUDE.md` file in the repository captures the project context that was maintained across sessions.