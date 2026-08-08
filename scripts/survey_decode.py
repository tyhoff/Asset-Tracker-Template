#!/usr/bin/env python3
#
# Copyright (c) 2026 Nordic Semiconductor ASA
#
# SPDX-License-Identifier: LicenseRef-Nordic-5-Clause

"""Decode radio-survey records into analysis-friendly JSON.

The wire format is defined by app/src/modules/survey/survey_record.cddl, which is the
authority. This decoder dispatches on the schema `version` field and refuses versions it
does not know rather than guessing at a layout.

Output uses nRF Cloud ground-fix field names (lte[] with mcc/mnc/eci/tac/earfcn/pci/adv/
rsrp/rsrq and nmr[], wifi.accessPoints[] with macAddress/signalStrength/channel/frequency,
position as lat/lon/acc/alt/spd/hdg), so converting this output into a ground-fix request
is close to identity.

No third-party dependencies, deliberately: this runs on whatever laptop is in the field,
and the CBOR subset the schema uses is small enough to read directly. See _cbor_load.

Examples:

    # Decode a "survey hex" dump captured from the device console.
    survey_decode.py --hex capture.txt

    # Decode a binary export, one JSON object per line.
    survey_decode.py export.bin > records.jsonl

    # Keep every record, reporting quality rather than dropping.
    survey_decode.py --no-drop export.bin
"""

import argparse
import json
import re
import sys

# The only schema this decoder understands. Bumping the on-device SURVEY_RECORD_VERSION
# without teaching the decoder the new layout must fail loudly, not silently mis-parse.
SUPPORTED_VERSION = 1


# --- Minimal CBOR reader ------------------------------------------------------------------
#
# Supports exactly what survey_record.cddl emits: unsigned ints, negative ints, byte
# strings, text strings, and arrays and maps in both definite and indefinite form.
# Anything else is an error, because encountering it means the file is not what we think
# it is.
#
# Indefinite-length containers are not optional to support: zcbor writes every map and
# array that way, so a real record opens with 0xBF and closes with 0xFF rather than
# carrying a count.


class CborError(ValueError):
    """Raised when the input is not decodable as the survey CBOR subset."""


# Additional-information 31 means "indefinite length" on a container, and "break" on
# major type 7. Returned as the argument so callers can spot both.
_INDEFINITE = object()
_BREAK = object()


def _read_head(data, pos):
    """Return (major_type, argument, new_pos)."""
    if pos >= len(data):
        raise CborError(f"truncated at offset {pos}")

    initial = data[pos]
    major = initial >> 5
    minor = initial & 0x1F
    pos += 1

    if minor < 24:
        return major, minor, pos
    if minor in (24, 25, 26, 27):
        width = 1 << (minor - 24)
        if pos + width > len(data):
            raise CborError(f"truncated {width}-byte argument at offset {pos}")
        return major, int.from_bytes(data[pos:pos + width], "big"), pos + width
    if minor == 31:
        if major == 7:
            return major, _BREAK, pos
        if major in (4, 5):
            return major, _INDEFINITE, pos
        raise CborError(
            f"indefinite length is not valid for major type {major} at offset {pos - 1}"
        )

    raise CborError(f"unsupported additional-information {minor} at offset {pos - 1}")


def _at_break(data, pos):
    """True when the next item is a break code. Does not consume it."""
    return pos < len(data) and data[pos] == 0xFF


def _cbor_load(data, pos=0):
    """Decode one CBOR item. Returns (value, new_pos)."""
    major, arg, pos = _read_head(data, pos)

    if arg is _BREAK:
        raise CborError(f"unexpected break code at offset {pos - 1}")

    if major == 0:
        return arg, pos
    if major == 1:
        return -1 - arg, pos
    if major in (2, 3):
        end = pos + arg
        if end > len(data):
            raise CborError(f"truncated string at offset {pos}")
        chunk = data[pos:end]
        return (chunk if major == 2 else chunk.decode("utf-8")), end
    if major == 4:
        out = []
        if arg is _INDEFINITE:
            while not _at_break(data, pos):
                item, pos = _cbor_load(data, pos)
                out.append(item)
            if pos >= len(data):
                raise CborError("unterminated indefinite-length array")
            pos += 1
        else:
            for _ in range(arg):
                item, pos = _cbor_load(data, pos)
                out.append(item)
        return out, pos
    if major == 5:
        out = {}
        if arg is _INDEFINITE:
            while not _at_break(data, pos):
                key, pos = _cbor_load(data, pos)
                value, pos = _cbor_load(data, pos)
                out[key] = value
            if pos >= len(data):
                raise CborError("unterminated indefinite-length map")
            pos += 1
        else:
            for _ in range(arg):
                key, pos = _cbor_load(data, pos)
                value, pos = _cbor_load(data, pos)
                out[key] = value
        return out, pos

    raise CborError(f"unsupported major type {major} at offset {pos - 1}")


