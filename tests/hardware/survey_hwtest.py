#!/usr/bin/env python3
#
# Copyright (c) 2026 Nordic Semiconductor ASA
#
# SPDX-License-Identifier: LicenseRef-Nordic-5-Clause

"""Integration tests that run against a real Thingy:91 X over the serial console.

The unit suites under tests/module prove logic on native_sim with the modem, the
filesystem and the radios faked. Everything those fakes stand in for -- that the
device boots, that a command exists in the build that was actually flashed, that a
scan returns APs, that records survive a power cycle -- is unproven until something
drives the hardware. This is that something.

Each test sends real shell commands and asserts on what the device says back. There
is no mocking here by design: a test that passes on this harness passed on silicon.

    # Nothing here writes to flash or resets the device. It does overwrite the cached
    # observation, so it is not invisible to a bench session in progress.
    python3 tests/hardware/survey_hwtest.py

    # Include the tests that need a network registration or a sky view.
    python3 tests/hardware/survey_hwtest.py --radio

    # Include the tests that append records and cold-boot the device. Asks first.
    python3 tests/hardware/survey_hwtest.py --destructive

    # One test, with the raw serial traffic shown. --only filters the selection, it does
    # not widen it, so a destructive or radio test still needs its flag.
    python3 tests/hardware/survey_hwtest.py --only selftest_renders -v
    python3 tests/hardware/survey_hwtest.py --destructive --yes --only survives_reboot

    # Prove the harness itself against a fake shell, no hardware needed.
    python3 tests/hardware/survey_hwtest.py --loopback

Exit status: 0 when at least one test passed and none failed, 1 on a failure, 2 on a
usage or serial problem, 3 when everything selected skipped -- because an all-skip run
is the shape an indoor bench produces, and reporting it as success would claim coverage
that did not happen.

pyserial is required; run under the toolchain if it is not on your path:

    nrfutil toolchain-manager launch --ncs-version v3.4.0 -- \\
        python3 tests/hardware/survey_hwtest.py
"""

import argparse
import re
import sys
import time
from pathlib import Path
from typing import Callable, NamedTuple, Optional, Sequence, TypedDict

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "scripts"))

import survey_console as console  # noqa: E402  (path set up above)

try:
    import serial
except ImportError:
    sys.exit("pyserial is required -- see the module docstring.")


PROMPT = "uart:~$"

# Tags a test can declare. The runner selects on these so that a default run is safe
# to point at a device mid-collection.
TAG_RADIO = "radio"            # needs network registration and/or a sky view
TAG_DESTRUCTIVE = "destructive"  # erases stored records
TAG_SLOW = "slow"              # takes more than ~30 s
TAG_SELF = "self"              # exercises the harness, not the device (--loopback only)


class TestCase(TypedDict):
    """One registered test. The runner reads these fields; nothing else does."""

    name: str
    fn: "TestFn"
    tags: set[str]
    doc: str


# Every test takes the open console and returns nothing; a verdict is an exception.
TestFn = Callable[["Device"], None]

_TESTS: list[TestCase] = []


class Failure(AssertionError):
    """Raised by the check helpers. Carries the device output for the report."""


class Skip(Exception):
    """Raised when a precondition the test cannot create is missing."""


def test(name: str, tags: Sequence[str] = ()) -> Callable[[TestFn], TestFn]:
    """Register a test function. The docstring is shown in --list."""
    def wrap(fn: TestFn) -> TestFn:
        _TESTS.append({"name": name, "fn": fn, "tags": set(tags),
                       "doc": (fn.__doc__ or "").strip().split("\n")[0]})
        return fn
    return wrap


# --- Device -------------------------------------------------------------------------

class Device:
    """One open console, with the waiting primitives the tests need."""

    def __init__(self, ser: serial.Serial, verbose: bool = False) -> None:
        self.ser = ser
        self.verbose = verbose

    def cmd(self, command: str, timeout: float = 5.0, quiet_for: float = 0.4) -> str:
        """Send a command and return its reply with echo and prompt removed."""
        reply, reason = console.send_command(self.ser, command, timeout, quiet_for, PROMPT)
        if self.verbose:
            print(f"    $ {command}")
            for line in reply.split("\n"):
                print(f"    | {line}")
            print(f"    ({reason})")
        return reply

    def await_line(self, pattern: str, timeout: float, command: Optional[str] = None,
                   settle: float = 0.2) -> str:
        """Send `command` (if any) then read until `pattern` appears.

        Returns the whole captured text. Unlike cmd(), this keeps reading past the
        prompt, because the interesting output of an asynchronous command arrives
        after the shell has already returned.

        Match the whole line you intend to parse, not a prefix of it: the match can
        land on a read boundary, and reading stops one settle interval later, so a
        pattern that stops short can return a truncated line.
        """
        rx = re.compile(pattern)
        if command is not None:
            self.ser.reset_input_buffer()
            console.drain(self.ser)
            self.ser.write((command + "\r").encode())
            self.ser.flush()
        deadline = time.monotonic() + timeout
        raw = ""
        buf = ""
        while time.monotonic() < deadline:
            chunk = self.ser.read(max(1, self.ser.in_waiting))
            if chunk:
                # Accumulate raw and re-strip the whole thing: an escape sequence, or a
                # multi-byte UTF-8 character, split across two reads survives per-chunk
                # stripping as literal text. survey_console.read_until_idle() does the
                # same, for the same reason.
                raw += chunk.decode("utf-8", "replace")
                buf = console.strip_ansi(raw)
                if rx.search(buf):
                    # Let the rest of the line arrive before returning it to be parsed.
                    time.sleep(settle)
                    tail = self.ser.read(max(1, self.ser.in_waiting))
                    if tail:
                        buf = console.strip_ansi(raw + tail.decode("utf-8", "replace"))
                    break
        if self.verbose:
            head = f"    $ {command}" if command else f"    (await {pattern!r})"
            print(head)
            for line in buf.split("\n"):
                if line.strip():
                    print(f"    | {line.strip()}")
        return buf

    def wake(self, settle: float = 1.0, max_wait: float = 20.0) -> None:
        """Bring the console to a clean prompt, waiting out any boot chatter first.

        Flashing resets the device, so a suite started straight afterwards runs its
        first test while the banner is still arriving. The reply to "survey" comes back
        interleaved with littlefs, wifi_nrf_bus and Memfault lines, and the test fails
        on a build that is perfectly correct -- a false red that looks exactly like the
        wrong image or the wrong port, which is what its failure message will claim.

        console.drain() does not cover this: its quiet threshold is 0.15 s and its
        ceiling 1.5 s, while boot output has gaps longer than the former and runs past
        the latter, so it returns in the middle of the banner. Those constants are right
        for draining between commands, which is what the interactive console wants, so
        the longer wait lives here rather than in the shared helper.

        A trailing prompt ends the wait immediately: boot output finishes with one, so
        the settle timer is only actually needed for the case where the device says
        nothing at all. It is still the authority -- the prompt can arrive mid-banner
        from a shell that came up before the last driver logged.
        """
        deadline = time.monotonic() + max_wait
        last_rx = time.monotonic()
        tail = b""

        while time.monotonic() < deadline:
            chunk = self.ser.read(max(1, self.ser.in_waiting))
            if chunk:
                last_rx = time.monotonic()
                # A window, not exactly len(PROMPT): the device emits a trailing space
                # after the prompt, so an exact-width tail never matches it.
                tail = (tail + chunk)[-32:]
                continue
            if (time.monotonic() - last_rx) >= settle or tail.rstrip().endswith(
                PROMPT.encode()
            ):
                break

        console.wake_shell(self.ser, PROMPT)


