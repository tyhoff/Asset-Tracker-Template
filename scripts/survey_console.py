#!/usr/bin/env python3
#
# Copyright (c) 2026 Nordic Semiconductor ASA
#
# SPDX-License-Identifier: LicenseRef-Nordic-5-Clause

"""Non-interactive driver for the Zephyr shell on a Thingy:91 X.

Sends shell commands over a USB CDC ACM port and prints the reply, so shell
sessions can be scripted instead of typed into a terminal emulator.

The Thingy:91 X exposes two CDC ports through the nRF5340 connectivity bridge:
one is the nRF9151 application console, the other is the nRF5340 itself. Use
--identify to see which is which before relying on auto-detection.

Examples:

    # List candidate ports.
    survey_console.py --list

    # Probe Nordic candidate ports and show what each one answers.
    survey_console.py --identify

    # Run one command.
    survey_console.py survey selftest

    # Hammer the scan path -- this is the -EBUSY regression check.
    survey_console.py --repeat 5 --delay 12 survey scan

    # Watch boot output without sending anything.
    survey_console.py --monitor --timeout 30

    # Verify this script against a fake shell, no hardware needed.
    survey_console.py --loopback survey selftest
"""

import argparse
import os
import re
import select
import sys
import threading
import time

try:
    import serial
    from serial.tools import list_ports
except ImportError:
    sys.exit(
        "pyserial is required. The nRF Connect SDK toolchain already provides it, so\n"
        "the simplest fix is to run this script through the toolchain:\n"
        "\n"
        "  nrfutil toolchain-manager launch --ncs-version v3.4.0 -- \\\n"
        "      python3 scripts/survey_console.py --identify\n"
        "\n"
        "Otherwise install it:  pipx install pyserial"
    )

# Nordic Semiconductor USB vendor ID, used to spot connectivity-bridge ports.
NORDIC_VID = 0x1915

DEFAULT_BAUD = 115200
DEFAULT_PROMPT = "uart:~$"

# CSI escape sequences, which the Zephyr shell emits for colour and cursor
# movement. Stripped so that captured output can be matched as plain text.
ANSI_CSI_RE = re.compile(r"\x1b\[[\x30-\x3f]*[\x20-\x2f]*[\x40-\x7e]")
# String sequences: OSC, DCS, APC, PM. These carry a payload terminated by BEL or
# ST, so they must be matched whole -- stripping only the introducer would leave the
# payload behind as literal text.
ANSI_STRING_RE = re.compile(r"\x1b[\x50\x5d\x5e\x5f].*?(?:\x1b\\|\x07)", re.S)
# Remaining two-character escapes. The introducers handled above are excluded, as is
# 0x5b: that is the CSI introducer, and matching it here would eat the start of a CSI
# sequence split across two reads.
ANSI_OTHER_RE = re.compile(r"\x1b[\x40-\x4f\x51-\x5a\x5c]")

# Longest escape sequence worth waiting for when one is split across reads.
MAX_ESCAPE_LEN = 32


def strip_ansi(text):
    """Remove terminal escape sequences and bare carriage returns."""
    text = ANSI_STRING_RE.sub("", text)
    text = ANSI_CSI_RE.sub("", text)
    text = ANSI_OTHER_RE.sub("", text)
    return text.replace("\r", "")


def candidate_ports():
    """Return plausible device ports, Nordic ones first.

    Nordic-VID ports sort first, but non-Nordic ports are still returned: a
    Thingy:91 X reached through an external debug probe or a USB-serial adapter
    will not carry Nordic's VID.
    """
    nordic = []
    others = []
    for port in sorted(list_ports.comports(), key=lambda p: p.device):
        # Skip macOS built-ins and the tty.* twins of each cu.* port. Reading
        # from tty.* on macOS blocks until DCD asserts, which hangs on CDC.
        if "/dev/tty." in port.device:
            continue
        if any(builtin in port.device
               for builtin in ("debug-console", "Bluetooth", "wlan-debug")):
            continue
        if port.vid == NORDIC_VID:
            nordic.append(port)
        else:
            others.append(port)
    return nordic + others