def cbor_decode_one(data):
    """Decode exactly one CBOR item, rejecting trailing bytes."""
    value, pos = _cbor_load(data, 0)
    if pos != len(data):
        raise CborError(f"{len(data) - pos} trailing byte(s) after the CBOR item")
    return value


# --- Unit conversions ---------------------------------------------------------------------
#
# These mirror survey_obs.c exactly. The record stores raw 3GPP indices because they are
# small and lossless; the conversion to dBm/dB belongs on this side.


def rsrp_idx_to_dbm(idx):
    """3GPP RSRP index to dBm. Mirrors SURVEY_RSRP_IDX_TO_DBM in survey_obs.c."""
    return idx - 140 if idx < 0 else idx - 141


def rsrq_idx_to_db(idx):
    """3GPP RSRQ index to dB. Mirrors survey_rsrq_idx_to_db in survey_obs.c."""
    if idx < 0:
        return (idx - 39) * 0.5
    if idx < 35:
        return (idx - 40) * 0.5
    return (idx - 41) * 0.5


# enum wifi_frequency_bands, as used by the Zephyr Wi-Fi driver.
WIFI_BAND_2_4_GHZ = 0
WIFI_BAND_5_GHZ = 1
WIFI_BAND_6_GHZ = 2

WIFI_BAND_NAMES = {
    WIFI_BAND_2_4_GHZ: "2.4GHz",
    WIFI_BAND_5_GHZ: "5GHz",
    WIFI_BAND_6_GHZ: "6GHz",
}


def wifi_frequency_mhz(band, channel):
    """Channel plus band to centre frequency, or None if the pair is not valid.

    Mirrors wifi_frequency_mhz in survey_obs.c. Frequency is derived rather than stored
    because it is a pure function of the pair. An unrecognised pair yields None rather
    than a fabricated frequency -- the driver does not validate the band.
    """
    if band == WIFI_BAND_2_4_GHZ:
        if channel == 14:
            return 2484
        return 2407 + 5 * channel if 1 <= channel <= 13 else None
    if band == WIFI_BAND_5_GHZ:
        return 5000 + 5 * channel if 32 <= channel <= 177 else None
    if band == WIFI_BAND_6_GHZ:
        if channel == 2:
            return 5935
        return 5950 + 5 * channel if 1 <= channel <= 233 else None
    return None


PROFILE_NAMES = {0: "FAST", 1: "DEEP"}
# enum survey_time_base, cast straight onto the wire by the encoder.
TIME_BASE_NAMES = {0: "unset", 1: "uptime", 2: "unix"}
NETWORK_MODE_NAMES = {0: "unknown", 1: "LTE-M", 2: "NB-IoT"}


# --- Record decoding ----------------------------------------------------------------------


def _decode_gnss_fix(raw):
    """A gnss-fix map to ground-fix position names, rescaled from stored integers."""
    fix = {
        "lat": raw[1] / 1e7,
        "lon": raw[2] / 1e7,
        "acc": raw[3] / 1000.0,
    }
    # Absent means not measured. Emitting these as 0 would assert a reading, so a missing
    # key stays missing all the way to the output.
    if 4 in raw:
        fix["alt"] = raw[4] / 1000.0
    if 5 in raw:
        fix["spd"] = raw[5] / 1000.0
    if 6 in raw:
        fix["hdg"] = raw[6] / 100.0
    if 7 in raw:
        fix["satsUsed"] = raw[7]
    return fix


def _decode_cell(raw):
    """An identified cell (serving or GCI) to ground-fix lte[] names."""
    cell = {
        "eci": raw[1],
        "mcc": raw[2],
        "mnc": raw[3],
        "tac": raw[4],
    }
    if 5 in raw:
        cell["earfcn"] = raw[5]
    if 6 in raw:
        cell["rsrp"] = rsrp_idx_to_dbm(raw[6])
        cell["rsrpIndex"] = raw[6]
    if 7 in raw:
        cell["rsrq"] = rsrq_idx_to_db(raw[7])
        cell["rsrqIndex"] = raw[7]
    if 8 in raw:
        cell["adv"] = raw[8]
    return cell