# --- Checks -------------------------------------------------------------------------

def want(condition: object, message: str, context: str = "") -> None:
    if not condition:
        raise Failure(message + (f"\n--- device said ---\n{context}" if context else ""))


def want_in(needle: str, haystack: str, message: Optional[str] = None) -> None:
    want(needle in haystack, message or f"expected {needle!r} in output", haystack)


def want_match(pattern: str, text: str, message: Optional[str] = None) -> "re.Match[str]":
    # re.M by default: device output is line-oriented, and anchoring on a line start is
    # how a pattern is kept from matching help text or a summary line elsewhere.
    m = re.search(pattern, text, re.M)
    want(m is not None, message or f"expected /{pattern}/ to match", text)
    # want() has already raised if the match failed; this is what makes the non-optional
    # return type above true rather than merely usually true.
    #
    # An assert rather than a raise, unlike survey_decode.py's _want_int(): there the assert
    # was the only check and `python -O` would have dropped it, turning a malformed record
    # into a confusing TypeError. Here it is narrowing on top of a check that has already
    # run, so under -O this line's absence changes nothing.
    assert m is not None
    return m


def want_int(pattern: str, text: str, message: Optional[str] = None) -> int:
    """Match a pattern with one capturing group and return it as an int."""
    return int(want_match(pattern, text, message).group(1))


# --- Tests: the build is what we think it is ----------------------------------------

@test("shell_responds")
def t_shell_responds(dev: "Device") -> None:
    """The console answers at all, and it is the survey build."""
    reply = dev.cmd("survey")
    want_in("Radio survey commands", reply, "no survey command group -- wrong build or wrong port")


@test("survey_subcommands_present")
def t_subcommands(dev: "Device") -> None:
    """Every subcommand this checkpoint relies on is in the flashed image."""
    reply = dev.cmd("survey")
    for sub in ("scan", "gnss", "show", "stats", "clear", "selftest", "hex", "store",
                "capture", "timing"):
        # Anchored on the name column, not a bare substring: several of these words also
        # occur in each other's help text, so "scan" alone passes with the subcommand
        # deleted (show's help is "Print the cached GNSS fix and radio scan").
        want_match(rf"^\s*{sub}\s+:", reply,
                   f"subcommand {sub!r} missing from the flashed build")


@test("storage_shell_present")
def t_storage_shell(dev: "Device") -> None:
    """The upstream storage command group is enabled, which the survey build needs."""
    reply = dev.cmd("att_storage stats", timeout=8.0)
    want_in("initiated", reply, "att_storage stats did not acknowledge")


# --- Tests: the selftest path, which needs no radio ---------------------------------

@test("selftest_renders")
def t_selftest(dev: "Device") -> None:
    """selftest injects a synthetic observation and the formatter renders it."""
    dev.cmd("survey clear")
    reply = dev.cmd("survey selftest", timeout=8.0)
    want_in("Injecting synthetic", reply)
    # The rendered lines, not bare field names: the command's own trailing "Expected:
    # lat ..., eci ..." summary contains those words whether or not the formatter ran.
    want_match(r"lat -?\d+\.\d+ lon -?\d+\.\d+ acc \d+\.\d+ m", reply,
               "no rendered GNSS line -- the formatter did not run")
    want_match(r"^\s+eci \d+", reply, "no rendered serving cell")
    want_match(r"^\s+wifi\.accessPoints\[\] \d+:", reply, "no rendered AP list")


@test("selftest_marks_synthetic")
def t_selftest_synthetic(dev: "Device") -> None:
    """Injected data is banner-marked so it cannot be mistaken for a measurement."""
    dev.cmd("survey selftest", timeout=8.0)
    reply = dev.cmd("survey show", timeout=8.0)
    want_match(r"(?i)synthetic", reply,
               "no synthetic banner -- injected data is indistinguishable from real")


@test("wifi_channel_and_band_rendered")
def t_wifi_fields(dev: "Device") -> None:
    """CP2's channel/band/frequency derivation reaches the console."""
    dev.cmd("survey selftest", timeout=8.0)
    reply = dev.cmd("survey show", timeout=8.0)
    want_match(r"channel\s+\d+", reply, "no Wi-Fi channel rendered")
    want_match(r"frequency\s+\d+\s*MHz", reply, "no derived frequency rendered")


@test("clear_empties_the_cache")
def t_clear(dev: "Device") -> None:
    """clear discards the cached observation rather than leaving stale data."""
    dev.cmd("survey selftest", timeout=8.0)
    dev.cmd("survey clear")
    reply = dev.cmd("survey show", timeout=8.0)
    want_match(r"GNSS: no fix cached", reply,
               "show still rendered a GNSS fix after clear")
    want_match(r"Scan: none cached", reply,
               "show still rendered a scan after clear")


@test("hex_encodes_the_cached_observation")
def t_hex(dev: "Device") -> None:
    """CP3's encoder runs on target and emits a plausible CBOR dump."""
    dev.cmd("survey selftest", timeout=8.0)
    reply = dev.cmd("survey hex", timeout=10.0)
    body = want_match(r"BEGIN SURVEY RECORD-----([\s\S]*?)-----END SURVEY RECORD",
                      reply, "no delimited record block in the hex dump").group(1)
    # The dump wraps at 64 characters with no separators, so count characters rather
    # than \b-delimited byte tokens -- there are no word boundaries inside a run.
    digits = re.sub(r"[^0-9a-fA-F]", "", body)
    claimed = want_int(r"survey record: (\d+) bytes", reply,
                       "hex dump did not state a record length")
    want(len(digits) == 2 * claimed,
         f"dump holds {len(digits) // 2} bytes but claims {claimed}", reply)
    want(claimed > 40, f"record is implausibly short at {claimed} bytes", reply)


# --- Tests: storage ------------------------------------------------------------------

def _try_survey_count(dev: "Device", timeout: float = 20.0) -> Optional[int]:
    """Ask for the record count once. Returns None if the storage thread did not answer.

    "att_storage stats" is a request, not a query: the shell acknowledges immediately
    and the counts arrive later as log lines from the storage thread. That thread is
    unavailable while it is busy -- measured silent for ~45 s after a cold boot with
    5,004 records, because it counts them at mount, and equally silent for the duration
    of a store. A silent window is therefore normal and is not a failure here.
    """
    reply = dev.await_line(r"SURVEY: \d+ records", timeout, command="att_storage stats")
    m = re.search(r"SURVEY: (\d+) records", reply)
    return int(m.group(1)) if m else None


