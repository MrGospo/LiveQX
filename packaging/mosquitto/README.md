# Mosquitto for LiveQX

LiveQX talks to its screens through a Mosquitto 2.0.18 broker with the Dynamic Security
plugin. The stock 2.0.18 broker has a bug that LiveQX triggers in normal operation, so the
broker has to be built with the patch in `patches/`.

## The bug

When the broker rereads its settings (SIGHUP) it walks every client context, including
persistent sessions whose client is offline. With `use_identity_as_username` such a context
has no TLS connection, so the broker "disconnects" it a second time. That puts the context
into the session expiry list twice; the first entry is left pointing at the context, which is
freed later, and `session_expiry__check()` reads freed memory. The broker dies with SIGSEGV
some time after the reload.

LiveQX sends SIGHUP after every publication of the certificate revocation list (server start,
daily refresh, every revocation), and screens that are switched off (every night) keep
persistent sessions on purpose: commands for them wait in the broker. So the stock broker is
not safe here.

Minimal reproduction without LiveQX: connect one MQTT 5 client with clean start off and a
session expiry above zero, disconnect it, send SIGHUP to the broker. Found with
AddressSanitizer. The affected code is the same in 2.0.22.

## The fix

`patches/0001-…patch` (two small changes, explained in its header):

* an unconnected context is skipped when settings are reread — it is judged when it connects;
* a context is put into the session expiry list at most once.

## Building

    packaging/mosquitto/build.sh [PREFIX]

Downloads the upstream 2.0.18 source (checksum pinned), applies the patches, builds only the
broker and installs `PREFIX/sbin/mosquitto` (default `build/mosquitto-patched`). The Dynamic
Security plugin and `mosquitto_ctrl` come from the distribution's mosquitto package, which must
be the same version.

The test suite uses `build/mosquitto-patched/sbin/mosquitto` when it exists. The test
`RereadingItsSettingsLeavesTheSessionsOfOfflineClientsAlone` checks the fix and is skipped
without the patched broker (it fails on the stock one).

## Installing and running it

`packaging/install.sh` does all of this (`--broker=local`, the default answer; `--broker=none` for an
external broker). The distribution's `mosquitto` and `mosquitto-clients` packages are installed for
the Dynamic Security plugin and `mosquitto_ctrl` only; their own `mosquitto.service` is not used (the
installer stops it if it was the one that brought the package in).

| What | Where |
|---|---|
| the broker (our build) | `/usr/local/libexec/liveqx/mosquitto` — the file name matters, the server signals only a process called `mosquitto` |
| its configuration | `/etc/liveqx/mosquitto.conf` (from `mosquitto.conf.in`; yours once it exists, a newer template is saved as `.dist`) |
| services | `liveqx-broker.path` (enabled) starts `liveqx-broker.service` (`Restart=always`, never gives up) |
| everything it reads or keeps | `/var/lib/liveqx/state/broker/` — owned by `liveqx`, the same user as the server |
| the process-id file | `/run/liveqx-broker/mosquitto.pid` (the server signals the broker through it) |

**Order of things on a new installation.** Before the device CA ceremony the server has nothing to
give the broker, so the broker is not started. After the ceremony the server writes, within about a
minute, `cafile.pem` (the device CA certificates of every generation), `broker.crt` and `broker.key`
(the broker's own certificate), `crl.pem` (the revocation list) and last of all `ready`; the path unit
sees `ready` and starts the broker. Nothing has to be started by hand. The Screens → Broker page
shows each step.

**The broker's certificate.** If the server holds the key of the root the screens are told to trust
(the default, `auto` TLS mode), it issues the certificate for the host in the `mqtt_broker_url`
setting and for the addresses of this machine, valid from a day ago (clocks of the screens drift),
and renews it 30 days before the end or when the host in the settings changes. If an administrator
brought their own root (the `ca_cert_pem` override), the server cannot issue one: put `broker.crt` and
`broker.key` into the broker directory yourself (the key readable by `liveqx` only). The page says
which of the two it is, and until when the certificate is good.

**Stopping and starting by hand.** `ready` stays, and the path unit would start the broker again, so
stop both: `systemctl stop liveqx-broker.path liveqx-broker.service`; start with
`systemctl start liveqx-broker.path`. A reload without stopping: `systemctl reload liveqx-broker`.

**Rights.** `dynamic-security.json` holds the broker's rights and is written by the broker itself.
The server rebuilds what players need from its own database (every few minutes and after any change),
so losing the file is not a loss of data; initialise it again with `mosquitto_ctrl dynsec init` as the
installer does (`liveqx-server` is the administrator, by certificate; the password asked for is never
used) and restart the broker. Never run `dynsec init` over a working file: it drops every right.

**Do not**
* rotate the broker's log with a signal; it logs to the journal (`journalctl -u liveqx-broker`);
* open port 1883 or any listener without client certificates: the configuration has one listener,
  on the port you chose (default 8883), and screens need a certificate to get in;
* forget the firewall: the screens come from outside, the port has to be reachable.
