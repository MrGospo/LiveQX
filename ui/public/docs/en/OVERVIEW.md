# What is LiveQX

LiveQX is a broadcast server. It takes your content (photos, videos, live sources), builds a continuous stream from it, encodes it once and delivers it to one or more destinations: SRT, multicast, RTMP, HLS, NDI.

Unlike an ordinary video player, LiveQX runs unattended around the clock and carries many independent broadcasts at the same time.

## Core concepts

### Channel

One broadcast. A channel has a name, encoding parameters (resolution, FPS, bitrate, codecs), a playlist and a list of outputs. Channels are independent: starting, stopping or changing one does not affect the others.

A channel's name, resolution and FPS are set at creation and cannot be changed later.

### Playlist

An ordered list of entries that the channel plays in a loop. An entry is:

- a **photo** — shown for a set number of seconds (10 by default);
- a **video file** — played in full;
- a **live source** — an external stream (RTMP, RTSP, multicast, NDI) inserted into the broadcast for a set time.

If the playlist is empty or a file is unavailable, the channel shows a **fallback** — a static image, if one is configured.

More: [Channel playlist](PLAYLIST_USER.md).

### Content source

Where the playlist comes from:

- **Manual** — the operator adds entries.
- **From a folder** — the channel watches a folder on the server or on a network share and keeps the playlist in line with its contents ("Cache" and "Passthrough" modes).

More: [Content source](CONTENT_SOURCE_USER.md).

### Live source

A playlist entry that takes a ready-made stream from outside — a camera, OBS, another server — and shows it on air at a set time.

More: [Live sources](LIVE_INPUT_USER.md).

### Output

The way a channel delivers the finished stream. A channel can have several outputs of different types. The channel encodes video once, and outputs only deliver the result.

More: [Outputs overview](OUTPUTS_OVERVIEW.md).

### Schedule

Rules by which a channel **switches to another playlist** at set hours: daily, on certain weekdays, on certain days of the month, or once. A schedule does not start or stop a channel.

More: [Schedule](SCHEDULE_USER.md).

### Transition

An effect between two clips: crossfade, wipe, push, dissolve and others.

More: [Transitions](TRANSITIONS_USER.md).

## How it works

```
[Playlist: files, photos, live sources]
                  ↓
           [Channel: encoding]
                  ↓
      ┌───────────┼───────────┐
      ↓           ↓           ↓
   [SRT]     [Multicast]   [RTMP / HLS / NDI]
```

## Roles

What you can do depends on your role:

- **Viewer** — can watch channels the administrator has granted access to.
- **Operator** — can manage channels: start and stop, playlist, schedule, outputs, configuration. Only channels on which the administrator has granted the "Operate" permission can be managed.
- **Administrator** — full access to all channels plus creating and deleting channels, users, network shares, time, TLS, Audit Trail and other system settings.

Access to a particular channel for a viewer or operator is granted by the administrator: on the channel's **Permissions** tab or in the user's card. There are two permission levels: **View** and **Operate**.

More: [Users](USERS_USER.md).

## What else is in the system

- **Observability** — metrics (including for Prometheus), the event stream, service health checks.
- **Gateways** — a separate feature for forwarding UDP/multicast streams (administrator).
- **Plugins** — installing additional modules (administrator).
- **Audit Trail** — who changed what in the system (administrator).