def _decode_neighbour(raw):
    """A neighbour measurement to ground-fix nmr[] names."""
    nbr = {"pci": raw[1]}
    if 2 in raw:
        nbr["earfcn"] = raw[2]
    if 3 in raw:
        nbr["rsrp"] = rsrp_idx_to_dbm(raw[3])
        nbr["rsrpIndex"] = raw[3]
    if 4 in raw:
        nbr["rsrq"] = rsrq_idx_to_db(raw[4])
        nbr["rsrqIndex"] = raw[4]
    if 5 in raw:
        nbr["timeDiff"] = raw[5]
    return nbr


def _decode_access_point(raw):
    """An access point to ground-fix wifi.accessPoints[] names."""
    mac = raw[1]
    ap = {
        "macAddress": ":".join(f"{b:02x}" for b in mac),
        "signalStrength": raw[2],
    }
    # Band is only ever stored alongside a channel: the zero band value is 2.4 GHz rather
    # than "unknown", so band on its own would assert a measurement never made.
    if 3 in raw:
        ap["channel"] = raw[3]
        band = raw.get(4)
        if band is not None:
            ap["band"] = WIFI_BAND_NAMES.get(band, f"unknown({band})")
            freq = wifi_frequency_mhz(band, raw[3])
            if freq is not None:
                ap["frequency"] = freq
    return ap


def decode_session(data):
    """Decode a session header. Raises ValueError on an unknown version."""
    raw = cbor_decode_one(data) if isinstance(data, (bytes, bytearray)) else data

    if not isinstance(raw, dict):
        raise CborError(f"expected a CBOR map at the top level, got {type(raw).__name__}")

    version = raw.get(1)
    if version != SUPPORTED_VERSION:
        raise ValueError(
            f"unsupported session schema version {version!r}; "
            f"this decoder understands version {SUPPORTED_VERSION}"
        )

    session = {
        "version": version,
        "deviceId": raw[2],
        "appVersion": raw[3],
        "modemVersion": raw[4],
    }
    if 5 in raw:
        session["exportedAt"] = raw[5]
    return session


def decode_record(data):
    """Decode one record. Raises ValueError on an unknown version."""
    raw = cbor_decode_one(data) if isinstance(data, (bytes, bytearray)) else data

    if not isinstance(raw, dict):
        raise CborError(f"expected a CBOR map at the top level, got {type(raw).__name__}")

    version = raw.get(1)
    if version != SUPPORTED_VERSION:
        raise ValueError(
            f"unsupported record schema version {version!r}; "
            f"this decoder understands version {SUPPORTED_VERSION}"
        )

    rec = {
        "version": version,
        "sequence": raw[2],
        "profile": PROFILE_NAMES.get(raw[3], f"unknown({raw[3]})"),
        "timeBase": TIME_BASE_NAMES.get(raw[4], f"unknown({raw[4]})"),
        "tBase": raw[5],
    }

    if 6 in raw:
        rec["gnssBefore"] = _decode_gnss_fix(raw[6])
    if 7 in raw:
        rec["gnssAfter"] = _decode_gnss_fix(raw[7])

    offsets = {}
    for key, name in ((8, "cellStartMs"), (9, "cellEndMs"), (10, "wifiStartMs"),
                      (11, "wifiEndMs"), (12, "gnssAfterMs")):
        if key in raw:
            offsets[name] = raw[key]
    if offsets:
        rec["offsets"] = offsets

    # The serving cell leads lte[] and carries the neighbour list, matching how a
    # ground-fix request is shaped. GCI cells follow as further identified cells.
    lte = []
    if 13 in raw:
        serving = _decode_cell(raw[13])
        if 14 in raw:
            serving["nmr"] = [_decode_neighbour(n) for n in raw[14]]
        lte.append(serving)
    elif 14 in raw:
        # Neighbours with no identified serving cell still describe the radio
        # environment, so they are kept rather than dropped with the missing parent.
        lte.append({"nmr": [_decode_neighbour(n) for n in raw[14]]})

    if 15 in raw:
        lte.extend(_decode_cell(c) for c in raw[15])
    if lte:
        rec["lte"] = lte

    if 16 in raw:
        rec["wifi"] = {"accessPoints": [_decode_access_point(a) for a in raw[16]]}

    if 17 in raw:
        rec["networkMode"] = NETWORK_MODE_NAMES.get(raw[17], f"unknown({raw[17]})")

    return rec


