# Quick start

This guide takes you from signing in to a working channel with one output.

## 1. Sign in

Open the LiveQX address in your browser (port `8080` by default). On the sign-in screen enter your username and password.

**First sign-in after installation.** The installer creates the `admin` user and stores its one-time password in the file `/var/lib/liveqx/state/initial_admin_password.txt` on the server. Sign in with it — the system immediately asks you to set a new password (at least 12 characters). After the change, the file with the one-time password is deleted.

If you enter the wrong password several times in a row, the account is temporarily locked (see [Troubleshooting](TROUBLESHOOTING.md)).

## 2. Interface overview

The main menu is on the left. The items you see depend on your role:

- **Dashboard** — a summary of all channels: how many are running, how many have errors, how many outputs are online.
- **Channels** — the channel list and the page of each channel.
- **Gateways** — forwarding of UDP/multicast streams (passthrough, demux, remux, transcode modes, FEC support). A separate feature from channels, not covered in this knowledge base. Administrator only.
- **Plugins** — installing additional modules (.so). Administrator only.
- **Observability** — metrics, events, service health.
- **Operations** — stress test and profiling (operator and above).
- **Settings** — users, LDAP, SMTP, Audit Trail, TLS, Storage, Time.
- **Knowledge Base** — the help you are reading now.

More about roles: [Users](USERS_USER.md).

## 3. Create a channel

Only an **administrator** can create channels.

1. Open **Channels** → **Create channel**.
2. Step **"Channel basics"**: enter a name (Latin letters, digits, `_` and `-`; it cannot be changed later), choose resolution, FPS and bitrate. The defaults are fine for a first test.
3. Step **"Output destinations"**: keep type **SRT**, address `0.0.0.0` and port `4000`.
4. Click **Create channel**.

More: [Creating a channel](CREATE_CHANNEL.md).

## 4. Add content

Open the channel → **Playlist** tab.

- **Add file** — enter the path to a file **on the server** or pick it with **Browse**.
- **Add folder** — all media files in the folder are added at once.

Files are not uploaded through the browser: they must already be on the server or on a connected [network share](MOUNTS_USER.md).

More: [Channel playlist](PLAYLIST_USER.md).

## 5. Start

On the channel page click **Start**. Within a few seconds the state changes to "running".

The easiest way to check the picture is right in the browser: the "⋯" menu → **WebRTC preview**.

To see the stream the way a viewer does, open `srt://server-address:4000` in VLC — the SRT output waits for a viewer to connect.

## 6. What next

- Add more outputs (multicast, RTMP, HLS, NDI) — [Outputs](OUTPUTS_OVERVIEW.md), [Adding an output](ADD_OUTPUT.md).
- Pick up files from a network folder automatically — [Content source](CONTENT_SOURCE_USER.md).
- Switch playlists automatically by time — [Schedule](SCHEDULE_USER.md).
- Something does not work — [Troubleshooting](TROUBLESHOOTING.md).
