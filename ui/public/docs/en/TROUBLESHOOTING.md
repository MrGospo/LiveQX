# Troubleshooting

Hints arranged by symptom: where to look and what to try.

## I cannot sign in

- **Wrong login or password** — check the keyboard layout and Caps Lock.
- **The account is locked** — after 5 failed attempts in a row, sign-in is locked for 30 seconds, and for repeated failures the time doubles (up to 1 hour). Wait, or ask an administrator to **unlock** the account (Settings → Users).
- **The account is disabled** — contact an administrator.
- **An LDAP user while the directory is unreachable** — the interface says so; local accounts keep working. Contact an administrator.
- **First sign-in as `admin`** — the one-time password is on the server in the file `/var/lib/liveqx/state/initial_admin_password.txt`.
- **A "Change password" page at sign-in** — an administrator requires you to set a new password (at least 12 characters); you cannot work without it.

## No access to a channel or section

A "forbidden" message or an empty channel list means you lack rights:

- **Viewers and operators** see only channels on which the administrator granted a permission (**View** or **Operate**). Ask to be added on the channel's **Permissions** tab.
- Only an **administrator** can create and delete channels and change system settings.

More: [Users](USERS_USER.md).

## The channel does not start

You clicked **Start**, but the channel goes into an error or stays stopped.

**Where to look:** the **Log** tab of the channel page and, if you are an administrator, the `channel.log` sub-tab; the service journal (commands for administrators are at the end).

Common causes:

- **The playlist is empty.** In that case the channel shows the fallback image (or a black frame) but will start. Check the playlist and the fallback image.
- **An output could not be created.** If the output's parameters are invalid (for example, the HLS directory does not exist or has no write permission), the output is skipped and the channel log shows a "build failed" error.
- **The port is busy.** An SRT output uses a port on the server; if another process holds it, the output will not come up. Change the port.
- **Not enough resources.** Too many channels for the CPU or graphics accelerator; ask an administrator to check the load.

## The channel is running, but the viewer sees no picture

1. Open the **Outputs** tab: the relevant output should show "connected", and the bytes and packets should grow.
2. Check the channel's own picture: the "⋯" menu → **WebRTC preview**. If the preview shows a picture, the problem is in the output or on the viewer's side.
3. Check the address on the viewer's side in VLC from a known-good computer.
4. If the output is "disconnected" or the counters stand still — see the section for the output type below.

## Output problems

### SRT

- LiveQX **waits for the viewer to connect** and does not connect anywhere itself. The receiver must connect to `srt://server-address:port` as a client.
- The output serves one viewer at a time. A second client cannot connect while the first is busy.
- Check the firewall: the SRT port works over UDP.

### Multicast

- **The network does not pass multicast.** Typical for cloud services and virtual machines; IGMP support on the switches is needed. Ask your network administrator.
- **Wrong interface.** On a server with several network cards, choose the **Source network interface** in the output form; otherwise the system picks the route itself.
- **TTL too small.** With `1` the stream does not leave the subnet.
- **The receiver merges channels.** If several channels broadcast into one subnet, each must have its own **Service ID** (Config → MPEG-TS / IPTV).

### RTMP

- **Cannot connect.** Check the URL (`rtmp://` or `rtmps://`), the platform's reachability from the server, and the stream key inside the URL.
- **401/403 or a drop right after connecting.** The key is wrong or outdated — copy it again from the platform.
- LiveQX reconnects by itself, increasing the pause from 1 second to 1 minute. The **Reconnects** counter grows.

### HLS

- **Files do not appear.** Check that the directory exists and the LiveQX service has write permission; whether the disk is out of space.
- **Files exist but the viewer sees nothing.** LiveQX does not serve HLS over HTTP: you need a web server (for example nginx) configured for that directory. The playlist address is `stream.m3u8`.
- **A big delay** — a property of the format.

### Queue and losses

If **Packets dropped** or **Queue drops** grow, the receiver or the network cannot take the stream fast enough. Check the network load and the receiver's state, and if needed raise the output's queue limit.

## Files from a folder do not appear in the playlist

For channels with the content source "Cache" or "Passthrough".

1. Open the **Watcher** tab: it shows how many files the server sees, the mode, the source and the latest errors.
2. Check the extensions. `.jpg`, `.jpeg`, `.png`, `.bmp`, `.webp`, `.mp4`, `.avi`, `.mkv`, `.mov`, `.ts` qualify; other files are ignored.
3. Subfolders are not scanned: files must lie directly in the specified folder.
4. A new file appears once its size and modification time stop changing. Wait a few seconds after copying.
5. If the share is unavailable, the "unreachable" counter on the tab grows — check the share's state in **Settings → Storage** (administrator).
6. Click **Rescan**.

## The schedule does not fire

- A schedule **switches the playlist** but does not start the channel: the channel must be running.
- Check the channel and server time zones, and for "once" entries that the time is given in UTC. See [Server time](TIME_USER.md).
- A window that crosses midnight is not supported: split it into two entries.
- Check the "Effective from / to" dates and the priorities of overlapping entries, and the "Upcoming" block. See [Schedule](SCHEDULE_USER.md).

## The channel stutters, FPS is below the target

- **The server is overloaded** — too many channels or encoding that is too heavy. Check the load in **Observability → Metrics** and run a [stress test](STRESS_USER.md) outside working hours.
- **Expensive transitions.** Dissolve and push load the CPU: reduce the duration or choose Crossfade or a hard cut.
- **Resolution or bitrate too high.** Pick values that fit the server (resolution and FPS of an existing channel do not change — you need a new channel).
- **Software encoding.** If the server has a graphics card, choose a hardware encoder (NVENC, QSV, VAAPI) in the channel config.
- **A slow disk**, if the channels have HLS outputs.

## A live source is not shown

- The source must be reachable by the server (RTMP/RTSP/multicast/NDI): check the URL and the network.
- The source is opened during the "warm-up" before the entry starts (5 seconds by default). If it is too slow, increase the warm-up.
- While the source is absent, the channel's fallback image or a black frame is shown.
- See [Live sources](LIVE_INPUT_USER.md).

## The interface does not respond after an upgrade

1. Reload the page with a cache clear (Ctrl+F5 or Cmd+Shift+R).
2. Try another browser or device.
3. If that does not help, it is a question for the administrator. On the server:

```
sudo systemctl status liveqx
sudo journalctl -u liveqx -f
```

## What to write in a request

To make it easier for the administrator to investigate, include:

- the exact **time** of the problem;
- the **channel name** and the output;
- **what you did** and **what you expected** versus what you saw;
- a screenshot of the error or of the channel page;
- your username.