def _survey_count(dev: "Device", timeout: float = 120.0) -> int:
    """Ask until the storage thread answers, or fail.

    Asking repeatedly is what dev_workflow.md warns against for *latency measurement*,
    where the request competes with the thread being measured and distorts the number.
    Here the number wanted is the count itself, which the ask does not perturb, and the
    asks are seconds apart rather than continuous. Do not reuse this to time a store.
    """
    deadline = time.monotonic() + timeout
    while True:
        remaining = deadline - time.monotonic()
        if remaining <= 0:
            raise Failure(f"storage never reported a SURVEY count within {timeout:.0f} s")
        count = _try_survey_count(dev, timeout=min(20.0, remaining))
        if count is not None:
            return count


@test("storage_reports_a_count", tags=(TAG_SLOW,))
def t_storage_count(dev: "Device") -> None:
    """The storage module answers with a SURVEY record count."""
    # The assertion is that _survey_count() returned at all: it raises if the storage
    # thread never answered. The value cannot be negative -- the pattern captures \d+.
    count = _survey_count(dev)
    print(f"[{count} records] ", end="", flush=True)


@test("store_increments_the_count", tags=(TAG_SLOW, TAG_DESTRUCTIVE))
def t_store_increments(dev: "Device") -> None:
    """A stored record actually lands on flash, not just in the publish path.

    Destructive: it appends a synthetic record to the survey partition permanently.
    Records cannot be deleted individually, so this leaves one non-measurement in any
    capture the device is holding.
    """
    before = _survey_count(dev)
    dev.cmd("survey selftest", timeout=8.0)
    reply = dev.cmd("survey store", timeout=10.0)
    want_in("handed to storage", reply)
    # The write completes on the storage thread after the shell returns, and a store
    # can stall for tens of seconds on metadata compaction -- see the FW-6 section of
    # REQUIREMENTS.md. Poll rather than sleeping a fixed amount.
    deadline = time.monotonic() + 120
    after = before
    while time.monotonic() < deadline:
        polled = _try_survey_count(dev)
        if polled is not None and polled > before:
            after = polled
            break
    want(after == before + 1,
         f"record count went {before} -> {after}, expected {before + 1}")


# --- Tests: the radio path ------------------------------------------------------------

class NoCounters(Failure):
    """`survey show` did not print the lines the counters are read from.

    Distinct from Failure so that a caller which converts a missing result into a Skip
    cannot also swallow a broken or missing command. That is a real failure at any bench.
    """


class Counters(NamedTuple):
    accepted: int
    dropped: int
    suppressed: int
    gnss: int
    scan: int


def counters(dev: "Device") -> Counters:
    """Return the trigger and observation counters from "survey show".

    Two different things, and the difference is the whole point.

    `gnss_count` and `scan_count` are the observation cache's counters. They advance when
    a *result* is cached -- and results are indistinguishable by origin. They are
    incremented in survey_obs_update(), called from the same zbus listener callback that
    prints "GNSS fix cached", so reading them instead of scraping that log line changes
    the transport and nothing else. Neither can tell our trigger from the application's
    own periodic sample.

    `accepted`, `dropped` and `suppressed` come from the location module, which
    increments `accepted` where it starts a request, `dropped` where it discards a
    trigger that arrived during a search, and `suppressed` where
    APP_SURVEY_CAPTURE_OWNS_SEARCH silences the application's own sampling timer. That is
    a positive token emitted on the *trigger* path, so requiring `accepted` to advance is
    what actually correlates a result with the command that asked for it. Delete the
    publish from cmd_survey_scan and `accepted` stops moving, whatever the radios are
    doing. See app/src/modules/location/location_trigger_stats.h.

    `suppressed` being its own counter is what makes `dropped` usable as "our trigger
    lost the race". While OWNS_SEARCH is set, the application's periodic triggers are
    suppressed before they can be dropped, so on this build every increment of `dropped`
    belongs to a trigger somebody here asked for.

    "survey clear" resets the observation counters. It does not reset the three trigger
    counters, which are per-boot.
    """
    reply = dev.cmd("survey show", timeout=8.0)
    obs = re.search(r"Survey observation \(gnss #(\d+), scan #(\d+)\)", reply, re.M)
    trig = re.search(
        r"Triggers: accepted (\d+), dropped (\d+), suppressed (\d+)", reply, re.M)
    if obs is None or trig is None:
        raise NoCounters(
            "survey show did not print both its counter lines -- is this the survey "
            "build, and does it include the trigger stats?"
            + f"\n--- device said ---\n{reply}")
    return Counters(int(trig.group(1)), int(trig.group(2)), int(trig.group(3)),
                    int(obs.group(1)), int(obs.group(2)))


def trigger(dev: "Device", command: str, which: str, timeout: float,
            attempts: int = 3) -> str:
    """Issue a location trigger and wait for *its* result. `which` is "gnss" or "scan".

    Two conditions have to hold, and only together do they mean anything:

      1. the location module's `accepted` counter advanced -- our trigger reached it and
         started a request, rather than never being sent;
      2. the matching observation counter advanced -- that request produced a result.

    Condition 2 alone is what an earlier version of this helper checked, and it passes on
    firmware with the trigger deleted: the application samples on its own, and a
    background result landing inside the window is identical from out here.

    Retrying is not defensive padding. A trigger that arrives while a search is already
    running is discarded, and the shell reports success either way. That is common on
    this build: the asset tracker takes one GNSS-first sample at startup which can own
    the Location library for as long as its GNSS timeout allows (600 s). The `dropped`
    counter says so directly, which is more reliable than waiting for a log line --
    Zephyr's logging is deferred, so the warning can arrive well after the event.

    Reading `dropped` as "our trigger was rejected" is only sound because `suppressed`
    exists: the application's own periodic triggers are silenced by OWNS_SEARCH rather
    than dropped, so they cannot make this helper give up on a trigger that is still
    pending. It is sound for the other direction too -- a capture cycle refuses shell
    triggers outright, and that refusal is visible in the reply, not in `dropped`.

    Returns the log text captured while waiting, for the caller to assert on.
    """
    for attempt in range(attempts):
        start = counters(dev)
        before = start.gnss if which == "gnss" else start.scan
        text = dev.await_line(r"ignoring", 3.0, command=command)
        if "capture cycle is running" in text.lower():
            # Not a race we can win by retrying: the orchestrator holds the radio for the
            # whole cycle and refuses the command outright.
            raise Skip(f"{command!r} was refused: a capture cycle owns the radio")
        deadline = time.monotonic() + timeout
        dropped = False
        while time.monotonic() < deadline:
            text += dev.await_line(r"cached|search done|timeout|error", 5.0)
            now = counters(dev)
            if now.dropped > start.dropped:
                # Rejected outright. Nothing will arrive; go round again rather than
                # spending the rest of the window waiting for it.
                dropped = True
                break
            if now.accepted > start.accepted and (
                    (now.gnss if which == "gnss" else now.scan) > before):
                return text
        if dropped:
            continue
        if counters(dev).accepted == start.accepted:
            raise Failure(
                f"{command!r} reported success but the location module never accepted a "
                f"trigger (accepted stayed at {start.accepted}) -- the command is not "
                "reaching the module"
                + (f"\n--- device said ---\n{text}" if text.strip() else ""))
        raise Failure(
            f"{command!r} was accepted but no {which} result arrived in {timeout:.0f} s"
            + (f"\n--- device said ---\n{text}" if text.strip() else ""))
    raise Skip(f"{command!r} was dropped {attempts} times -- another search kept the "
               "Location library busy for the whole window")


