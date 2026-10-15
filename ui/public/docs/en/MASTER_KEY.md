# Master key

Section: **Settings → Master key**. Available to the **administrator only**.

The master key is the key LiveQX uses to encrypt the secrets it stores:

- LDAP and SMTP passwords;
- network share (CIFS) passwords;
- the secret that signs user sessions;
- the signature of the action log (it shows that log entries have not been forged).

User passwords are **not** encrypted with this key: they are stored as one-way hashes.

If the key is stolen together with the data, the encryption protects nothing. If the key is lost, the stored secrets cannot be read. So it matters **where** the key lives and **how** it is backed up.

## Where the key can live

| Way | What it is | When to choose it |
|---|---|---|
| **File outside the data directory** | `/etc/liveqx/master.key`, owner `root`, group `liveqx`, mode `0640`. The installer's default | A normal installation. A copy of the data directory (backup, VM snapshot) does not carry the key |
| **systemd credential** | The key is encrypted by systemd and bound to this server, and to its TPM2 module when there is one | A server with a TPM2: even a copy of the whole disk cannot open the key on another machine |
| **Passphrase** | The key file is encrypted with a passphrase the administrator types | You need protection even against theft of the whole server, and you accept typing the passphrase after every start |

The **Master key** page shows how the key is stored now. If the key sits **in the data directory** (as in older installations), a warning appears at the top: any copy of the directory carries the key with it, and the encryption stops helping.

Without a TPM2 a systemd credential protects against a copy of `/etc` and of the data directory, but **not** against a copy of the whole disk: the server's secret is on the same disk.

## Choosing at install time

```
sudo ./packaging/install.sh --master-key-store=file         # default
sudo ./packaging/install.sh --master-key-store=creds        # systemd credential
sudo ./packaging/install.sh --master-key-store=passphrase   # passphrase
```

`creds` needs systemd 250 or newer. For `passphrase` the installer asks for the passphrase in the terminal.

## An existing installation: move the key out of the data directory

The key moves **without re-encryption**: the same 32 bytes are simply stored elsewhere.

```
sudo ./packaging/install.sh migrate-master-key
```

The command stops the service, moves the key to `/etc/liveqx/master.key`, starts the service and, only once it is running, removes the old file. If anything fails, everything is put back.

Old backups of the data directory still contain the **previous** key. To make them useless, rotate the key after the move (see "Rotation").

## Passphrase

Turn passphrase protection on (the service must be stopped):

```
sudo systemctl stop liveqx
sudo liveqx --config /etc/liveqx/config.json --protect-master-key
sudo systemctl start liveqx
```

The passphrase is asked for twice, must be at least **12 characters** and is not shown on screen. The data key itself does not change, so nothing needs re-encrypting.

Other commands:

| Command | What it does |
|---|---|
| `--change-master-passphrase` | Changes the passphrase (asks for the old and the new one) |
| `--unprotect-master-key` | Removes passphrase protection |

For scripts without a terminal add `--passphrase-stdin`: passphrases are read from standard input, one line per question.

### What happens at start

A service with a passphrase starts **sealed**: it is running but serves only a "The service is sealed" page. Open the usual LiveQX address in a browser and type the passphrase. The service then starts normally on the same address.

**Important:** until the passphrase is entered nothing works: channels are not loaded and nothing is on air. After a reboot or a power failure the broadcast will not come back by itself until someone types the passphrase. If the server must return to air unattended, choose the file or a systemd credential.

Brute-force protection: after a few wrong attempts from one address, input is paused and the pauses grow up to 5 minutes. During a pause even the correct passphrase is refused.

## Rotation (changing the key)

Rotation creates a **new** key and re-encrypts every secret with it. The service must be stopped:

```
sudo systemctl stop liveqx
sudo liveqx --config /etc/liveqx/config.json --rotate-master-key
sudo systemctl start liveqx
```

The old key stays next to it as `master.key.bak.<time>` — keep or delete it deliberately. If even one secret cannot be decrypted with the old key, rotation refuses to run and **changes nothing**.

If the key is protected by a passphrase it is asked for, and the new key keeps the same passphrase.

Rotation does not apply when the key is delivered from outside (an environment variable or a systemd credential): LiveQX cannot replace it there. Create the new key in its store and re-encrypt the secrets.

## Reset

Reset is for when the key is **lost**. It wipes every secret encrypted with the old key (LDAP, SMTP and share passwords, the session secret) and creates a new key. User accounts and their passwords are kept.

```
sudo systemctl stop liveqx
sudo liveqx --config /etc/liveqx/config.json --reset-master-key --confirm-reset
sudo systemctl start liveqx
```

After a reset, enter the LDAP, SMTP and share passwords again. All users are signed out.

## Backing up the key

**Copy the key separately from the data.** A backup of the data without the key is unreadable, and a key next to the data protects nothing.

- File: save `/etc/liveqx/master.key` somewhere safe, not where the data backup lives.
- Passphrase: save the key file and **remember or safely write down the passphrase**. The key cannot be opened without it.
- systemd credential: the key exists nowhere else, so save a decrypted copy: `sudo systemd-creds decrypt /etc/credstore.encrypted/liveqx_master_key - | base64`. Without it the key is lost if the server or its TPM fails.

## What this does not protect against

If an attacker gets administrator rights on the operating system of a **running** server, they can read the key from the process memory. The protection is meant for theft of a disk, a machine image or a backup, not for that.
