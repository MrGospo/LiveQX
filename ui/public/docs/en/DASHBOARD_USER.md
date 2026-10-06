# Dashboard and channel overview

This article covers two pages: the **Dashboard** — a summary of all channels — and the **Overview** tab on an individual channel's page.

## Dashboard

The **Dashboard** menu item shows the state of all channels on one screen. A viewer sees only the channels they have been granted access to; operators and administrators see all channels in the list.

### Summary figures

Four numbers at the top:

- **Total channels**;
- **Active** — how many channels are running;
- **With errors** — red when greater than zero;
- **Outputs online** — how many outputs are healthy right now.

### Filters and buttons

- Filters: **All**, **Running**, **Stopped**, **Failed**, **Degraded**.
- **Refresh** — reload the data (normally the data updates by itself).
- **Create channel** — administrators only.

### Channel card

Each channel's card shows:

- a name like `ch<number>-<name>` and a **state badge**;
- the content source (folder path) and the fallback image, if set;
- **Now playing** — the clip number and how much is left until its end;
- **outputs** (SRT, MULTICAST, RTMP, HLS, NDI) — only while the channel is running; an unhealthy output is highlighted red;
- the **Open** button (open the channel page), **Start** or **Stop**, and **Next** (next clip), and for a running channel, **WebRTC preview**.

Control buttons are available if you have the right to operate the channel.

### Channel states

- **running** (green) — the channel is working;
- **starting** — it is starting up;
- **degraded** (yellow) — working with problems, for example one of the outputs is unhealthy;
- **failed** (red) — an error; details are in the channel log;
- **stopped** (gray) — stopped.

## The channel "Overview" tab

It opens when you go to a channel's page.

### Header

The channel name and the buttons: **Start**, **Stop**, **Restart** and **Next clip**. In the "⋯" menu: **WebRTC preview**, **Edit config** and (administrators only) **Delete channel**.

### "Status" block

- **State** and **Health**;
- **FPS** — the current frame rate and the **target**. If FPS is noticeably below the target, the channel cannot keep up with real time;
- **Resolution**, **Preset**, **Bitrate**, **NUMA**.

### "Now playing" block

- **Clip** — the path to the file or a live-source label;
- **Elapsed**, **Remaining**, **Duration** and **Progress**.

### "Outputs" block

All outputs of the channel with their state and counters. More: [Outputs overview](OUTPUTS_OVERVIEW.md). If there are no outputs, the message "No outputs configured" is shown.

### "Recent events" block

The channel's latest events: clip changes, errors, reconnects.

## Other channel tabs

- **Playlist** — [Channel playlist](PLAYLIST_USER.md).
- **Schedule** — [Schedule](SCHEDULE_USER.md).
- **Outputs** — the list of outputs, adding, editing, restarting, deleting: [Adding an output](ADD_OUTPUT.md).
- **Log** — the playback log and the `channel.log` file.
- **Config** — encoding, the default transition, the fallback image, the content source, the playback log, the time zone.
- **Watcher** — the state of folder synchronization: [Content source](CONTENT_SOURCE_USER.md).
- **Permissions** — which viewers and operators are allowed access to the channel (administrator).

## Real-time updates

Pages receive updates from the server automatically. If the data looks "frozen", reload the page.
