# Adding an output

How to add an output to a channel. The fields depend on the type; the general sequence is the same.

> First read [Outputs overview](OUTPUTS_OVERVIEW.md) to choose the right type.

## Who can

Operators (on channels with the "Operate" permission) and administrators can add, change, restart and delete outputs.

## General sequence

1. Channel page → **Outputs** tab.
2. **Add output**.
3. Choose the **type**: SRT, MULTICAST, RTMP or HLS.
4. Set the **output ID** — Latin letters, digits, `_` and `-`, up to 64 characters. It is unique within the channel and does not change.
5. Fill in the type's fields (see below).
6. If needed, change the **queue limit (MB)** — 4 by default.
7. Click **Add**.

If the channel is already running, the output starts at once. If it is stopped, the output starts together with the channel.

The output type cannot be changed after creation — you need to delete the output and create a new one.

## Common fields

- **Enabled** — a disabled output stays in the configuration but does not deliver the stream.
- **Source network interface** (SRT, Multicast, RTMP) — which network card to send traffic through. "Default" — the system picks the route. Useful on servers with several network cards.

## SRT

- **Address** — `0.0.0.0` is filled in.
- **Port** — the port on which LiveQX waits for a connection (4000 by default in the form).

Check: in VLC "Open network stream" → `srt://server-address:port`.

An SRT output serves one viewer at a time.

## Multicast

- **Address** — a multicast group from the range 224.0.0.0–239.255.255.255. In private networks `239.x.x.x` is usually used.
- **Port** — a UDP port from 1 to 65535.
- **TTL** — how many routers the stream may cross (1–255, 16 by default): `1` — local subnet only, `16` — corporate network, `255` — no limit.

MPEG-TS parameters (Service ID, TSID and others) are set on the channel's **Config** tab in the **MPEG-TS / IPTV** block. If several channels broadcast into one subnet, each needs its own **Service ID**.

For multicast use constant bitrate (CBR) in the channel's encoding.

Check: VLC → `udp://@239.x.x.x:port`.

## RTMP

- **URL** — the full receiving address together with the stream key, for example `rtmp://a.rtmp.youtube.com/live2/xxxx-xxxx-xxxx` or `rtmps://…`. It must start with `rtmp://` or `rtmps://`.

There is no separate field for the key: the key is part of the URL. **Do not show this URL to outsiders** — it allows broadcasting to your channel.

The URL and key are in the platform's dashboard (YouTube Studio → Stream; VK Video → stream settings; Twitch → creator dashboard).

Check: start the channel and after 10–30 seconds open the broadcast page on the platform.

## HLS

- **Directory** — an absolute path to a directory on the server where files are written. The directory must exist, and the LiveQX service must have write permission (you can pick it with the browse button, available to administrators).

LiveQX writes the `stream.m3u8` playlist and segments `seg_00000.ts` and so on into the directory (6-second segments, 5 segments in the playlist, old ones are deleted). **Serving** these files to viewers is up to your web server (nginx, etc.) or a CDN — LiveQX itself does not serve HLS over HTTP.

Check: make sure new files keep appearing in the directory, and open the `stream.m3u8` address on your web server in Safari or VLC.

## NDI

The web interface form has no NDI type. The server supports the NDI output: it can be added through the REST API (`POST /api/channels/{id}/outputs`) with `type: "ndi"` and `ndi_name` (the source name for NDI receivers), or specified in the channel configuration file. The NDI library must be installed on the server.

## Changing and deleting

In the output's row on the **Outputs** tab:

- **Edit** — opens the same form. On saving, the output is recreated:
  - **Multicast** is replaced **without a gap in the stream**: the new output starts before the old one stops. If the new one fails to start, the previous one keeps running as before.
  - **SRT, RTMP, HLS, NDI** hold a port, directory, stream key or name that cannot be used twice, so the stream on that output is interrupted for a few seconds. If the new parameters do not work, the server **restores the previous output** and shows the reason for the error.

  In both cases the settings are not lost. If the previous output cannot be restored (for example, another process has taken its port), the output stays in the configuration and is shown as not working ("Output is down…"), and the event is recorded in the Audit Trail and in the events. Bring it back with the **Restart** button or by restarting the channel.
- **Restart** — rebuilds the output from its stored configuration. Use it to bring up an output that is not working once the cause is fixed. Multicast without a gap, other types with an interruption of a few seconds.
- **Delete** — the output is stopped and removed; the action is irreversible.

## Checking that it works

After the channel starts, on the **Outputs** tab the output should show "connected", and the counters of bytes sent should grow. If not, see [Troubleshooting](TROUBLESHOOTING.md).
