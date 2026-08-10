**English** | [Русский](README.md)

# LiveQX

**LiveQX** is a C++ engine designed for continuous media broadcasting: it plays back playlists containing photos and videos. Each channel behaves close to a full-fledged TV channel — the operator builds a playlist through the web interface, and the engine automatically processes the material: it reads files from local storage or a network share (CIFS/SMB), normalizes audio loudness, joins fragments with various transitions (cross-fade, dissolve, fade-to-black, push, wipe), and broadcasts an MPEG-TS stream over multiple protocols simultaneously — SRT, HLS, RTMP, RTSP, and multicast UDP — so a single channel feeds several outputs in parallel.

If a GPU is available, hardware encoding can be used — NVENC, QSV, or VAAPI. The base build includes: RBAC-based access control with per-channel permissions, optional LDAP integration, Prometheus metrics collection, and low-latency stream preview in the browser via WebRTC.

---

## LiveQX System Architecture

### Video Processing Core

The engine is built around a frame-based render loop: each channel has its own thread that generates frames at a fixed time step along the timeline.

A unified `IClip` interface is used to work with media content and supports three clip types:

- **ImageClip** — photos whose duration is set through the interface; the frame is placed into a cache;
- **VideoClip** — videos whose duration is determined by container data; playback always completes cleanly, to the end of the file;
- **FallbackClip** — a placeholder that activates when the playlist is empty or the source is unavailable.

Transitions between fragments and image composition are implemented through pluggable interfaces `ITransition` and `ICompositor`. Cross-fade is used by default.

Encoder selection is done via a factory: the baseline is x264 (CPU-based) — always available. Hardware encoding can be enabled on demand: NVIDIA NVENC, Intel Quick Sync (QSV), or VAAPI (Linux, AMD and Intel GPUs). These modules are enabled at build time via CMake flags `ENABLE_NVENC`, `ENABLE_QSV`, and `ENABLE_VAAPI`.

Inter-thread data transfer is organized using lock-free SPSC queues between read, decode, encode, and output stages. A frame pool (`FramePool`) is used to avoid dynamic memory allocation in hot code paths.

The default output profile is 720p at 25 frames per second, H.264 and AAC codecs, MPEG-TS container. Encoding parameters can be tuned flexibly on a per-channel basis.

### Channel and Configuration Management

Each channel has its own directory with a clear structure: `channels/ch{id}-{name}/`, containing `config.json` and folders for logs (`logs/`), cache (`cache/`), and playback (`playback/`). The `config.json` file is the single source of truth for channel settings. Configuration updates are atomic: changes are first written to a temporary file, then renamed and forcibly synced to disk (`fsync`).

At startup, `ChannelManager` scans all `channels/*/config.json` folders. An error in one channel's config does not affect the others.

The global `config.json` contains only process-level settings: API and TLS parameters, log level, root channels directory, watchdog settings.

### Path Resolution

The `Paths` class builds paths (`state_dir`, `log_dir`, `channels_dir`, etc.) according to source priority: command-line flags > environment variables > config file > defaults.

By default, the system follows the FHS standard: if `/var/lib/liveqx` exists, it will be used. Otherwise, paths relative to the current working directory are applied — this is convenient for development and testing.

### Media Content Sources

Content can be located in a local folder or on a network share (CIFS/SMB).

A background mechanism (`ShareScanner` + `ContentSync`) tracks changes in directories: it indexes contents, reacts to file additions or deletions, and automatically updates the playlist.

Dynamic mounting of network shares without restarting the engine is supported. This is handled by a dedicated systemd unit `liveqx-mountd`, running as root and executing `mount.cifs`. The main process is notified of changes via mount-namespace event propagation.

### Outputs and Broadcasting

The system allows a single channel to be delivered simultaneously over several protocols: SRT, HLS, RTMP, RTSP, and multicast UDP. A single encoder is used, and packet branching is done at the packetizer level. `OutputManager` is responsible for multiplexing and distributing the stream across the set of transport protocols.

For UI preview there is a WebRTC stream (based on libdatachannel) with low latency: 480p resolution, signal taken from a tap interface on the render thread. This makes it possible to preview a channel in the UI without going on-air.

