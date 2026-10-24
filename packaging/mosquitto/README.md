# Mosquitto for LiveQX

LiveQX talks to its screens through a Mosquitto broker with the Dynamic Security plugin. We build
it ourselves from the unmodified upstream release (`build.sh`): the broker, the plugin and
`mosquitto_ctrl` from one source, so they always match. The distribution's package is not used,
for two reasons.

**Old releases can kill the broker.** Up to and including 2.0.22, when the broker rereads its
settings (SIGHUP) it walks every client context, including persistent sessions whose client is
offline. With `use_identity_as_username` such a context has no TLS connection, so the broker
"disconnects" it a second time. That puts the context into the session expiry list twice; the first
entry is left pointing at the context, which is freed when the session expires, and the broker reads
freed memory and dies with SIGSEGV. LiveQX sends SIGHUP after every publication of the certificate
revocation list (server start, daily refresh, every revocation), and screens that are switched off
(every night) keep persistent sessions on purpose: commands for them wait in the broker. So such a
release is not safe here. Ubuntu 24.04 ships 2.0.18.

Reproduction without LiveQX: one MQTT 5 client with a client certificate, clean start off and a
session expiry of a few seconds, disconnect it, send SIGHUP to the broker, wait for the session to
expire (found with AddressSanitizer on 2.0.18; checked again on 2.0.22's code).

**It is fixed upstream in 2.0.23** (2026-01-14): a context is put into the expiry list at most once.
That is what `build.sh` builds, with no local patch. (2.0.23 still "disconnects" an offline session
again at every reload — a spurious line in the broker's log, nothing more.) The test
`RereadingItsSettingsWhileASessionWaitsForItsClientDoesNotKillTheBrokerWhenTheSessionExpires` guards
against going back: it fails on 2.0.18. `packaging/check.sh` refuses a `build.sh` older than 2.0.23.

The second reason is the plugin: it has to be of the same release as the broker, and the distribution
carries only its own. Building all three from one source removes the question.

## Building

    packaging/mosquitto/build.sh [PREFIX]

Needs `curl`, a C compiler, `make`, `libssl-dev` and `libcjson-dev`. Downloads the upstream source
(checksum pinned in the script — take it from a trusted copy when you move to a newer release,
or verify the detached `.asc` signature of the tarball), builds the broker, the Dynamic Security
plugin and `mosquitto_ctrl`, and installs them side by side in `PREFIX/libexec` (default
`build/mosquitto`); `mosquitto_ctrl` finds its `libmosquitto.so.1` next to itself.

The test suite uses this build when it exists (broker, plugin and tool together), otherwise
whatever `mosquitto` is installed — the test about the reload is then skipped.

## Installing and running it

`packaging/install.sh` does all of this (`--broker=local`, the default answer; `--broker=none` for an
external broker). It needs the build above and installs only `libcjson1` from the distribution (the
plugin and `mosquitto_ctrl` need it); the distribution's own `mosquitto` package is not needed and is
not touched.

| What | Where |
|---|---|
| the broker (our build) | `/usr/local/libexec/liveqx/mosquitto` — the file name matters, the server signals only a process called `mosquitto` |
| its Dynamic Security plugin and `mosquitto_ctrl` | `/usr/local/libexec/liveqx/` — the same release as the broker |
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
