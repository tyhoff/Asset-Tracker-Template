# Hardware integration tests

`survey_hwtest.py` drives a real Thingy:91 X over its USB console and asserts on what
the device says back. Nothing here is mocked. The suites under `tests/module` prove
logic against faked modem, filesystem and radios; this proves the things those fakes
stand in for — that the image booted, that a command exists in the build that was
actually flashed, that a scan returns access points, that records survive a reset.

## Running

pyserial is needed, and the SDK toolchain already has it:

```sh
nrfutil toolchain-manager launch --ncs-version v3.4.0 -- \
    python3 tests/hardware/survey_hwtest.py
```

| Invocation | What runs |
| --- | --- |
| *(no flags)* | Everything that needs no radio. Writes nothing to flash and never resets the device. |
| `--radio` | Adds scan and GNSS tests. Needs registration and, for GNSS, a sky view. |
| `--destructive` | Adds tests that append records and cold-boot the device. Asks first; `--yes` answers. |
| `--fast` | Drops the tests tagged slow. |
| `--loopback` | The harness's own self-tests, against the fake shell. No hardware. |
| `--list` | Names, tags, descriptions, and which the current flags would select. |
| `--only <substring>` `-v` | One test, with the serial traffic shown. Filters the selection rather than widening it, so a destructive or radio test still needs its own flag. |

Budget time for `--radio`. Each `trigger()` attempt costs its timeout plus a round trip
for the counters, and retries up to three times when another search is holding the
Location library, so `gnss_fix` alone can run about ten minutes indoors before it skips
and `scan_repeats_without_ebusy` calls `trigger()` three more times.

**What the default run still disturbs:** it injects synthetic data into the observation
cache (`survey selftest`) and clears it (`survey clear`, which also zeroes the counters).
That is not visible in stored records and does not survive a reboot, but it will confuse
a bench session in progress. What it does *not* do is write to the survey partition or
reset the device — those tests are behind `--destructive`, because a stored record cannot
be deleted individually, so a synthetic one stays in the capture for good.

The port is auto-detected among Nordic-VID devices only, since auto-detection writes shell
commands to whatever it picks. The Thingy:91 X exposes two such ports and only one is the
nRF9151 console; the harness says which it chose, and `python3 scripts/survey_console.py
--identify` says which is which.

Exit status: 0 when at least one test passed and none failed, 1 on a failure, 2 on a usage
or serial problem, **3 when everything selected skipped** — an all-skip run is what an
indoor bench produces for `--radio`, and exiting 0 would claim coverage that did not happen.

## Adding a test

```python
@test("name_in_snake_case", tags=(TAG_RADIO, TAG_SLOW))
def t_something(dev):
    """One line, shown in --list."""
    reply = dev.cmd("survey show")
    want_in("expected text", reply)
```

`dev.cmd()` is for commands that answer on the shell. `dev.await_line(pattern, timeout,
command=...)` is for the ones that do not: it keeps reading past the prompt, which is
what asynchronous output requires — match the whole line you intend to parse, not a
prefix, or a read boundary can hand you a truncated one.

Tests run in definition order. Anything that reboots belongs at the end of the file.

Raise `Skip` when a precondition you cannot create is missing (no sky view, no records
yet). Do not weaken an assertion to make it pass on a bench that lacks something.

## Three device behaviours the tests are built around

**Nothing on the result path can correlate a trigger, so the firmware emits a token on
the trigger path.** `GNSS fix cached` and `Scan cached` are emitted by a zbus listener on
`location_chan` that fires for *any* location result — including the application's own
startup sample and its periodic timer. The `gnss #N, scan #M` counters in `survey show`
are no better: they are incremented in `survey_obs_update()`, called from that same
callback, so reading them changes the transport and not the correlation. A test built on
either passes with the trigger deleted from the firmware, because a background search
finishing inside the window is indistinguishable from ours.

Absence of the "trigger received while a search is active, ignoring" warning does not
close it either — that warning is equally absent when no trigger ever arrived.

So the location module counts what it *accepts*, what it *discards*, and what
`APP_SURVEY_CAPTURE_OWNS_SEARCH` *suppresses*, and `survey show` prints
`Triggers: accepted N, dropped M, suppressed K`
(`app/src/modules/location/location_trigger_stats.h`). `trigger()` requires `accepted` to
advance **and** the matching observation counter to advance. `dropped` replaces waiting
for the warning, which Zephyr's deferred logging can deliver late.

The third counter is what makes the second usable. `trigger()` treats a rise in `dropped`
as "our trigger was rejected, retry"; if the application's own suppressed sampling landed
there, the harness would give up on a trigger that was still pending. It is also the only
evidence from outside that the suppression exists — delete the branch and every other test
still passes, which is why `owns_search_suppresses_the_application_sample` asserts on it
directly after a cold boot.

`self_trigger_notices_a_dead_trigger` keeps this from rotting. The fake shell impersonates
the exact regression: observation counters that climb on every look, `accepted` frozen at
zero. A harness that checks only "did a result appear" passes it, so if it ever goes green
for the wrong reason, check that the fake's counters still advance.

**A location trigger issued while a search is running is discarded.** The shell still
reports success — only `<wrn> location_module: Location trigger received while a search
is active, ignoring` says otherwise. This build has a second driver: the asset tracker
takes one GNSS-first sample at startup that can own the library for up to the 600 s GNSS
timeout. `trigger()` waits it out and retries; the reboot tests run last so they cannot
starve the radio tests. CP6 removes the second driver.

**`att_storage stats` is a request, not a query.** The shell prints "Storage statistics
request initiated." immediately and the counts arrive later, as log lines from the
storage thread. That thread is unavailable while busy: measured silent for about 45 s
after a cold boot at 5,004 records, because it counts them at mount, and for the duration
of a store (up to 43.9 s — see the FW-6 section of `REQUIREMENTS.md`). `_survey_count()`
asks again rather than concluding anything from one silent window. It is not a way to
*time* a store; `docs/common/dev_workflow.md` explains why polling distorts that.
