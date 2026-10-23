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

## Running the broker

* Do not rotate the broker's log by signalling it: logrotate hooks that send SIGHUP run into
  the same code. Log to the journal, or use `copytruncate`.
* Run it under a supervisor with `Restart=always`.