@test("scan_returns_observations", tags=(TAG_RADIO, TAG_SLOW))
def t_scan(dev: "Device") -> None:
    """A real scan yields cells and/or APs -- CP1's scan-only trigger works."""
    dev.cmd("survey clear")
    trigger(dev, "survey scan", "scan", 60.0)
    # Assert on the cache rather than the log line: this is the scan the counter says
    # is ours, whereas a log line could belong to any search.
    show = dev.cmd("survey show", timeout=8.0)
    want("Scan: none cached" not in show,
         "the scan counter advanced but no scan is cached", show)
    # Counts, not the presence of the header lines that announce them. "nmr[] %u
    # neighbor(s):" and "wifi.accessPoints[] %u:" are printed whenever a scan is cached,
    # including an empty one, so matching the line proves nothing; the number does. Same
    # for the serving cell: "eci" appears in "eci absent (cell not identified)" too.
    neighbors = int(want_match(r"^\s+nmr\[\] (\d+) neighbor", show,
                               "no neighbour-cell line in the cached scan").group(1))
    aps = int(want_match(r"^\s+wifi\.accessPoints\[\] (\d+)", show,
                         "no access-point line in the cached scan").group(1))
    served = re.search(r"^\s+eci (\d+)", show, re.M) is not None
    want(neighbors > 0 or aps > 0 or served,
         f"scan completed but returned nothing: {neighbors} neighbours, {aps} APs, "
         "no serving cell -- no network and nothing in range?", show)


@test("scan_drops_locally_administered_bssids", tags=(TAG_RADIO, TAG_SLOW))
def t_scan_drops_local_macs(dev: "Device") -> None:
    """No randomised BSSID reaches the cache from a real scan.

    Asserted on what survived rather than on what was dropped, because how many phone
    hotspots are in range is a property of the room and not of the firmware -- an
    assertion that something *was* dropped would pass in an office and fail in a lab at
    midnight. The invariant that holds everywhere is that nothing with the U/L bit set is
    left behind.

    Skipped on a build with the filter off, which the Kconfig help explicitly invites for
    bench comparison: asserting it there reports a firmware defect against a configuration
    working exactly as designed. The device states which way it was built on the caps line.
    """
    stats = dev.cmd("survey stats", timeout=8.0)
    if "drop_local_mac y" not in stats:
        want_in("drop_local_mac", stats,
                "the device does not report its BSSID filter setting, so this test "
                "cannot tell a working filter from one that is compiled out")
        raise Skip("built with CONFIG_APP_SURVEY_DROP_LOCAL_MAC=n")

    dev.cmd("survey clear")
    scan_log = trigger(dev, "survey scan", "scan", 60.0)
    show = dev.cmd("survey show", timeout=8.0)

    macs = re.findall(r"macAddress ([0-9A-F]{2}):", show)
    want(bool(macs) or "wifi.accessPoints[] 0" in show,
         "neither access points nor an empty AP list in the cached scan", show)

    local = [m for m in macs if int(m, 16) & 0x02]
    want(not local,
         f"{len(local)} of {len(macs)} cached BSSID(s) are locally administered "
         f"(first octets {', '.join(local)}) -- the filter did not run", show)

    # The dropped line is conditional on something having been dropped, so its absence is
    # not a failure. When it is there, it has to agree with itself: a zero would mean the
    # render condition and the counter disagree.
    dropped = re.search(r"(\d+) locally-administered BSSID\(s\) dropped", show)
    if dropped is not None:
        want(int(dropped.group(1)) > 0,
             "the dropped-BSSID line was rendered with a count of zero", show)

    # The "Scan cached:" log line has no unit test behind it -- survey.c is not built by
    # any native_sim suite -- so this is the only thing standing between it and a silent
    # regression. Both of its counts are checked against the rendered cache, which catches
    # the format arguments being swapped, the subtraction being dropped, and the count
    # being read back from a cache some other command has since changed.
    #
    # The comparison is only meaningful if the log line and the render describe the SAME
    # scan, and on a build with the capture orchestrator running they need not: it samples
    # on its own cadence, so an older cycle's line can be sitting in trigger()'s buffer
    # before "survey scan" is even sent, and a newer cycle can land between trigger()
    # returning and "survey show" being answered. Either way the counts disagree because
    # the room changed, not because the firmware is wrong -- an intermittent red that
    # accuses the filter.
    #
    # "survey clear" above zeroes the counters and "survey show" prints the serial of what
    # it rendered, so "scan #1" is an exact statement that one scan has happened since the
    # clear and this is it. Anything else means a cycle intervened; the U/L assertions
    # above still stand on their own, so the cross-check is skipped rather than failed.
    serial = re.search(r"scan #(\d+)\)", show)
    want(serial is not None, "no scan serial in the survey show header", show)
    if serial.group(1) != "1":
        print(f"    [scan #{serial.group(1)} since clear -- an autonomous cycle "
              f"intervened, skipping the log/render cross-check]")
        return

    matches = re.findall(r"Scan cached: \d+ neighbor\(s\), \d+ gci, (\d+) AP\(s\) "
                         r"\((\d+) randomised BSSID\(s\) dropped\)", scan_log)
    want(bool(matches), "no 'Scan cached' line with an AP and dropped count", scan_log)
    logged = matches[-1]
    want(int(logged[0]) == len(macs),
         f"the log announced {logged[0]} AP(s) but the cache holds {len(macs)}",
         scan_log + "\n---\n" + show)
    want(int(logged[1]) == (int(dropped.group(1)) if dropped else 0),
         f"the log announced {logged[1]} dropped but the cache reports "
         f"{dropped.group(1) if dropped else 0}", scan_log + "\n---\n" + show)


@test("scan_repeats_without_ebusy", tags=(TAG_RADIO, TAG_SLOW))
def t_scan_repeats(dev: "Device") -> None:
    """Back-to-back scans do not wedge the Location library.

    This is the -EBUSY regression: cancelling a request leaves the Wi-Fi driver
    running, and the next request fails. CP1 answers scan-only requests with
    LOCATION_EXT_RESULT_UNKNOWN instead of cancelling, and this proves it holds.
    """
    for attempt in range(3):
        text = trigger(dev, "survey scan", "scan", 60.0)
        want("EBUSY" not in text and "-16" not in text,
             f"scan {attempt + 1} of 3 returned EBUSY", text)


@test("gnss_fix", tags=(TAG_RADIO, TAG_SLOW))
def t_gnss(dev: "Device") -> None:
    """A GNSS-only request returns a fix. Needs a sky view; skipped indoors."""
    try:
        trigger(dev, "survey gnss", "gnss", 180.0)
    except NoCounters:
        # Not "no sky view": the command or its output is broken, which fails at any
        # bench. Blanket-converting every Failure here would hide that behind a Skip,
        # and a --radio run of nothing but skips still exits 0 when it is the only test
        # that skipped.
        raise
    except Failure as exc:
        want("never accepted a trigger" not in str(exc),
             f"the GNSS trigger is not reaching the location module: {exc}")
        raise Skip("no GNSS fix in 180 s -- indoors, or no antenna")
    show = dev.cmd("survey show", timeout=8.0)
    want("GNSS: no fix cached" not in show,
         "the GNSS counter advanced but no fix is cached", show)
    want_match(r"lat -?\d+\.\d+ lon -?\d+\.\d+", show, "no coordinates in the cached fix")


