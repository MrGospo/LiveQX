# Network shares

A network share is a folder on a remote server (NAS, file server, workstation) that LiveQX attaches to the server as a local one. Channels can take content from it in [folder content source](CONTENT_SOURCE_USER.md) mode and add files to the [playlist](PLAYLIST_USER.md).

Section: **Settings → Storage**. Available to **administrators only**.

Connected shares are visible on the server in the `/mnt/liveqx/` directory. The connection is set up as system mount points and **survives a server reboot**.

## The list

For each share the table shows:

- **Target** — the path on the server, for example `/mnt/liveqx/media`;
- **Type** — `cifs` or `nfs`;
- **Source** — the share's address on the remote server;
- **State** — Active, Activating…, Inactive, Failed or Unknown;
- **Credentials** — for CIFS: the user name (an asterisk means the password is saved) or `guest`;
- **Updated** — when the state was last checked.

The row offers **Test**, **Refresh status**, **Edit** and **Delete**.

## Types

### CIFS (SMB, "Windows share")

The Windows and Samba file-sharing protocol. Source: `//192.168.1.10/share` or `//nas.local/media`. Connecting requires a **user name** and password, and a **domain** if needed, or `guest` mode (no password).

### NFS

The Linux/Unix and enterprise-storage protocol. Source: `192.168.1.10:/srv/share`. No credentials are needed; access is allowed by IP address on the storage side.

## Adding

1. **New mount** at the top right.
2. Fill in:
   - **Filesystem** — CIFS or NFS.
   - **Source** — the share address (see above).
   - **Target** — a path inside `/mnt/liveqx/`, for example `/mnt/liveqx/media`. The folder is created automatically.
   - **Mount options** — optional; comma-separated, without `ro` (there is a toggle for that). Hints: for CIFS `vers=3.0,iocharset=utf8`, for NFS `vers=4.1`.
   - **Read-only** — recommended to keep on if channels only read content: that way files cannot be accidentally changed or deleted.
   - For CIFS: **Username**, **Password**, **Domain**, or `guest`.
3. Click **Test** — the server tries to mount the resource and reports whether it is reachable (or why not).
4. Save. Source and target are required; for CIFS a username is required.

After saving, the state changes to "Active" or "Failed" with a description.

## Use in channels

Once the share is connected, its contents are available at `/mnt/liveqx/…`. This path is specified:

- in the channel's **Content source** settings (the folder for "Cache" or "Passthrough" modes);
- when adding a file or folder to a playlist manually.

Several channels can use the same share.

## Editing

**Edit** opens the same form. To keep the current CIFS password, leave the password field empty. The password itself is never shown in the interface.

## Deleting

**Delete** unmounts the share and removes the entry. Files on the remote server itself are not touched. Channels that took content from this share lose access to new files; cached files ("Cache" mode) keep playing.

## State and diagnostics

- **Refresh status** asks the system for the share's state again.
- If the state is "Failed", check the address and credentials, the network reachability of the remote server, and the protocol version in the options (`vers=…`).
- To check that a channel sees the files, open the channel's **Watcher** tab — see [Content source](CONTENT_SOURCE_USER.md).

## Security

- The CIFS password is stored in the LiveQX database in encrypted form (with the Master Key). For the connection itself, the mount service temporarily writes it to a protected in-memory file (`/run/liveqx/creds/`, accessible to root only) that disappears on reboot. The password is never shown in the interface.
- The "Read-only" mode protects files on the share from accidental changes.
