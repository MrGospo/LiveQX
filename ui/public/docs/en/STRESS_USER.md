# Stress test

A stress test creates many **temporary artificial channels** on the server, keeps them on air for a set time (optionally simulating failures along the way) and checks that the server withstood the load without dropped frames, memory leaks or channel crashes. The result is a report with a verdict of "Pass" or "Fail".

Section **Operations → Stress test**.

## Who can do what

- **Operators** and above — see the current run, the schedule, the last verdict and the list of reports.
- **Administrators** — enable and disable the stress test, change the schedule, start and stop a run manually, open the detailed report and delete reports.

## What it is for

- Check a **new server** before putting live broadcasts on it.
- Make sure an **upgrade** of LiveQX has not made performance worse.
- Find out how many channels your hardware can carry.
- Regularly (on a schedule) check stability: no memory leaks or crashes.

## When not to run it

The test creates real channels and loads the CPU, memory and network, so on a production server during working hours it can degrade real broadcasts. Run it on a dedicated machine or outside working hours.

## What is on the page

- **Enable stress test** — the master switch. While it is off, scheduled runs and manual runs are unavailable.
- **Current run** — progress, remaining time and the run state.
- **Schedule** — the time of the daily automatic run (02:00 by default).
- **Last pass** and **Last verdict**.
- The **Start manual run** and **Stop** buttons.
- **Reports** — a table of finished runs: start, end, duration, verdict. The server keeps up to 100 latest reports; older ones are deleted.

## How a run goes

1. The server creates the set number of test channels (50 by default), each with several outputs.
2. The channels start and work for the whole set time (24 hours by default).
3. If failure scenarios are enabled, problems are simulated during the test: breaking random outputs, corrupting clips, cutting the network on an interface.
4. At the end the test channels are removed, metrics are collected and a verdict is issued.

The run parameters (number of channels, duration, resolution, outputs per channel, scenarios, criteria) are stored in the stress test configuration on the server and are changed by an administrator through the REST API (`PUT /api/stress/config`); in the interface only the schedule and the master switch are configured.

## Pass criteria

A run **passes** if all conditions are met:

- on no channel did the frame loss exceed **1 %** (the deviation of the actual FPS from the expected);
- the growth of used memory, converted to one hour, is no more than **0.1 %**;
- no channel crashed during the test (the allowed number of crashes is 0).

Otherwise the verdict is **Fail**.

## The report

The report page (administrator) contains:

- a **Summary**: verdict, scenario, duration, number of channels, drops, underruns, average FPS, the state of the system before and after;
- **Per-channel results** — a table of each channel's figures;
- a **Timeline** — graphs over the run, including drops per second;
- a **Configuration snapshot** — the parameters the run used;
- the **Raw JSON** and a **Download JSON** button.

## What to do with a bad result

1. Look at the CPU load: if it hits the limit, you need more cores or fewer channels. Also check the **Observability → Metrics** page.
2. Check memory: does it grow during the test (a sign of a leak).
3. Check the network: the total bitrate of all outputs must not exceed the network card's capability.
4. Check the disk if the test channels have HLS outputs: a slow disk causes delays.
5. Try a smaller number of channels to find the safe limit, and leave a margin of about 30–50 % for the production load.

## Profiling

Next to it in the **Operations** section there is a **Profiling** page (available to operators): it shows which processing stages a channel's time is spent on. The "Instrumentation" mode adds load on every frame, so turn it on briefly and deliberately.