### API and User Interface

The system is controlled through an HTTP/HTTPS Control API based on cpp-httplib. A CA certificate and a self-signed certificate are generated automatically on first launch. Several operating modes are supported: `auto`, `provided`, `behind_proxy`, `disabled`.

The web interface is implemented in React and is loaded via the `--ui-dir` parameter. An SPA fallback is provided for correct react-router behavior. A bilingual knowledge base (Russian and English) is included inside the UI.

An SSE event stream is used for real-time status updates.

Metrics are collected in a Prometheus-compatible format at the `/api/metrics` endpoint. Access is protected by a bearer token generated at first launch and stored in `state_dir`.

### Security

Authentication of local users is based on bcrypt; sessions are transported via JWT.

The RBAC model provides three roles: `admin`, `operator`, `viewer`, with the ability to assign permissions on a per-channel basis. The `/api/status` endpoint automatically filters data according to the granted permissions.

LDAP integration is optionally supported.

The master key (based on libsodium) is stored either in the `state/master.key` file (with `0600` permissions) or in the `LIVEQX_MASTER_KEY` environment variable (base64-encoded). Key rotation is performed via CLI with exclusive locking, backup creation, and clearing of credentials for LDAP, SMTP, and network shares.

### Time Handling

The system includes a built-in SNTP client and honors the server's time zone. Each channel can have its own time zone — by default it is inherited from the system setting. Frame timestamps (render timestamps) are monotonic, and playlist playback scheduling is bound to the channel's local time.

### Data Storage and Durability

SQLite is used for data storage:

- `auth.db` — user accounts, roles, and tokens;
- `mounts.db` — credentials for CIFS shares, encrypted with the master key;
- `playback.db` (per-channel) — playback data split by ISO weeks (e.g., `2026-W22.db`), with configurable retention and statuses (`completed`, `skipped_user`, `removed`, `error`).

Database schema migrations are performed via `PRAGMA user_version`. If the database version is higher than the executable version, the system refuses to start — this prevents silent data corruption.

### Audio Processing

Audio decoding is performed via FFmpeg; loudness normalization follows the EBU R128 standard, targeting −23 LUFS. Audio is encoded to AAC (stereo, 48 kHz, 128 kbps). Silence frames are generated for `ImageClip`.

At clip boundaries an audio crossfade is applied: fade-out of the previous fragment and fade-in of the next. Audio and video are synchronized by PTS in the muxer.

### Safeguards and Fault Tolerance

To avoid a black screen on decoder failures, the system keeps the last successful frame (`last_good_frame`) and replays it during performance dips.

The `getFrame()` method is non-blocking with a timeout: on delay it falls back to `last_good_frame`, so the render loop is not stalled by I/O operations.

The `Preloader` mechanism prepares the next clip N seconds before the transition; on powerful hardware N can be increased.

`FallbackClip` ensures stream continuity: if content is missing or the folder is unavailable, a static placeholder with silence is played. As a result, the broadcast is never interrupted.

Integration with systemd is done via watchdog and `sd_notify`: if the render loop hangs, the watchdog fires and the system restarts.

---

## Control API

HTTP/JSON REST on top of cpp-httplib. The full specification is in `openapi.yaml` at the repository root. Main endpoint groups:

| Group        | Prefix                                      | Purpose                                        |
|--------------|---------------------------------------------|------------------------------------------------|
| Channels     | `/api/channels`                             | CRUD channels, status, config                  |
| Playlist     | `/api/channels/{id}/playlist`               | Load, append, delete, notify-deleted           |
| Transport    | `/api/channels/{id}/{play,stop,next}`       | Start, stop, forced skip                       |
| Monitoring   | `/api/health`, `/api/metrics`               | Health check, Prometheus metrics               |
| System       | `/api/system/{interfaces,time,…}`           | NIC, NTP, server timezone                      |
| Auth/RBAC    | `/api/auth`, `/api/users`, `/api/sessions`  | Login, JWT, users, grants                      |
| Mounts       | `/api/mounts`                               | CIFS shares: add, update, status               |
| TLS          | `/api/tls/*`                                | Certificates, CA, regeneration                 |
| Preview      | `/api/preview/*`                            | WebRTC offer/answer                            |
| Stress       | `/api/stress/*`                             | Load scenarios and reports                     |