# --- Ground-truth interpolation -----------------------------------------------------------


def scan_window(rec):
    """Return (start_ms, end_ms) of the radio scan relative to tBase, or None.

    The window spans both legs: Wi-Fi runs on the nRF7002 and overlaps the cellular
    measurement, so the union is what the position has to cover.
    """
    offsets = rec.get("offsets", {})
    starts = [offsets[k] for k in ("cellStartMs", "wifiStartMs") if k in offsets]
    ends = [offsets[k] for k in ("cellEndMs", "wifiEndMs") if k in offsets]
    if not starts or not ends:
        return None
    return min(starts), max(ends)


def interpolate(rec):
    """Attach an interpolated ground-truth position at the scan midpoint.

    Sets rec["interpolated"] with lat/lon and interp_uncertainty_m, or leaves the record
    untouched when the brackets or the scan window are missing. The raw bracketing fixes
    are always retained: the interpolated value is an addition, never a replacement.

    The uncertainty model is deliberately simple and stated rather than tuned:

        interp_uncertainty_m = speed * (gap / 2 + scan_duration / 2)

    The first term bounds how far a non-linear path can depart from the straight line
    between the brackets; the second covers the device moving while the scan is running.
    Speed is the faster of the two bracketing fixes. With no speed reported the model has
    nothing to work with and the uncertainty is reported as None.
    """
    before = rec.get("gnssBefore")
    after = rec.get("gnssAfter")
    if not before or not after:
        return rec

    window = scan_window(rec)
    gnss_after_ms = rec.get("offsets", {}).get("gnssAfterMs")
    # "is None", not falsiness: the firmware omits an offset it did not measure, so 0 here
    # is a measurement -- the same absent-not-zero rule the wire format is built on.
    if window is None or gnss_after_ms is None:
        return rec
    if gnss_after_ms <= 0:
        # The closing fix at or before the opening one. Interpolating would extrapolate
        # backwards past the bracket, or divide by zero, and silently report a position
        # the device was never at.
        rec["interp_skipped"] = "non_positive_bracket_gap"
        return rec

    midpoint = (window[0] + window[1]) / 2.0
    frac = midpoint / gnss_after_ms

    interp = {
        "lat": before["lat"] + frac * (after["lat"] - before["lat"]),
        "lon": before["lon"] + frac * (after["lon"] - before["lon"]),
        "atMs": midpoint,
    }

    speeds = [f["spd"] for f in (before, after) if "spd" in f]
    if speeds:
        gap_s = gnss_after_ms / 1000.0
        scan_s = (window[1] - window[0]) / 1000.0
        interp["interp_uncertainty_m"] = max(speeds) * (gap_s / 2.0 + scan_s / 2.0)
    else:
        interp["interp_uncertainty_m"] = None

    rec["interpolated"] = interp
    return rec


# --- Quality gating -----------------------------------------------------------------------


class Gate:
    """Configurable quality gate. Never silent: every decision is counted and reported."""

    def __init__(self, max_acc_m=100.0, max_interp_uncertainty_m=50.0,
                 require_bracket=True, allow_uptime=False):
        self.max_acc_m = max_acc_m
        self.max_interp_uncertainty_m = max_interp_uncertainty_m
        self.require_bracket = require_bracket
        self.allow_uptime = allow_uptime

    def reasons(self, rec):
        """Return the list of reasons this record fails the gate. Empty means it passes."""
        out = []

        # An uptime-based record cannot be correlated with anything off-device, so it can
        # never be scored against ground truth however good its fix was.
        # Named by what the record actually says, so the drop report points at the real
        # cause: "unset" means nothing ever timestamped the cycle, which is a different
        # problem from a clock that ran but was never synchronised.
        time_base = rec.get("timeBase")
        if time_base != "unix" and not self.allow_uptime:
            out.append("uptime_timebase" if time_base == "uptime"
                       else f"{time_base}_timebase")

        has_bracket = "gnssBefore" in rec and "gnssAfter" in rec
        if self.require_bracket and not has_bracket:
            out.append("missing_bracket")

        if self.max_acc_m is not None:
            for key in ("gnssBefore", "gnssAfter"):
                fix = rec.get(key)
                if fix and fix["acc"] > self.max_acc_m:
                    out.append("poor_gnss_accuracy")
                    break

        if self.max_interp_uncertainty_m is not None:
            unc = rec.get("interpolated", {}).get("interp_uncertainty_m")
            if unc is not None and unc > self.max_interp_uncertainty_m:
                out.append("excessive_interp_uncertainty")

        return out


