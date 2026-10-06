# Outputs overview

An output is the way a channel delivers the finished stream outward. Without outputs a channel works, but nobody can watch it.

One channel can have several outputs of different types. The channel **encodes video once**, and each output only delivers the finished stream in its own way. So adding an output hardly loads the server — it is not the same as adding a new channel.

## Output types

### SRT

LiveQX opens a port and **waits for a viewer to connect** ("listener" mode). A receiver (VLC, ffplay, another server, a decoder) connects to `srt://server-address:port`.

**When to choose:** reliable delivery over a lossy network, contribution to another server, checking the stream in VLC.

Note: an SRT output serves one connected viewer at a time.

### Multicast

MPEG-TS over UDP to a multicast group. Many receivers pick up one stream without extra load on the server.

**When to choose:** corporate network, hotel IPTV, internal displays, strict receivers and middleware (Service ID, TSID and other MPEG-TS parameters are set on the "Config" tab).

**Downsides:** works only in networks where multicast is enabled; does not travel over the internet. Multicast usually requires constant bitrate (CBR).

### RTMP (push to an external platform)

LiveQX itself connects to an external server and sends the stream there: YouTube, VK Video, Twitch, Restream or your own RTMP server. LiveQX cannot receive RTMP streams — it only sends.

The address looks like `rtmp://…` or `rtmps://…` (encrypted). If the connection drops, LiveQX reconnects by itself, increasing the pause between attempts from 1 second to 1 minute.

### HLS

LiveQX slices the stream into short `.ts` segments and writes them, together with the `stream.m3u8` playlist, into a **directory on the server**. These files are served to viewers by a separate web server (for example nginx) or a CDN — LiveQX has no HTTP server of its own for HLS.

**When to choose:** playback in browsers and mobile apps.

**Downsides:** a delay of tens of seconds — that is how the format works. The default segment is 6 seconds, with 5 segments in the playlist.

### NDI

Studio-quality delivery over a local network to mixers and monitors (vMix, OBS, NDI Studio Monitor). It works only if the NDI library is installed on the server. The server supports the NDI output, but the "Add output" form in the web interface does not have it yet: it is configured through the REST API or the channel configuration file. More: [Adding an output](ADD_OUTPUT.md).

## What LiveQX does not have

There is no RTSP output: LiveQX cannot serve an RTSP stream outward. RTSP is supported only as a **source** (see [Live sources](LIVE_INPUT_USER.md)).

## What to choose

| What the stream is for | Type |
|---|---|
| Check the picture, hand it to a single receiver | SRT |
| Screens in an office, hotel, shopping mall | Multicast |
| Website, mobile app | HLS (+ web server) |
| YouTube, VK, Twitch and other platforms | RTMP |
| Studio mixer | NDI |

## Output state

On the **Outputs** tab, each output shows: whether it is connected, the bitrate, how many bytes and packets were sent, queue usage, lost packets and frames, the number of reconnects, and RTT (for SRT).

If an output's queue overflows (the receiver cannot keep up), packets are dropped and the loss counters grow — a sign of a problem on the network or receiver side. The queue size is set in the output form (4 MB by default).

An output can be temporarily turned off with the **Enabled** switch without deleting it.

## Next

- How to add an output — [Adding an output](ADD_OUTPUT.md).
- If you need an external stream instead of files — [Live sources](LIVE_INPUT_USER.md).
- If an output does not work — [Troubleshooting](TROUBLESHOOTING.md).