# --- Tests: the capture orchestrator (CP6) ----------------------------------------------

# Slack added to whatever worst case the firmware reports, to cover the store -- which is
# not part of the cycle's radio budget and has been measured at tens of seconds on a full
# partition -- plus the serial round trips. Patience that is too short does not find a bug;
# it reports a device that is merely indoors as a failure.
CAPTURE_STORE_SLACK_SECONDS = 90.0

# How long a second, unwanted record gets to show up before "exactly one" is believed. A
# store on a full partition has been measured at tens of seconds, so a short settle would
# make this assertion pass for the wrong reason.
SECOND_RECORD_SETTLE_SECONDS = 60.0


def run_capture(dev: "Device") -> tuple[str, int]:
    """Run one cycle and return (log text, accepted-trigger delta).

    The delta is the correlation, and here it is stronger than for a single trigger: a
    cycle is three separate location requests, so `accepted` must advance by exactly
    three. Two would mean a step was dropped and the cycle silently ran short -- which is
    the failure this whole module is built to prevent, and it is invisible in the record.

    How long to wait is asked of the device rather than hardcoded. "survey capture" prints
    SURVEY_CAPTURE_WORST_CASE_SECONDS, which is derived from the same Kconfig timeouts the
    orchestrator obeys, so raising a step timeout cannot leave this harness aborting cycles
    that were still within their budget.
    """
    accepted_before = counters(dev).accepted
    text = dev.await_line(r"Expect up to (\d+) s|already running", 15.0,
                          command="survey capture")
    if "already running" in text:
        raise Skip("a capture was already running")
    m = re.search(r"Expect up to (\d+) s", text)
    want(m is not None,
         "\"survey capture\" did not report its worst-case duration -- without it there "
         "is no honest timeout to wait for", text)
    timeout = int(m.group(1)) + CAPTURE_STORE_SLACK_SECONDS

    text += dev.await_line(r"Cycle \d+ stored|Cycle \d+ produced nothing", timeout)
    want(re.search(r"Cycle \d+ (stored|produced nothing)", text) is not None,
         f"no cycle outcome within {timeout:.0f} s -- the orchestrator did not finish "
         "inside its own worst case", text)
    accepted_after = counters(dev).accepted

    return text, accepted_after - accepted_before


@test("capture_runs_gnss_scan_gnss_in_order", tags=(TAG_RADIO, TAG_SLOW, TAG_DESTRUCTIVE))
def t_capture_order(dev: "Device") -> None:
    """One cycle issues three sequenced requests and stores the result.

    FW-2's sequencing, proved from outside: the modem has one RF front end, so the steps
    cannot overlap, and the Location library stops at the first method that succeeds, so
    a single combined request would return a fix and never scan.
    """
    text, accepted = run_capture(dev)
    want("produced nothing" not in text, "the cycle produced no data at all", text)

    # The sequencing claim, and it holds indoors: three separate requests were issued and
    # the location module accepted all three. This is the assertion that fails if a step
    # is dropped, and it does not depend on any of them succeeding.
    want(accepted == 3,
         f"the cycle had {accepted} triggers accepted, expected 3 (GNSS, scan, GNSS) -- "
         "a step was dropped and the cycle ran short", text)

    # Order, from the module's own log. Anchored on the whole line so a truncated read
    # cannot match a prefix, and taken as a sequence rather than a set.
    steps = re.findall(r"(GNSS fix cached|Scan cached)", text)

    # Indoors both GNSS steps spend their timeout and cache nothing, so there is no
    # bracket to check the order of -- the same condition that makes gnss_fix skip. Skip
    # rather than fail, but only after `accepted == 3` above has been required: the part
    # of the claim that does not need a sky view is still enforced on every run.
    if steps.count("GNSS fix cached") < 2:
        want("Scan cached" in steps,
             f"the cycle cached nothing at all: {steps}", text)
        raise Skip(f"only {steps.count('GNSS fix cached')} of 2 GNSS steps produced a "
                   "fix -- indoors, so there is no bracket to order")

    want(steps == ["GNSS fix cached", "Scan cached", "GNSS fix cached"],
         f"steps arrived as {steps}, expected GNSS, scan, GNSS", text)


@test("capture_timing_reports_real_durations", tags=(TAG_RADIO, TAG_SLOW))
def t_capture_timing(dev: "Device") -> None:
    """"survey timing" reports measured durations, not zeros or placeholders.

    This is what FW-7's cadence has to be set from. A cycle that reported zeros would
    look like a fast device rather than a broken measurement.
    """
    reply = dev.cmd("survey timing", timeout=6.0)
    if "No capture has completed" in reply:
        raise Skip("no cycle has completed yet; run capture_runs_gnss_scan_gnss_in_order")
    # \s+ throughout: the report is column-aligned, so the gap after each label is padding
    # whose width follows the longest label and changes whenever one is renamed.
    before = want_int(r"gnss_before\s+(-?\d+) ms", reply, "no gnss_before in survey timing")
    scan = want_int(r"scan\s+(-?\d+) ms", reply, "no scan duration in survey timing")
    after = want_int(r"gnss_after\s+(-?\d+) ms", reply, "no gnss_after in survey timing")
    store = want_int(r"store\s+(-?\d+) ms", reply, "no store duration in survey timing")
    total = want_int(r"total\s+(-?\d+) ms", reply, "no total in survey timing")
    # Asked of the device for the same reason the capture timeout is: this bound is
    # arithmetic over the Kconfig timeouts, and an earlier version of it was recomputed
    # here in Python, where changing a timeout would have silently invalidated it.
    budget = want_int(r"wind_down budget\s+(-?\d+) ms", reply,
                      "no wind-down budget in survey timing -- without it the slack "
                      "check would have to reproduce the firmware's timeout arithmetic")

    want(scan > 0, f"the scan step reported {scan} ms; a real scan takes seconds", reply)
    want(total > 0, f"the cycle reported a total of {total} ms", reply)
    # Each step is measured against its own boundaries now, so a zero means a step never
    # ran -- the trigger never reached the library -- rather than a fast one. Even a step
    # that finds nothing spends its timeout.
    for name, value in (("gnss_before", before), ("scan", scan), ("gnss_after", after)):
        want(value > 0, f"{name} reported {value} ms; a step that ran cannot take no "
                        "time, so its trigger never reached the location module", reply)
    want(store >= 0, f"store reported {store} ms", reply)

    # Not equality. The four numbers are measured independently, and the cycle also spends
    # time between them waiting for each search to report done -- real elapsed time that
    # belongs to no step. So the parts must fit inside the whole, and the slack must be
    # small enough to be wind-down rather than an unaccounted step.
    #
    # An earlier version asserted the parts summed to the total within a second. That could
    # not fail: gnss_after was *defined* as the remainder, so the identity held even when
    # the second GNSS step had not run at all.
    parts = before + scan + after + store
    want(parts <= total,
         f"steps and store sum to {parts} ms but the cycle reports {total} ms -- the "
         "parts cannot exceed the whole unless they are measured on different clocks",
         reply)
    want(total - parts <= budget,
         f"{total - parts} ms of the cycle belongs to no step; the gaps should only be "
         "the three waits for a search to wind down", reply)


