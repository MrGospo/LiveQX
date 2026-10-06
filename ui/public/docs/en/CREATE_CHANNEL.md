# Creating a channel

A channel is one broadcast: a playlist turned into a continuous video stream and sent to outputs. This article explains how to create a channel and what the form fields mean.

**Who can create channels:** administrators only. The **Create channel** button is not shown to other roles.

## Where

Left menu → **Channels** → **Create channel**. The form has two steps.

## Step 1. Channel basics

### Name

Latin letters, digits, `_` and `-`, up to 64 characters. The name is unique. **It cannot be changed after creation.**

### Resolution and frame rate

- **Resolution** — in `width×height` format, for example `1920x1080` (the default).
- **Target FPS** — from 1 to 120, 25 by default.

Resolution and FPS **cannot be changed** after the channel is created. If you need different values, create a new channel.

### Video

- **Video codec** — H.264 (default) or MPEG-2 Video. MPEG-2 is only needed for old set-top boxes that cannot decode H.264; it is always encoded on the CPU.
- **Encoder** — `auto` (default), `cpu`, `nvenc`, `qsv`, `vaapi`. Hardware options are available if the server has a suitable graphics card and a build with support for it. In `auto` mode the server chooses.
- **Bitrate** — in kbps, from 100 to 100,000, 4000 by default.
- **Rate control:**
  - **CBR** — constant bitrate. Required for MPEG-TS multicast (set-top boxes and hotel decoders rely on a fixed stream).
  - **VBR** — variable with a ceiling; suitable for HLS and RTMP. You can set a maximum bitrate (0 means chosen automatically).
  - **CRF** — quality-based, software x264 encoding only. On hardware encoders and MPEG-2 it silently falls back to VBR (a warning is written to the log).
- **Preset** (H.264 only) — from `ultrafast` to `veryslow`, `veryfast` by default. Faster means less CPU load, slower means better compression.
- **Max B-frames**, **GOP size**, **H.264 / MPEG-2 profile and level** — fine-tuning for strict receivers. The defaults suit most cases; each field shows a detailed hint.

### Audio

- **Audio codec** — AAC-LC (default) or MP2 (the classic DVB codec for set-top boxes).
- **Audio bitrate** — 32–512 kbps, 128 by default.
- **Sample rate** — 44100 or 48000 Hz (default).

### NUMA node

The number of the server's NUMA node (0–7) the channel runs on. On single-processor servers leave 0.

### Photo duration

How many seconds to show a photo that has no duration of its own. 10 seconds by default.

### Content source

- **off** — the channel does not read files from a folder; the playlist is filled in manually, or the channel plays live sources only.
- **Passthrough** — files are read directly from the specified folder.
- **Cache** — a background service copies files from the folder (for example, a network share) into a local cache, and the channel plays from the cache.

More: [Content source](CONTENT_SOURCE_USER.md).

### Fallback image

A static image shown when there is nothing to play in the playlist. The path is on the server.

### Playback log

What was played and when:

- **None** — nothing is recorded.
- **File** — daily JSONL files in the channel directory.
- **SQLite** — a shared database for all channels with fast search; records older than the set number of days (90 by default) are removed automatically.

## Step 2. Output destinations

The **first output** is configured during creation:

- **Output type** — SRT, Multicast, RTMP or HLS.
- **Output ID** — any identifier (`main` by default).
- Fields depend on the type: address and port, URL, or directory. For SRT and Multicast you can choose the sending network interface; for Multicast, the TTL.
- For Multicast, an **MPEG-TS / IPTV** block opens: service name, provider, Service ID, TSID, ONID, mux rate and SDT/PAT periods. If several channels broadcast into one multicast subnet, they **must have different Service IDs**, otherwise the receiver merges them into one program.

Additional outputs are added later on the **Outputs** tab. More: [Adding an output](ADD_OUTPUT.md).

## After creation

The channel appears in the list stopped and its page opens. Next:

1. Add content — [Channel playlist](PLAYLIST_USER.md).
2. Click **Start**.

Tabs on the channel page: **Overview**, **Playlist**, **Schedule**, **Outputs**, **Log**, **Config**, **Watcher**, **Permissions** (the last one is for administrators).

## Changing settings later

The **Config** tab lets you change most parameters, including encoding, the default transition, the fallback image, the content source, the playback log and the channel time zone. Some changes take effect only after the channel is restarted — the page warns you about it. The name, resolution and FPS cannot be changed.

## Deleting a channel

The "⋯" menu on the channel page → **Delete channel** (administrators only). To confirm you must type the channel name.

Deletion stops all outputs and erases the channel's configuration and cache. Logs remain on disk. Source files on the server and on shares are **not touched**.