def describe_port(port):
    vid = f"{port.vid:04x}" if port.vid is not None else "----"
    pid = f"{port.pid:04x}" if port.pid is not None else "----"
    bits = [f"{port.device:<28} {vid}:{pid}"]
    if port.manufacturer:
        bits.append(port.manufacturer)
    if port.product:
        bits.append(port.product)
    if port.serial_number:
        bits.append(f"sn={port.serial_number}")
    if port.location:
        bits.append(f"loc={port.location}")
    return "  ".join(bits)


def drain(ser, quiet_for=0.15, max_wait=1.5):
    """Discard buffered output until the line has been quiet for a moment.

    reset_input_buffer() alone is not enough: the shell prints a prompt after
    every command, and bytes still in flight when we reset would arrive
    afterwards and be mistaken for the next command's reply.
    """
    deadline = time.monotonic() + max_wait
    last_rx = time.monotonic()
    while time.monotonic() < deadline:
        if ser.read(max(1, ser.in_waiting)):
            last_rx = time.monotonic()
        elif (time.monotonic() - last_rx) >= quiet_for:
            return


def read_until_idle(ser, timeout, quiet_for, prompt, require=None):
    """Accumulate output until the prompt returns, or the line goes quiet.

    Returns (text, reason). Waiting for the prompt is the fast path; the
    quiet-for fallback covers commands that print asynchronously or firmware
    with a different prompt.

    `require` is a marker -- normally the echoed command -- that must appear
    before a prompt is allowed to end the read. Without it, a prompt left in
    the buffer by the previous command ends the read immediately and the real
    reply is lost.
    """
    deadline = time.monotonic() + timeout
    buf = bytearray()
    last_rx = time.monotonic()
    reason = "timeout"
    synced = require is None

    while time.monotonic() < deadline:
        chunk = ser.read(max(1, ser.in_waiting))
        now = time.monotonic()
        if chunk:
            buf.extend(chunk)
            last_rx = now
            text = strip_ansi(bytes(buf).decode("utf-8", "replace"))
            if not synced:
                synced = require in text
                # Fall through rather than continue: echo, reply and prompt often
                # arrive in a single read, and skipping the check here would cost a
                # full quiet_for on every command.
            # The echoed command line also ends in the prompt, so require the
            # prompt to be the tail of the buffer rather than merely present.
            if synced and prompt and text.rstrip().endswith(prompt):
                reason = "prompt"
                break
        elif buf and synced and (now - last_rx) >= quiet_for:
            reason = "idle"
            break

    return strip_ansi(bytes(buf).decode("utf-8", "replace")), reason


def monitor(ser, timeout):
    """Print incoming output as it arrives, until the timeout expires.

    Streams rather than buffering: the point of this mode is watching a device
    boot, which is useless if nothing appears until the end.
    """
    deadline = time.monotonic() + timeout
    total = 0
    # An escape sequence can be split across reads. Hold back a short tail so it
    # is stripped as one piece rather than printed as literal text.
    pending = ""
    while time.monotonic() < deadline:
        chunk = ser.read(max(1, ser.in_waiting))
        if not chunk:
            continue
        total += len(chunk)
        pending += chunk.decode("utf-8", "replace")
        text = strip_ansi(pending)
        # Keep a trailing partial escape sequence for the next round, but only while it
        # can still become one: no escape sequence contains a newline, and none is longer
        # than MAX_ESCAPE_LEN. Without both tests a stray ESC that strip_ansi does not
        # recognise sits at index 0 forever and stalls the stream until the timeout.
        cut = text.rfind("\x1b")
        tail = text[cut:] if cut != -1 else ""
        if cut == -1 or "\n" in tail or len(tail) > MAX_ESCAPE_LEN:
            pending = ""
        else:
            pending, text = tail, text[:cut]
        if text:
            sys.stdout.write(text)
            sys.stdout.flush()
    if pending:
        sys.stdout.write(strip_ansi(pending))
    if total == 0:
        print(f"\n[nothing received in {timeout:.0f}s -- device quiet, or wrong "
              f"port; try --identify]", file=sys.stderr)
        return 2
    sys.stdout.write("\n")
    return 0