@test("capture_stores_one_record", tags=(TAG_RADIO, TAG_SLOW, TAG_DESTRUCTIVE))
def t_capture_stores(dev: "Device") -> None:
    """A completed cycle appends exactly one record.

    One, not two: the cycle's two fixes and its scan are a single record, which is what
    lets the host interpolate between the brackets. Two records would mean the
    observation path is also storing, and the pairing would be lost.
    """
    before = _survey_count(dev)
    run_capture(dev)
    deadline = time.monotonic() + 120
    after = before
    while time.monotonic() < deadline:
        polled = _try_survey_count(dev)
        if polled is not None and polled > before:
            after = polled
            break
    want(after > before,
         f"record count stayed at {before}; the cycle stored nothing within 120 s")

    # Breaking out of the poll on the first increase proves "at least one", which is the
    # weaker half of the claim -- a second record written a moment later is exactly the
    # bug this test names, and the loop above would never see it. Storage is asynchronous,
    # so give the second write time to appear and then look again.
    time.sleep(SECOND_RECORD_SETTLE_SECONDS)
    settled = _survey_count(dev)
    want(settled == before + 1,
         f"record count went {before} -> {settled} after settling, expected exactly one "
         "new record -- the observation path is storing alongside the cycle and the "
         "bracketing is lost")


@test("capture_refuses_to_overlap", tags=(TAG_RADIO, TAG_SLOW, TAG_DESTRUCTIVE))
def t_capture_no_overlap(dev: "Device") -> None:
    """A second "survey capture" during a cycle is refused, not queued.

    Overlapping cycles cannot work: the second one's triggers are discarded while the
    first still holds the library, so its record would carry the first cycle's radio work
    under its own timestamps -- wrong data that looks entirely well-formed.
    """
    first = dev.await_line(r"Expect up to (\d+) s", 15.0, command="survey capture")
    want("Capture started" in first, "the first capture did not start", first)
    worst_case = want_int(r"Expect up to (\d+) s", first,
                          "the capture did not report its worst-case duration")
    try:
        second = dev.cmd("survey capture", timeout=6.0)
        want("already running" in second,
             "a second capture was accepted while one was running", second)
        # A shell trigger is refused for the same reason, and that is the sharper edge of
        # the same claim: "survey scan" during a cycle would win the library, starve the
        # cycle's next step, and hand the cycle a result it did not ask for.
        scan = dev.cmd("survey scan", timeout=6.0)
        want("capture cycle is running" in scan.lower(),
             "a location trigger was accepted from the shell while a cycle was running -- "
             "its result would be filed under the cycle's timestamps", scan)
    finally:
        # Do not leave the cycle running into the next test.
        dev.await_line(r"Cycle \d+ stored|Cycle \d+ produced nothing",
                       worst_case + CAPTURE_STORE_SLACK_SECONDS)


# --- Tests: reboot ----------------------------------------------------------------------
#
# Registered last on purpose. Tests run in definition order, and on a build without
# APP_SURVEY_CAPTURE_OWNS_SEARCH a cold boot hands the Location library to the asset
# tracker's startup GNSS-first sample, which can hold it for the whole 600 s GNSS timeout.
# Rebooting before the radio tests made them retry until they skipped -- which reads as "no
# coverage" when the cause was the test order. OWNS_SEARCH suppresses that sample, so the
# hazard is gone on the default survey build; the ordering stays because the option is
# documented as disable-able for comparison against upstream behaviour.

def reboot(dev: "Device") -> str:
    """Cold-reboot and return the boot log from reset up to the partition check.

    Issued through await_line's `command` rather than with cmd() first. cmd() reads for
    its whole timeout before returning, so the first seconds of the boot land in *its*
    reply and never reach the caller: a capture that begins at 01.69 has already missed
    main.c entering its sampling state at 01.68. That cost two separate debugging rounds
    on this bench -- once for the watchdog channel line, once for the sampling entry --
    and both times the failure read as "the device never logged it" rather than "we
    started listening too late".
    """
    boot = dev.await_line(r"LittleFS partition size verified.*blocks", 30.0,
                          command="kernel reboot cold")
    dev.wake()
    return boot


@test("capture_thread_is_watchdogged", tags=(TAG_DESTRUCTIVE,))
def t_capture_watchdog(dev: "Device") -> None:
    """The capture thread got a task-watchdog channel at boot.

    It can block indefinitely in one place -- the record publish uses K_FOREVER -- and the
    channel is what turns "surveying stopped forever, silently" into a reset. So it has to
    be there, and it is not free: upstream already claims all eight channels prj.conf
    allocates, and `task_wdt_add()` answers the ninth caller with -ENOMEM.

    That is not hypothetical. It happened on this bench, and the symptom was twelve
    unrelated tests failing at once with nothing pointing at the cause but one line in a
    boot log. Assert on the positive line the module logs when registration succeeds --
    absence of the error is also what a build with the watchdog block deleted looks like.
    """
    # Awaited from the reboot command itself, not after a separate cmd(). The registration
    # happens as the capture thread starts, about half a second into the boot, and cmd()
    # reads for its whole timeout -- so issuing the reboot with cmd() first consumes the
    # very line this test is looking for and the failure reads as "never reported".
    boot = dev.await_line(r"Capture thread watchdog channel -?\d+|"
                          r"Failed to add capture thread to watchdog", 30.0,
                          command="kernel reboot cold")
    dev.wake()
    want("Failed to add capture thread to watchdog" not in boot,
         "task_wdt_add() refused the capture thread -- CONFIG_TASK_WDT_CHANNELS is one "
         "short of what this build needs, and the thread is running uncovered", boot)
    channel = want_int(r"Capture thread watchdog channel (-?\d+)", boot,
                       "the capture module never reported a watchdog channel at boot")
    want(channel >= 0, f"the capture thread reported watchdog channel {channel}", boot)


@test("owns_search_suppresses_the_application_sample", tags=(TAG_DESTRUCTIVE,))
def t_owns_search(dev: "Device") -> None:
    """The application's startup location sample is suppressed, not merely raced.

    APP_SURVEY_CAPTURE_OWNS_SEARCH is the reason a capture cycle can assume the radio is
    its own. Nothing else in this suite can fail if the suppression branch is deleted:
    without it main.c's startup trigger simply starts a search, the capture tests retry
    and pass, and the only visible symptom is occasional cycles that ran short.

    So assert the branch directly. main.c publishes LOCATION_SEARCH_TRIGGER once at
    startup, so a fresh boot must show `suppressed` advancing while `accepted` stays at
    zero -- nothing on this build starts a search until somebody asks.
    """
    reboot(dev)
    deadline = time.monotonic() + 60
    seen = None
    while time.monotonic() < deadline:
        try:
            seen = counters(dev)
        except NoCounters:
            # The shell answers before the survey module has printed its first counters
            # on some boots; keep asking rather than calling it a broken build.
            time.sleep(2.0)
            continue
        if seen.suppressed > 0:
            break
        time.sleep(2.0)

    want(seen is not None, "survey show never reported its counters after a reboot")
    # Narrowing only -- want() has already raised. Safe to lose under -O; see want_match().
    assert seen is not None
    want(seen.suppressed > 0,
         f"no trigger was suppressed in 60 s after boot (accepted {seen.accepted}, "
         f"dropped {seen.dropped}) -- main.c's startup sample is reaching the Location "
         "library, so a capture cycle is racing it")
    want(seen.accepted == 0,
         f"the location module accepted {seen.accepted} trigger(s) after boot with "
         "nothing having asked for one -- the application's sampling is still driving "
         "searches")