The OpenAPI contract in `openapi.yaml` is the single source of truth for request and response schemas. TypeScript types for the frontend are generated from it — run `npm run gen:api-types`. In addition, contract tests in the `tests/` directory use this file as a reference: they verify that the engine's actual responses conform to the schemas described in the contract.

### Channel Config Example

`channels/ch1-news/config.json`:

```json
{
  "id": 1,
  "name": "News",
  "resolution": "1280x720",
  "fps": 25,
  "bitrate": 4000000,
  "preset": "medium",
  "scale_mode": "fit",
  "default_photo_duration": 10.0,
  "default_transition": { "type": "crossfade", "duration": 2.0 },
  "fallback": { "image_path": "/var/lib/liveqx/fallback.png" },
  "outputs": [
    { "type": "srt",  "port": 9000, "latency_ms": 200 },
    { "type": "rtmp", "url": "rtmp://server/live/ch1" }
  ],
  "audio": {
    "codec": "aac", "bitrate": 128000, "sample_rate": 48000,
    "normalize": true, "target_lufs": -23
  }
}
```

Channel configuration is updated via a `PATCH` (or `PUT`) request to `/api/channels/{id}/config`. The operation is implemented as atomic rewrite: data is first written to a temporary file, then renamed and synced to disk (via `fsync`). Restarting the channel is not required — changes are applied on the fly.

---

## Installing LiveQX

The system is compatible with Ubuntu 22.04 and newer (including 24.04), as well as Debian 12 and newer. During installation the scripts automatically:

- install required dependencies via `apt`;
- create the `liveqx` system user;
- create the required directories: `/var/lib/liveqx`, `/etc/liveqx`, `/usr/share/liveqx/ui`, `/mnt/liveqx`;
- install three systemd units: `liveqx-mount-init`, `liveqx-mountd`, `liveqx`;
- start the main service.

### Option 1: install from scratch (build and deploy with a single script)

```bash
git clone https://github.com/MrGospo/LiveQX.git
cd LiveQX
sudo ./packaging/bootstrap.sh
```

The script itself installs the build tools (CMake, `build-essential`, Node.js 20 or newer, required dev packages), builds the backend and the web interface, and then hands off control to `install.sh`. To skip confirmation prompts, add the `-y` flag:

```bash
sudo ./packaging/bootstrap.sh -y
```

### Option 2: deploy a pre-built solution

#### Build

Run the following commands to compile the project with the required options:

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release \
    -DENABLE_NVENC=ON -DENABLE_QSV=ON -DENABLE_VAAPI=ON \
    -DENABLE_WEBRTC_PREVIEW=ON -DENABLE_SYSTEMD=ON
cmake --build build -j$(nproc) --target liveqx liveqx-mountd
```

Then build the user interface:

```bash
(cd ui && npm ci && npm run build)
```

#### Install

Run the install script:

```bash
sudo ./packaging/install.sh
```

During installation the system will ask which hardware encoders (NVENC, VAAPI, QSV) you plan to use. This step only records your choice in the configuration — the drivers and SDKs themselves (for example, NVIDIA components, `intel-media-va-driver`, `libmfx`) must be installed manually. After they are deployed, restart the service:

```bash
sudo systemctl restart liveqx
```

### Verification and Uninstall

To verify the installation:

```bash
sudo ./packaging/install.sh check
```

This command displays the status of the systemd units, checks the presence of key files, and performs a request to the `/api/health` endpoint.

To fully remove the system:

```bash
sudo ./packaging/install.sh uninstall
```

This will remove binaries, databases, config files, and the system user. Note: packages installed via `apt` are not removed. Clean them up manually if needed:

```bash
sudo apt-get autoremove --purge
```

### Running from Source in Development Mode

For debugging and testing, build the project in `Debug` mode:

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build -j$(nproc)
```

Run the tests:

```bash
ctest --test-dir build --output-on-failure
```

And launch the engine itself with the default configuration:

```bash
./build/liveqx --config config/default.json
```

---

## License

The project is distributed under the [Apache License 2.0](LICENSE).
