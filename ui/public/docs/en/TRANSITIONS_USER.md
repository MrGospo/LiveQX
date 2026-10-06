# Transitions

A transition is an effect between two clips in a playlist. When one file ends and the next begins, the channel can either switch instantly (a "hard cut") or apply an effect.

## Where it is configured

- The **default transition** for all clip boundaries of a channel — channel page → **Config** tab → **Default transition** block.
- **A transition in a schedule entry** — when creating or editing a [schedule](SCHEDULE_USER.md) entry there is its own "Transition (type / mode / duration sec / easing)" block. It applies when switching to that entry's playlist.

## Transition parameters

- **Type** — which effect to apply.
- **Duration** — how many seconds the effect lasts. **2 seconds** by default. A value of `0` means a hard cut.
- **Easing** — how the speed of the effect changes over time.
- **Mode** — how the clips behave during the transition (available in schedule entries, see below).

If no transition has been configured on a channel, a **crossfade** of 2 seconds with linear easing is used.

## Transition types

There are 12 types.

- **Crossfade** — the old frame gradually fades out, the new one fades in. A universal soft transition.
- **Wipe** — the new frame slides over the old one. Four directions: left, right, up, down.
- **Push** — the old frame slides away in one direction while the new one pushes it out; both frames move at the same time. Four directions: left, right, up, down.
- **Dissolve** — similar to crossfade but with a noise effect.
- **Fade to black** — the old frame fades to black, then the new one appears from black.
- **Hard cut (hardcut)** — an instant switch with no effect. Duration and easing are not used.

In the "Default transition" block a hard cut is selected with the value "None".

## Easing curves

- **Linear** — the same speed from start to end.
- **Ease in** — the effect starts slowly and speeds up.
- **Ease out** — starts fast and slows down toward the end.
- **Ease in-out** — slow start, speeds up in the middle, slows down at the end.

## Modes (in schedule entries)

- **hard_cut** — an instant switch; the duration is ignored.
- **freeze_fade** (default) — for the duration of the transition both clips are "frozen": the old one on its last frame, the new one on its first. The clips themselves play in full, and the total airtime grows by the transition duration.
- **live_mix** — the old clip is frozen while the new one plays "live" from the very beginning of the transition and continues without a pause. The total time does not grow.

## When a transition is not applied

- If the transition duration is zero or a hard cut is selected.

## Performance

Transitions are computed on the CPU. On a server with many channels, complex effects (especially Dissolve) noticeably load the processor. If FPS drops below the target on the channel page or in monitoring, reduce the duration or choose Crossfade or a hard cut.
