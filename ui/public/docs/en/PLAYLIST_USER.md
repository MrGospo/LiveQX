# Channel playlist

A playlist is an ordered list of entries that a channel plays in a loop. This article explains how to manage it manually.

> If the channel has a **content source** ("Cache" or "Passthrough" mode, see [Content source](CONTENT_SOURCE_USER.md)), the playlist is built automatically from the folder contents. Manual adding and removing is then disabled, and a banner on the tab explains it.

## Where to open it

Channel page → **Playlist** tab. Operators and administrators can manage the playlist (an operator only on channels where they have the "Operate" permission).

## Files are not uploaded through the browser

The playlist stores **paths to files on the server**. There is no drag-and-drop from the browser and no upload through the interface: files must already be on the server or on a connected [network share](MOUNTS_USER.md) (visible on the server under `/mnt/liveqx/`).

## Adding entries

There are three buttons above the list:

### Add file

Enter the path to a file on the server or pick it with **Browse**. Optionally set a duration (for photos). Click **Add**.

File browsing is available to administrators only; an operator enters the path manually.

### Add folder

Specify a folder on the server — all media files in it are added to the playlist at once. After adding, the number of files added is shown.

### Add live source

An entry that inserts an external stream into the broadcast for a set time. More: [Live sources](LIVE_INPUT_USER.md).

### Supported formats

The file picker offers:

- **Photos:** PNG, JPG/JPEG, WebP, BMP.
- **Video:** MP4, MOV, MKV, AVI, TS, M2TS, WebM.

Automatic folder synchronization uses a narrower list — see [Content source](CONTENT_SOURCE_USER.md).

## Playback order

Entries are played from top to bottom; when the end is reached, the playlist starts over.

To change the order, use the **Move up** and **Move down** arrows in the entry's row. There is no drag-and-drop with the mouse. There is no random shuffle either.

## Editing an entry

The **Edit item** button in a row opens a window where you can change:

- the file path;
- the **duration** (in seconds) — for photos only; ignored for video. An empty value means "use the channel's default duration";
- for a live entry — the type, URL, duration, warm-up and signal-loss threshold.

## Duration

- A **photo** is shown for as many seconds as set in its entry, and if that is empty — as many as the channel's "Photo duration" setting says (10 seconds unless changed).
- A **video** plays for as long as the file lasts. This cannot be changed.

## Fallback image

If the playlist is empty or a file is unavailable, the channel shows the **fallback image** — a static picture. It is set on the **Config** tab in the **Fallback image** block: the path to a PNG, JPG, WebP or BMP on the server. The change takes effect **after the channel is restarted**. If no fallback image is set, a black frame is shown.

## Deleting and clearing

- The delete button in a row removes the entry from the playlist. The file itself on disk is not deleted.
- The **Clear** button removes all entries.

## If files disappear from disk

If some files from the playlist no longer exist, a warning "Missing files detected" appears at the top with a **Re-scan playlist** button.

## Playback log

The **Log** tab shows what was played and when: time, status (`completed`, `skipped`, `removed`, `error`), path, type, transition, how long was played, and the error reason. There are filters by time and status, and text search.

The log works only if playback recording is enabled in **Config** (File or SQLite); otherwise the tab says the log is disabled.

The **Clear log** button removes records for the chosen period (older than 7 days, older than 30 days, by the current filters, or the whole log) — irreversibly.

The tab also has a live view of the `channel.log` file; it is available to administrators only.

## FAQ

**What happens if the playlist has a single photo?**
The channel shows it indefinitely. This is a normal mode for information displays.

**Can I change the playlist of a running channel?**
Yes, changes are applied without stopping.
