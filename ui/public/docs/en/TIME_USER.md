# Server time

Time matters to LiveQX: the windows of the [schedule](SCHEDULE_USER.md) and the timestamps in the playback log and the Audit Trail depend on it.

Section **Settings → Time** (the "System time" page). Only an **administrator** can change the settings.

## What "effective time" is

LiveQX uses its own "effective" time: it is the server's system clock plus an offset set by the selected source. **LiveQX does not change the operating system's own clock.** The offset applies only inside LiveQX: to the schedule, the playback log and the Audit Trail.

The **Runtime snapshot** block shows the active source, the current offset, the effective time and the system clock reading.

## Time source

One of three modes is chosen:

- **Local server time** — the host clock is used as is, with no offset. The default mode.
- **NTP** — LiveQX polls one or more SNTP servers and uses the discovered difference as an offset to the system clock.
- **Manual** — a fixed time set by the operator. LiveQX remembers the difference between the set time and the host clock, and it persists across restarts.

If the server itself syncs with NTP through the operating system (chrony, systemd-timesyncd), leave the "Local server time" mode.

## Server time zone

It is specified by an **IANA name** (for example `Europe/Moscow`, `Asia/Yekaterinburg`, `UTC`). Choose from the **Preset** list or enter your own value ("— custom —").

The server time zone:

- is used for displaying time;
- is inherited by channels that have no zone of their own: schedule windows are calculated in it.

The time zone is a presentation setting and does not depend on the selected time source.

## NTP settings

The block appears when NTP mode is selected:

- **Enable NTP polling**;
- **NTP servers** — one per line, in the `host` or `host:port` format;
- **Poll interval** — in seconds;
- **Last sync** and **Last offset** — the result of the last poll;
- **Probe servers** — a one-time poll with a table: server, status, offset, response time (RTT).

## Manual time

In "Manual" mode, enter the time that LiveQX should consider current. The time is interpreted in your browser's time zone; the **Use browser now** button fills in your computer's current time. Only the difference from the host clock is stored on the server.

## Channel time zone

Each channel can have **its own** time zone: channel page → **Config** tab → **Timezone**. By default a channel **inherits** the server zone. Setting its own makes sense if the channel broadcasts to another region: then its schedule windows are calculated in local time.

A channel's new zone is applied **at the next channel start**. If a channel inherits the server zone, then when the server zone changes, it picks up the new value automatically.

## Time zone and the schedule

The hierarchy is **server → channel**. Start and end times of daily, weekly and monthly schedule entries are calculated in the channel's zone. "Once" entries are always specified in UTC. A schedule entry has no zone of its own.

## If the schedule fires at the wrong time

1. Compare the effective time in the "Runtime snapshot" block with the real time.
2. Check the server time zone.
3. Check the channel time zone: whether it is inherited, and if set manually, whether it is correct.
4. For "once" entries make sure the time is given in UTC.
5. If necessary, fix the time source (NTP or a manual offset).

## Effect on running channels

Changing the time zone or the time source does not stop channels. Only how the next schedule firings and timestamps are calculated changes.