@test("suppressed_trigger_still_releases_main", tags=(TAG_DESTRUCTIVE,))
def t_suppressed_trigger_releases_main(dev: "Device") -> None:
    """Suppressing the trigger must not strand main.c in its sampling state.

    LOCATION_SEARCH_DONE is the only exit from main.c's two SAMPLING states, and entering
    one stops the sample timer -- so a trigger that is swallowed without an answer leaves
    main in SAMPLING with nothing left to wake it. No periodic power or environmental
    samples, and no complaint: main feeds its watchdog before waiting on zbus, not after,
    so the stall is silent. It unwedges only when some later capture cycle's own DONE
    happens along, which on an idle disconnected device is up to a cadence away.

    This is why the suppression branch answers with a synthetic DONE. The counters in
    owns_search_suppresses_the_application_sample cannot see the difference -- they read
    the same whether main is running or stuck -- so assert on main's own transition: it
    must enter a WAITING state after the SAMPLING it entered at boot.
    """
    boot = reboot(dev)
    sampling = boot.find("sampling_entry")
    want(sampling >= 0,
         "main.c never entered a sampling state after boot, so there was no suppressed "
         "trigger to answer -- this test proved nothing", boot)
    want("waiting_entry" in boot[sampling:],
         "main.c entered sampling and never came back out; the suppressed trigger was "
         "swallowed without a LOCATION_SEARCH_DONE, so main is stuck with its sample "
         "timer stopped and periodic sampling dead", boot[sampling:])


@test("partition_is_the_survey_size", tags=(TAG_DESTRUCTIVE,))
def t_partition(dev: "Device") -> None:
    """The 24 MiB overlay is in the flashed build, not the stock 1 MiB partition."""
    boot = reboot(dev)
    have = want_int(r"have (\d+) blocks", boot,
                    "boot never logged the partition size check")
    want(have == 6144,
         f"partition is {have} blocks; the survey overlay gives 6144 (24 MiB). "
         "The build was flashed without -DEXTRA_DTC_OVERLAY_FILE.")


@test("storage_survives_reboot", tags=(TAG_SLOW, TAG_DESTRUCTIVE))
def t_storage_persists(dev: "Device") -> None:
    """Records are durable across a reset -- the whole point of FW-6."""
    # Both calls are generous for the same reason: the storage thread counts every record
    # at mount before it will answer anything -- measured silent for ~45 s at 5,004
    # records. The pre-reboot call needs it too, because the test before this one cold-
    # rebooted the device and this may land inside that mount count.
    before = _survey_count(dev, timeout=180.0)
    if before == 0:
        raise Skip("no records stored; run store_increments_the_count first")
    reboot(dev)
    after = _survey_count(dev, timeout=180.0)
    want(after == before,
         f"record count changed across reboot: {before} -> {after}")


# --- Tests: the harness itself ---------------------------------------------------------
#
# A test harness that reports PASS when it is failing to read the port is worse than no
# harness. These run against survey_console.py's FakeShell, which answers a handful of
# canned commands, and prove the parts of this file that could silently degrade: that a
# reply is stripped of echo and prompt, that asynchronous output is captured, and -- most
# importantly -- that the check helpers actually fail when the device says the wrong
# thing. They are selected only by --loopback, since the fake answers nothing else.

@test("self_cmd_strips_echo_and_prompt", tags=(TAG_SELF,))
def t_self_cmd(dev: "Device") -> None:
    """cmd() returns the reply alone, with the echoed command and prompt removed."""
    reply = dev.cmd("kernel version")
    want_in("Zephyr version 4.4.0", reply)
    want("kernel version" not in reply, f"echo leaked into the reply: {reply!r}")
    want(PROMPT not in reply, f"prompt leaked into the reply: {reply!r}")


@test("self_await_line_captures_output", tags=(TAG_SELF,))
def t_self_await(dev: "Device") -> None:
    """await_line() sees output that arrives after the prompt has come back."""
    text = dev.await_line(r"SYNTHETIC", 5.0, command="survey selftest")
    want_match(r"SYNTHETIC", text, "await_line missed output it was told to wait for")


@test("self_await_line_times_out", tags=(TAG_SELF,))
def t_self_timeout(dev: "Device") -> None:
    """await_line() returns rather than hanging when the pattern never appears."""
    started = time.monotonic()
    dev.await_line(r"this string will never appear", 2.0, command="kernel version")
    took = time.monotonic() - started
    want(1.5 < took < 4.0, f"timeout took {took:.1f} s, expected about 2 s")


@test("self_checks_fail_when_they_should", tags=(TAG_SELF,))
def t_self_checks(dev: "Device") -> None:
    """The check helpers raise Failure on a wrong answer instead of passing it."""
    reply = dev.cmd("kernel version")
    for check in (lambda: want_in("Zephyr version 9.9.9", reply),
                  lambda: want_match(r"version 9\.9", reply),
                  lambda: want(False, "always false")):
        try:
            check()
        except Failure:
            continue
        raise Failure("a check that should have failed passed instead")


@test("self_trigger_notices_a_dead_trigger", tags=(TAG_SELF,))
def t_self_trigger(dev: "Device") -> None:
    """trigger() fails when the command never reaches the location module.

    The fake shell accepts "survey scan" and reports success exactly as the real one
    does, and its *observation* counters climb on every "survey show" -- a device
    sampling on its own timer, with our trigger going nowhere. Its `accepted` counter
    stays at zero, because nothing ever asked the module for anything.

    This is the regression, not merely a dead device. A harness that asks only "did a
    result appear after I sent the command" passes here; the one in this file does not,
    because it also requires the module to have accepted a trigger. If this test ever
    starts passing for the wrong reason, check that the fake's counters still advance --
    a frozen fake would make it vacuous, which is what it was before.
    """
    try:
        trigger(dev, "survey scan", "scan", 3.0, attempts=1)
    except NoCounters:
        raise Failure("the fake shell no longer prints both counter lines, so this "
                      "test cannot distinguish the case it exists for")
    except Failure as exc:
        want("never accepted a trigger" in str(exc),
             "trigger() failed, but not because the module never accepted the trigger "
             f"-- so it is not the correlation doing the work: {exc}")
        return
    except Skip:
        raise Failure("trigger() treated a dead trigger as a busy library")
    raise Failure("trigger() reported success although the location module never "
                  "accepted a trigger")


