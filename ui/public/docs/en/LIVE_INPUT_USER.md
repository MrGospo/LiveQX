# Live sources

A live source is a playlist entry that takes a **ready-made external stream** (a camera, OBS, another server) and shows it on air for a set time. The other playlist entries — files and photos — keep playing as usual.

Suitable, for example, for:

- inserting a live feed from a camera among clips;
- relaying a stream from another server at a set time;
- a broadcast from OBS or studio equipment.

## How it differs from a file

| | File or photo | Live source |
|---|---|---|
| What plays | Pre-prepared content | An outside stream in real time |
| How long on air | The file's or photo's duration | Set in the entry: **duration** |
| If the source disappears | — | The channel shows a fallback frame and waits for it to come back |

This is **not a mode of the whole channel** but an ordinary playlist entry: it can be placed between files and moved around like them.

## Adding

Channel page → **Playlist** tab → **Add live source**. Fill in:

- **Type** — RTMP, RTSP, Multicast or NDI.
- **ID** — a name for the entry, unique in the playlist (required).
- **URL** — the stream address (for NDI, the **NDI source name** is given instead of a URL).
- **Duration** — how many seconds the entry stays on air.

After adding, the entry can be changed with the **Edit item** button: **warm-up** and the **signal-loss threshold** are set there too.

Operators (on a channel with the "Operate" permission) and administrators can add entries. If the channel uses a content source from a folder, the playlist is maintained automatically and manual entries cannot be added — see [Content source](CONTENT_SOURCE_USER.md).

## Supported types

- **RTMP** — receiving a stream from an RTMP server at `rtmp://server/app/key`. LiveQX connects to the source itself (client mode). The stream key is hidden in diagnostics.
- **RTSP** — IP cameras and RTSP servers: `rtsp://login:password@192.168.1.10:554/stream1`. TCP is used by default.
- **Multicast** — receiving a stream from a group: `udp://239.1.2.3:1234`.
- **NDI** — a source by name on the local network; the NDI library is required on the server.

## Warm-up, signal loss and duration

- **Warm-up** (5 seconds by default) — this many seconds before the entry starts, LiveQX opens the source in advance so a slow connection or joining a multicast group does not delay the broadcast.
- **Signal-loss threshold** (2 seconds by default) — if there is no data from the source for longer than this, the entry is considered lost.
- **Duration** — the entry ends exactly when the set time elapses, even if the source is still streaming. This keeps the broadcast precise.

## What happens if the source disappears

While the source is unavailable (before the entry starts, during warm-up, or after signal loss), a **fallback frame** is shown instead. By default it is the channel's fallback image (**Config → Fallback image**); if no fallback image is set, a black frame is shown. The source keeps reconnecting by itself, and when the stream returns, the entry shows it again automatically.

## Address security

If the URL contains a login and password (`rtsp://user:pass@…`), they are visible to everyone who can edit the channel's playlist. So it is better to create a separate account on the camera with minimal rights and to restrict access by IP or through a VPN.

## An alternative

If you need to forward a UDP/multicast stream "as is" without inserting it into a playlist, use the **Gateways** section (administrator).