def clean_reply(text, command, prompt):
    """Drop the echoed command and the trailing prompt from captured output."""
    lines = text.split("\n")
    if command:
        # The shell echoes the command, possibly after a prompt on the line.
        while lines and (command in lines[0] or lines[0].strip() == ""):
            first = lines.pop(0)
            if command in first:
                break
    while lines and (lines[-1].strip() in ("", prompt) or
                     lines[-1].rstrip().endswith(prompt)):
        stripped = lines[-1].rstrip()
        if stripped.endswith(prompt):
            remainder = stripped[: -len(prompt)].strip()
            lines.pop()
            if remainder:
                lines.append(remainder)
            break
        lines.pop()
    return "\n".join(lines).strip("\n")


def send_command(ser, command, timeout, quiet_for, prompt):
    """Write one command and return its reply."""
    ser.reset_input_buffer()
    drain(ser)
    # Carriage return only. Sending CRLF makes the shell act on the CR and then
    # print a second prompt for the LF, leaving a stray prompt in the buffer.
    ser.write((command + "\r").encode())
    ser.flush()
    text, reason = read_until_idle(ser, timeout, quiet_for, prompt,
                                   require=command)
    return clean_reply(text, command, prompt), reason


def wake_shell(ser, prompt, timeout=2.0):
    """Send a bare return so the shell prints a prompt, then swallow it."""
    ser.reset_input_buffer()
    ser.write(b"\r")
    ser.flush()
    text, _ = read_until_idle(ser, timeout, 0.3, prompt)
    drain(ser)
    return text


def do_list():
    ports = candidate_ports()
    if not ports:
        print("No candidate serial ports found.")
        print("Check the USB-C cable and that the power switch is on.")
        return 1
    print(f"{len(ports)} candidate port(s):")
    for port in ports:
        marker = "*" if port.vid == NORDIC_VID else " "
        print(f" {marker} {describe_port(port)}")
    print("\n* = Nordic VID. Use --identify to see which port answers.")
    return 0


def do_identify(baud, timeout, prompt, quiet_for, probe_all):
    ports = candidate_ports()
    if not ports:
        print("No candidate serial ports found.")
        return 1
    # Probing writes to a port. Restrict that to Nordic devices by default so a
    # printer or GPS on another port is not sent stray shell commands.
    if not probe_all:
        nordic = [p for p in ports if p.vid == NORDIC_VID]
        skipped = [p.device for p in ports if p.vid != NORDIC_VID]
        if skipped:
            print(f"Skipping {len(skipped)} non-Nordic port(s): "
                  f"{', '.join(skipped)}")
            print("Pass --probe-all to probe them too.\n")
        ports = nordic
        if not ports:
            print("No Nordic-VID ports found. Use --probe-all or --port.")
            return 1
    responded = []
    for port in ports:
        print(f"--- {port.device} ---")
        try:
            with serial.Serial(port.device, baud, timeout=0.05) as ser:
                wake_shell(ser, prompt)
                reply, _ = send_command(ser, "kernel version", timeout,
                                        quiet_for, prompt)
                survey, _ = send_command(ser, "survey stats", timeout,
                                         quiet_for, prompt)
        except (OSError, serial.SerialException) as err:
            print(f"  could not open: {err}\n")
            continue
        if not reply and not survey:
            print("  no response\n")
            continue
        responded.append(port.device)
        print(f"  kernel version -> {reply or '(nothing)'}")
        has_survey = survey and "not found" not in survey.lower()
        print(f"  survey command -> {'present' if has_survey else 'absent'}")
        if survey:
            for line in survey.split("\n")[:4]:
                print(f"      {line}")
        print()
    if not responded:
        print("Nothing answered. Try a different --baud, or press reset.")
        return 1
    print("Responding port(s): " + ", ".join(responded))
    print("The one reporting the survey command is the nRF9151 console.")
    return 0