@test("self_unknown_command_is_visible", tags=(TAG_SELF,))
def t_self_unknown(dev: "Device") -> None:
    """A command missing from the build is reported, not silently empty.

    This is the failure mode that matters most: flash the default build by mistake and
    every survey test should fail loudly rather than read back nothing and pass.
    """
    reply = dev.cmd("survey definitely-not-a-command")
    want_match(r"command not found|unknown", reply,
               f"a missing command produced no diagnosis: {reply!r}")


# --- Runner ---------------------------------------------------------------------------

def select(tests: list[TestCase], args: argparse.Namespace) -> list[TestCase]:
    chosen = []
    for t in tests:
        if args.only and args.only not in t["name"]:
            continue
        # The fake shell answers only a handful of canned commands, so the device tests
        # are meaningless against it, and the self-tests are meaningless against a device.
        if (TAG_SELF in t["tags"]) != bool(args.loopback):
            continue
        if TAG_RADIO in t["tags"] and not args.radio:
            continue
        if TAG_DESTRUCTIVE in t["tags"] and not args.destructive:
            continue
        if TAG_SLOW in t["tags"] and args.fast:
            continue
        chosen.append(t)
    return chosen


def confirm_destructive(tests: list[TestCase]) -> bool:
    """Say what will be done to the device's stored data, and require a yes."""
    names = [t["name"] for t in tests if TAG_DESTRUCTIVE in t["tags"]]
    if not names:
        return True
    print("These tests write to the survey partition or reset the device:")
    for name in names:
        print(f"  {name}")
    print("Stored records cannot be deleted individually, so a synthetic record added\n"
          "here stays in the capture. Continue? [y/N] ", end="", flush=True)
    return sys.stdin.readline().strip().lower() in ("y", "yes")


def run(dev: "Device", tests: list[TestCase]) -> list[tuple[str, str, str]]:
    results: list[tuple[str, str, str]] = []
    width = max(len(t["name"]) for t in tests)
    for t in tests:
        print(f"  {t['name']:<{width}} ", end="", flush=True)
        started = time.monotonic()
        try:
            t["fn"](dev)
            status, detail = "PASS", ""
        except Skip as exc:
            status, detail = "SKIP", str(exc)
        except Failure as exc:
            status, detail = "FAIL", str(exc)
        except Exception as exc:  # harness or serial fault, not a device verdict
            status, detail = "ERROR", f"{type(exc).__name__}: {exc}"
        took = time.monotonic() - started
        # Flushed per test, because a run of this length is usually watched through a
        # redirect: block buffering makes a suite that is patiently waiting out a 180 s
        # GNSS timeout look identical to one that has hung on the port.
        print(f"{status}  ({took:.1f}s)", flush=True)
        if detail and status != "PASS":
            for line in detail.split("\n"):
                print(f"      {line}", flush=True)
        results.append((t["name"], status, detail))
    return results


def main() -> int:
    p = argparse.ArgumentParser(description=__doc__,
                                formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("--port", help="serial port; auto-detected when omitted")
    p.add_argument("--baud", type=int, default=115200)
    p.add_argument("--only", help="run tests whose name contains this substring")
    p.add_argument("--radio", action="store_true",
                   help="include tests needing network registration or a sky view")
    p.add_argument("--destructive", action="store_true",
                   help="include tests that write records or reset the device (asks first)")
    p.add_argument("--fast", action="store_true", help="skip tests tagged slow")
    p.add_argument("--yes", action="store_true",
                   help="answer the --destructive confirmation, for unattended runs")
    p.add_argument("--list", action="store_true", help="list tests and exit")
    p.add_argument("--loopback", action="store_true",
                   help="run against the fake shell in survey_console.py")
    p.add_argument("-v", "--verbose", action="store_true",
                   help="show the serial traffic")
    args = p.parse_args()

    if args.list:
        selected = {t["name"] for t in select(_TESTS, args)}
        for t in _TESTS:
            tags = ",".join(sorted(t["tags"])) or "-"
            mark = "*" if t["name"] in selected else " "
            print(f"{mark} {t['name']:<34} [{tags}]  {t['doc']}")
        print("\n* would run with the flags given; the rest are filtered out.")
        return 0

    tests = select(_TESTS, args)
    if not tests:
        print("No tests selected.", file=sys.stderr)
        return 2

    if args.destructive and not args.yes and not confirm_destructive(tests):
        print("Aborted.", file=sys.stderr)
        return 2

    fake = None
    ser = None
    try:
        if args.loopback:
            port, fake = console.open_loopback()
            print(f"[loopback: fake shell on {port}]")
        elif args.port:
            port = args.port
        else:
            # Nordic VID only. Auto-detect writes shell commands to whatever it picks,
            # and candidate_ports() deliberately includes non-Nordic ports for the cases
            # where --port names one. survey_console.py refuses to auto-write to them
            # for the same reason.
            nordic = [p for p in console.candidate_ports() if p.vid == console.NORDIC_VID]
            if not nordic:
                print("No Nordic-VID serial port found. Pass --port, or run "
                      "scripts/survey_console.py --list.", file=sys.stderr)
                return 2
            port = nordic[0].device
            if len(nordic) > 1:
                # The Thingy:91 X exposes two, and only one is the nRF9151 console.
                print(f"[{len(nordic)} Nordic ports found, using {port}; run "
                      f"scripts/survey_console.py --identify to confirm]", file=sys.stderr)
            else:
                print(f"Using {port} (pass --port to override)")

        ser = serial.Serial(port, args.baud, timeout=0.1)
        dev = Device(ser, verbose=args.verbose)
        dev.wake()

        print(f"Running {len(tests)} test(s):")
        results = run(dev, tests)

        if not args.loopback:
            # The suite leaves synthetic data in the observation cache, which would be
            # confusing to find on the bench later. Storage cannot be undone.
            #
            # Its own handler: a --radio run takes minutes, and losing the verdict to a
            # serial hiccup during tidying up would throw away the entire result.
            try:
                dev.cmd("survey clear")
            except (OSError, serial.SerialException) as err:
                print(f"[cleanup: \"survey clear\" failed: {err}]", file=sys.stderr)
    except (OSError, serial.SerialException) as err:
        print(f"Serial error: {err}", file=sys.stderr)
        return 2
    finally:
        if ser is not None:
            ser.close()
        if fake:
            fake.stop.set()

    passed = sum(1 for _, s, _ in results if s == "PASS")
    skipped = [n for n, s, _ in results if s == "SKIP"]
    bad = [(n, s) for n, s, _ in results if s in ("FAIL", "ERROR")]

    print()
    print(f"{passed} passed, {len(skipped)} skipped, {len(bad)} failed")
    if skipped:
        print(f"  skipped: {', '.join(skipped)}")
    for name, status in bad:
        print(f"  {status}: {name}")
    if bad:
        return 1
    if not passed:
        # Every selected test skipped. Not a pass: this is the shape an indoor bench
        # produces for --radio, and exiting 0 would report coverage that did not happen.
        print("Nothing passed -- every selected test skipped.", file=sys.stderr)
        return 3
    return 0


if __name__ == "__main__":
    sys.exit(main())
