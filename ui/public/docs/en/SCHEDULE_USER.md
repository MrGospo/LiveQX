# Schedule

A schedule lets a channel **automatically switch to another playlist** at set hours. For example: one set of clips in the morning, another in the evening, and a special one on weekends.

Important: a schedule **does not start or stop** a channel. Starting and stopping is done manually with the **Start** and **Stop** buttons. The schedule only decides *what* a running channel plays.

Open it on the channel page → **Schedule** tab. Operators (with the "Operate" permission on the channel) and administrators can edit the schedule.

## How it works

- A channel has a **regular playlist** (the "Playlist" tab).
- Each schedule entry is a **time window** with its **own playlist**. While the window is active, the channel plays the entry's playlist.
- When no window is active, the channel returns to the regular playlist.
- If several windows are active at once, the entry with the **higher priority** wins.

The tab shows a **Currently active** block (which entry is in effect), a list of **upcoming firings** and a daily timeline.

## Entry parameters

- **ID** — a unique name for the entry.
- **Priority** — a number from 0 to 1000 (100 by default). When windows overlap, the higher value wins.
- **Loop mode:**
  - `loop` — the entry's playlist loops while the window is active;
  - `play_once_then_idle` — the playlist plays once, then the channel waits for the window to end.
- **Hard switch** — if enabled, the current clip is interrupted immediately when the window starts. If disabled, the switch happens at the nearest clip boundary.
- **Effective from / to** — optional dates (inclusive) outside which the entry does not work. Handy for seasonal and holiday entries.
- **Playlist** — paths to clips on the server that play in the window.
- **Transition** — type, mode, duration and easing of the transition when switching. See [Transitions](TRANSITIONS_USER.md).
- **Recurrence** — when the window fires (see below).

## Recurrence kinds

- **Once** — from an exact start date and time to an exact end date and time. Time is given in **UTC**. The start is inclusive, the end is exclusive.
- **Daily** — every day from the **start time** to the **end time**.
- **Weekly** — the same window, but only on the selected days of the week.
- **Monthly** — the same window on the selected days of the month (1–31). If a month has no such day (for example, February 31), the entry is simply skipped for that month.

A window that **crosses midnight** (for example, 22:00–02:00) is currently **not supported**: the end time must be later than the start time. For a night broadcast create two entries: until 23:59 and from 00:00.

## Time zone

Start and end times for daily, weekly and monthly entries are calculated in the **channel's time zone**. By default a channel inherits the server's time zone; you can set it separately on the **Config** tab → **Timezone** (the change takes effect at the next channel start). More in [Server time](TIME_USER.md).

"Once" entries are always specified in UTC.

## Several entries

You can have as many entries as you like. For example:

- 06:00–10:00 every day — the morning playlist (priority 100);
- 18:00–23:00 on weekdays — the evening playlist (priority 100);
- 12:00–13:00 on Fridays — a special playlist (priority 200, overrides the others).

## If the schedule fires at the wrong time

1. Check the server and channel time zones — see [Server time](TIME_USER.md).
2. Make sure that for "once" entries the time is given in UTC.
3. Check the "Effective from / to" dates and the priorities of overlapping entries.
4. Open the **Upcoming** block and compare the times with your expectations.

## Holidays

There is no separate holiday support. For a special day add a "once" entry, or an entry with "Effective from / to" dates and a higher priority.