# --- Input handling -----------------------------------------------------------------------

_HEX_BLOCK = re.compile(
    r"-----BEGIN SURVEY RECORD-----\s*(.*?)\s*-----END SURVEY RECORD-----",
    re.DOTALL,
)


def records_from_hex_capture(text):
    """Extract records from a console capture containing "survey hex" output.

    Matches the delimiters cmd_survey_hex prints, so a terminal log can be fed in
    unedited. Whitespace inside a block is ignored, which is what makes the wrapped
    hex lines work.
    """
    out = []
    for block in _HEX_BLOCK.findall(text):
        out.append(bytes.fromhex("".join(block.split())))
    if not out:
        raise ValueError(
            "no '-----BEGIN SURVEY RECORD-----' block found; is this a survey hex capture?"
        )
    return out


def main(argv=None):
    parser = argparse.ArgumentParser(
        description="Decode radio-survey CBOR records into JSON Lines.",
    )
    parser.add_argument("input", help="file to read; '-' for stdin")
    parser.add_argument("--hex", action="store_true",
                        help="input is a console capture of 'survey hex' output")
    parser.add_argument("--no-drop", action="store_true",
                        help="emit every record, annotated with its gate result")
    parser.add_argument("--max-acc", type=float, default=100.0,
                        help="drop records whose bracket accuracy exceeds this (metres)")
    parser.add_argument("--max-interp-uncertainty", type=float, default=50.0,
                        help="drop records whose interpolation uncertainty exceeds this")
    parser.add_argument("--allow-uptime", action="store_true",
                        help="keep records timestamped against uptime rather than Unix time")
    parser.add_argument("--allow-missing-bracket", action="store_true",
                        help="keep records without both bracketing GNSS fixes")
    args = parser.parse_args(argv)

    if args.hex:
        text = sys.stdin.read() if args.input == "-" else open(args.input).read()
        try:
            blobs = records_from_hex_capture(text)
        except ValueError as err:
            # A truncated terminal capture is the normal field failure, not an
            # exceptional one. Report it as a message rather than a traceback.
            print(f"could not read hex capture: {err}", file=sys.stderr)
            return 1
    else:
        raw = sys.stdin.buffer.read() if args.input == "-" else open(args.input, "rb").read()
        # Without the CP8 export framing there is no length prefix to split on, so a
        # bare binary input is treated as exactly one record.
        blobs = [raw]

    gate = Gate(
        max_acc_m=args.max_acc,
        max_interp_uncertainty_m=args.max_interp_uncertainty,
        require_bracket=not args.allow_missing_bracket,
        allow_uptime=args.allow_uptime,
    )

    read = written = 0
    dropped = {}

    for blob in blobs:
        read += 1
        try:
            rec = interpolate(decode_record(blob))
        # AttributeError included on purpose: a top-level CBOR item that is not a map --
        # a corrupted capture that still parses -- fails on .get() rather than raising
        # CborError, and a traceback in place of a drop count is the wrong answer.
        except (CborError, ValueError, KeyError, AttributeError) as err:
            dropped["decode_error"] = dropped.get("decode_error", 0) + 1
            print(f"record {read}: {err}", file=sys.stderr)
            continue

        reasons = gate.reasons(rec)
        if reasons and not args.no_drop:
            for reason in reasons:
                dropped[reason] = dropped.get(reason, 0) + 1
            continue

        if reasons:
            rec["qualityFlags"] = reasons
        print(json.dumps(rec, sort_keys=True))
        written += 1

    print(f"records in: {read}, out: {written}", file=sys.stderr)
    for reason, count in sorted(dropped.items()):
        print(f"  dropped {reason}: {count}", file=sys.stderr)

    return 0


if __name__ == "__main__":
    sys.exit(main())