class FakeShell(threading.Thread):
    """A minimal Zephyr-shell impersonator on a pty, for --loopback.

    Exists so the capture, ANSI-stripping, and prompt-detection logic can be
    exercised without hardware. It is a test double, not an emulator.
    """

    # Stands in for the log lines a real device emits unprompted after boot.
    BOOT_LOG = (
        b"*** Booting nRF Connect SDK v3.4.0 ***\r\n",
        b"[00:00:00.301,000] \x1b[0m<inf> network: Network module started\x1b[0m\r\n",
        b"[00:00:01.882,000] \x1b[0m<inf> survey: Survey module ready\x1b[0m\r\n",
    )

    REPLIES = {
        "kernel version": "Zephyr version 4.4.0",
        "survey stats": (
            "gnss fixes 1  scans 1\n"
            "gnss cached 12 s ago\n"
            "scan cached 12 s ago"
        ),
        "survey selftest": (
            "*** SYNTHETIC SELFTEST DATA -- NOT A REAL OBSERVATION ***\n"
            "lat 63.4212340  lon 10.4056780  acc 4.2 m\n"
            "eci 12345678  rsrp 55 (idx) = -86 dBm"
        ),
        "survey scan": (
            'Radio scan (Wi-Fi + cellular) requested. Wait about 5-10 s, then '
            'run "survey show". If a search is already running this trigger is '
            'dropped and a warning is logged.'
        ),
    }

    def __init__(self, fd):
        super().__init__(daemon=True)
        self.fd = fd
        self.stop = threading.Event()

    def run(self):
        # Colour codes and \r\n line endings are deliberate: they are what the
        # real shell sends, and the parser has to survive them.
        line = bytearray()
        boot_line = 0
        while not self.stop.is_set():
            # Emit log-style output until the client says something, so that
            # --monitor has a real stream to exercise. A prompt written before
            # pyserial opens the slave would be flushed away by its open().
            ready, _, _ = select.select([self.fd], [], [], 0.3)
            if not ready:
                if boot_line < len(self.BOOT_LOG):
                    try:
                        os.write(self.fd, self.BOOT_LOG[boot_line])
                    except OSError:
                        return
                    boot_line += 1
                continue
            try:
                data = os.read(self.fd, 1024)
            except OSError:
                return
            if not data:
                return
            for byte in data:
                if byte == 0x0a:
                    continue  # the real shell acts on CR and ignores a bare LF
                if byte != 0x0d:
                    os.write(self.fd, bytes([byte]))  # echo, as the shell does
                    line.append(byte)
                    continue
                if line:
                    cmd = line.decode("utf-8", "replace").strip()
                    reply = self.REPLIES.get(cmd, f"{cmd}: command not found")
                    os.write(self.fd,
                             b"\r\n" + reply.replace("\n", "\r\n").encode())
                    line.clear()
                os.write(self.fd, b"\r\n\x1b[1;32muart:~$ \x1b[m")


def open_loopback():
    """Return (port_path, shell) wired to a FakeShell.

    The slave is put into raw mode before the shell writes anything. A pty starts
    in canonical mode with echo on, so a prompt emitted before pyserial opens the
    slave comes back through the line discipline mangled.
    """
    # Imported here, not at module scope: tty pulls in termios, which does not exist on
    # Windows, and the rest of the script is useful there.
    import tty

    master, slave = os.openpty()
    tty.setraw(slave)
    shell = FakeShell(master)
    shell.start()
    return os.ttyname(slave), shell


