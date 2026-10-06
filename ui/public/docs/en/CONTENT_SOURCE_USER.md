# Content source

A content source is the rule by which a channel gets its playlist. It is set when creating a channel or later on the **Config** tab → **Content source** block.

There are three modes.

## Off (manual playlist)

The channel does not read files from a folder. The operator fills in the playlist manually — see [Channel playlist](PLAYLIST_USER.md). This mode is also suitable for a channel that plays only [live sources](LIVE_INPUT_USER.md).

## Passthrough

The channel watches the specified **folder** and plays files directly from it, without copying.

- New files appear in the playlist quickly.
- If the folder disappears (for example, a network share drops), the channel loses access to the content.

Suited for local folders on the server itself.

## Cache

A background service of the channel watches the folder (the **source**, for example a network share), copies files into a **local cache**, and the channel plays from the cache.

- The channel is resilient to network drops: while the share is unavailable, it keeps playing what is already in the cache.
- You need disk space on the server for the file copies.
- The cache path can be left empty — then a `cache` subfolder in the channel directory is used.

Suited for network shares (CIFS/NFS).

## What Passthrough and Cache have in common

### How files get in

- The playlist is built automatically from the **files lying directly in the specified folder**. Subfolders are not scanned.
- On first start, files are added in alphabetical order by name. To set the order you need, begin names with numbers: `01_intro.mp4`, `02_lobby.jpg`, `03_promo.mp4`.
- Files that appear later are added **to the end** of the playlist.
- A file removed from the folder disappears from the playlist; if it is on air right now, it plays to the end.

### Which files qualify

By extension only:

- **Photos:** `.jpg`, `.jpeg`, `.png`, `.bmp`, `.webp`.
- **Video:** `.mp4`, `.avi`, `.mkv`, `.mov`, `.ts`.

All other files (`.tmp`, `.part`, `.upload`, documents, etc.) are ignored without errors.

### Protection from half-copied files

A new file does not enter the playlist at once: the server waits until its size and modification time stop changing between two checks. So it is safe to copy files into the folder while the channel is running.

### Check period

The folder is checked every **2 seconds** (the default). If the folder is unavailable, the interval gradually grows up to 30 seconds, and as soon as access returns, checking goes back to the normal rate.

## How to set up a folder on a network share

1. The administrator connects the share in **Settings → Storage** — see [Network shares](MOUNTS_USER.md). It appears on the server under `/mnt/liveqx/…`.
2. In the content source settings choose **Cache** mode and specify a folder on the share, for example `/mnt/liveqx/media/promo`. An administrator can pick the folder with the browse button; others enter the path manually.

## The "Watcher" tab

It shows the state of the synchronization service: mode, source, cache, number of files and cache size, scan interval, time of the last successful scan, the number of times the share was unreachable, copy errors, files waiting for deletion, and files skipped as too large. The **Rescan** button starts a check immediately.

If the channel has no source configured, the tab says the watcher is disabled.

## What to choose

| Situation | Mode |
|---|---|
| 5–10 files, rarely changed, operator manages manually | Off (manual playlist) |
| Content is in a local folder on the server | Passthrough |
| Another team prepares content on a network share | Cache |
| The channel plays live sources only | Off |

## If the playlist is not updating

See [Troubleshooting](TROUBLESHOOTING.md), the section "Files from a folder do not appear in the playlist".