def main():
    parser = argparse.ArgumentParser(
        description="Drive the Zephyr shell on a Thingy:91 X non-interactively.",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog=__doc__.split("Examples:")[1] if "Examples:" in __doc__ else None,
    )
    parser.add_argument("command", nargs="*",
                        help="shell command, e.g. survey selftest")
    parser.add_argument("-p", "--port", help="serial port (default: auto-detect)")
    parser.add_argument("-b", "--baud", type=int, default=DEFAULT_BAUD,
                        help=f"baud rate (default {DEFAULT_BAUD})")
    parser.add_argument("-t", "--timeout", type=float, default=10.0,
                        help="seconds to wait for a reply (default 10)")
    parser.add_argument("-q", "--quiet-for", type=float, default=0.5,
                        help="idle seconds that mean output has ended "
                             "(default 0.5)")
    parser.add_argument("--prompt", default=DEFAULT_PROMPT,
                        help=f"shell prompt to synchronise on "
                             f"(default {DEFAULT_PROMPT!r})")
    parser.add_argument("-n", "--repeat", type=int, default=1,
                        help="run the command N times (default 1)")
    parser.add_argument("-d", "--delay", type=float, default=0.0,
                        help="seconds between repeats")
    parser.add_argument("-l", "--list", action="store_true",
                        help="list candidate ports and exit")
    parser.add_argument("--identify", action="store_true",
                        help="probe Nordic candidate ports and report replies")
    parser.add_argument("--probe-all", action="store_true",
                        help="with --identify, also write to non-Nordic ports")
    parser.add_argument("--monitor", action="store_true",
                        help="print incoming output without sending anything")
    parser.add_argument("--loopback", action="store_true",
                        help="run against a built-in fake shell, no hardware")
    args = parser.parse_args()

    if args.list:
        return do_list()
    if args.identify:
        return do_identify(args.baud, args.timeout, args.prompt,
                           args.quiet_for, args.probe_all)

    shell = None
    if args.loopback:
        port, shell = open_loopback()
        print(f"[loopback: fake shell on {port}]")
    elif args.port:
        port = args.port
    else:
        ports = candidate_ports()
        if not ports:
            print("No serial port found. Try --list, or pass --port.",
                  file=sys.stderr)
            return 1
        # Auto-detect writes shell commands to whatever it picks, so restrict it to
        # Nordic devices for the same reason --identify is restricted. --port overrides.
        nordic = [p for p in ports if p.vid == NORDIC_VID]
        if not nordic:
            print("No Nordic-VID serial port found. Pass --port explicitly, or run "
                  "--list to see what is attached.", file=sys.stderr)
            return 1
        port = nordic[0].device
        if len(nordic) > 1:
            print(f"[{len(nordic)} Nordic ports found, using {port}; "
                  f"run --identify to confirm]", file=sys.stderr)

    command = " ".join(args.command)
    if not command and not args.monitor:
        print("Nothing to do: pass a command, --monitor, --list, or --identify.",
              file=sys.stderr)
        return 1

    status = 0
    try:
        with serial.Serial(port, args.baud, timeout=0.05) as ser:
            if args.monitor:
                return monitor(ser, args.timeout)

            wake_shell(ser, args.prompt)
            for attempt in range(1, args.repeat + 1):
                if args.repeat > 1:
                    print(f"--- {command}  [{attempt}/{args.repeat}] ---")
                reply, reason = send_command(ser, command, args.timeout,
                                             args.quiet_for, args.prompt)
                print(reply if reply else "(no output)")
                if reason == "timeout":
                    print(f"[warning: no prompt within {args.timeout}s; "
                          f"output may be truncated]", file=sys.stderr)
                    status = 2
                if attempt < args.repeat and args.delay:
                    time.sleep(args.delay)
    except (OSError, serial.SerialException) as err:
        print(f"Serial error on {port}: {err}", file=sys.stderr)
        return 1
    finally:
        if shell:
            shell.stop.set()

    return status


if __name__ == "__main__":
    sys.exit(main())
