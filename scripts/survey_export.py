#!/usr/bin/env python3
#
# Copyright (c) 2026 Nordic Semiconductor ASA
#
# SPDX-License-Identifier: LicenseRef-Nordic-5-Clause

"""Export stored survey records off a Thingy:91 X over MCUmgr fs_mgmt (CP8 / FW-8).

This is the technical fallback for CP9's browser page: when Web Serial or the page itself
is unavailable, this is "run this script" instead of "wait for a fix". It walks the same
on-flash layout the browser client will (fixed-stride slots addressed by the header's
read/write offsets, see app/src/modules/survey/survey_store.h and
app/src/modules/storage/backends/littlefs_backend.c), verifies each data file's CRC32 via
fs_mgmt before trusting its bytes, and writes out a tightly packed stream of
length-prefixed CBOR records -- 4-byte little-endian length, then that many bytes of CBOR,
repeated -- which is what scripts/survey_decode.py's bare-binary input path expects.

There is deliberately no "clear" command here. Clearing the partition is a destructive,
explicit-confirmation operation that belongs on a console session
(the upstream `att_storage` shell), not behind an export transport that a non-technical
operator's browser page also talks to.

Examples:

    # Export everything from the shell transport (uart0, the known-good port).
    survey_export.py --port /dev/cu.usbmodem102 --baud 115200 out.bin

    # Export only records with sequence >= 500, from the dedicated export transport.
    survey_export.py --port /dev/cu.usbmodem105 --baud 1000000 --since 500 out.bin

    # Just report what's on the device without downloading records.
    survey_export.py --port /dev/cu.usbmodem102 --baud 115200 --stats-only -
"""

from __future__ import annotations

import argparse
import struct
import sys
import zlib
from typing import BinaryIO, Final, Iterator, Optional, Sequence

from survey_decode import CborError, decode_record
from survey_smp import FsMgmt, SmpSerial

STORAGE_TYPE: Final[str] = "SURVEY"
MOUNT_POINT: Final[str] = "/att_storage"

# Mirrors app/src/modules/survey/survey_store.h. Duplicated rather than queried, the same
# way survey_store.c's own sequence recovery duplicates the backend's arithmetic instead of
# calling into it -- fs_mgmt has no "tell me your block size" command, so this is the only
# place from which it can come as a plain USB export.
SLOT_SIZE: Final[int] = 816

# Mirrors app/overlay-survey.conf's CONFIG_APP_STORAGE_MAX_RECORDS_PER_TYPE for the SURVEY
# type. See app/src/modules/storage/backends/littlefs_backend.c for how this and SLOT_SIZE
# combine into a file index and an offset within it.
RECORDS_PER_TYPE: Final[int] = 25000

# The LittleFS backend computes this from fs_statvfs() at runtime; this export tool cannot
# ask the device that over fs_mgmt, so it assumes the erase block size measured and recorded
# for CP5/CP8 (app/src/modules/survey/survey_store.h): 4096 bytes, giving 5 slots of 816
# bytes per data file. If a future board revision uses a different block size, this constant
# -- and only this constant -- needs to change.
ENTRIES_PER_BLOCK: Final[int] = 4096 // SLOT_SIZE


class ExportError(Exception):
    """Raised for a device-side or verification failure that aborts the export."""


def _path_for_file_index(file_index: int) -> str:
    return f"{MOUNT_POINT}/{STORAGE_TYPE}_{file_index}.bin"


def read_header(fs: FsMgmt) -> tuple[int, int]:
    """Return (read_offset, write_offset) from the SURVEY.header file."""
    data = fs.download(f"{MOUNT_POINT}/{STORAGE_TYPE}.header")
    if len(data) != 8:
        raise ExportError(f"SURVEY.header is {len(data)} bytes, expected 8")
    read_offset, write_offset = struct.unpack("<II", data)
    if write_offset < read_offset:
        raise ExportError(
            f"SURVEY.header has write_offset={write_offset} < read_offset={read_offset} -- "
            "header looks corrupt, not just empty"
        )
    return read_offset, write_offset


def verified_download(fs: FsMgmt, path: str) -> bytes:
    """Download a file and confirm it against the device's own CRC32 before trusting it."""
    data = fs.download(path)
    expected = fs.checksum_crc32(path)
    actual = zlib.crc32(data) & 0xFFFFFFFF
    if actual != expected:
        raise ExportError(
            f"{path}: CRC32 mismatch after download (device says {expected:#010x}, "
            f"computed {actual:#010x}) -- transfer is corrupt, not just short"
        )
    return data


def iter_live_slots(
    fs: FsMgmt, read_offset: int, write_offset: int
) -> Iterator[tuple[int, bytes]]:
    """Yield the raw SLOT_SIZE-byte slot for every record between the header's offsets.

    Downloads (and CRC-verifies) each data file once, even though a file holds
    ENTRIES_PER_BLOCK records, by caching the current file's bytes across consecutive
    indices -- the same file-index arithmetic survey_store.c's recover_last_sequence()
    duplicates from the backend for the same reason: there is no indexed read.
    """
    cached_index: Optional[int] = None
    cached_bytes: bytes = b""

    for index in range(read_offset, write_offset):
        wrapped = index % RECORDS_PER_TYPE
        file_index = wrapped // ENTRIES_PER_BLOCK
        slot_index = wrapped % ENTRIES_PER_BLOCK

        if file_index != cached_index:
            cached_bytes = verified_download(fs, _path_for_file_index(file_index))
            cached_index = file_index

        start = slot_index * SLOT_SIZE
        yield index, cached_bytes[start : start + SLOT_SIZE]


def slot_to_record(slot: bytes) -> bytes:
    """Extract the encoded CBOR bytes from a raw on-flash slot, dropping the pad."""
    if len(slot) < 4:
        raise ExportError(f"slot is {len(slot)} bytes, too short to carry a length prefix")
    (length,) = struct.unpack("<I", slot[:4])
    if length == 0 or length > len(slot) - 4:
        raise ExportError(
            f"slot declares length {length}, which does not fit a {len(slot)}-byte slot"
        )
    return slot[4 : 4 + length]


def export_records(
    fs: FsMgmt, since: Optional[int], out: BinaryIO
) -> tuple[int, int, int]:
    """Write the length-prefixed record stream. Returns (seen, written, skipped_since)."""
    read_offset, write_offset = read_header(fs)
    seen = written = skipped = 0

    for index, slot in iter_live_slots(fs, read_offset, write_offset):
        seen += 1
        try:
            cbor = slot_to_record(slot)
        except ExportError as err:
            print(f"record at ring index {index}: {err}", file=sys.stderr)
            continue

        if since is not None:
            try:
                sequence = decode_record(cbor)["sequence"]
            except (CborError, ValueError, KeyError) as err:
                print(f"record at ring index {index}: cannot check sequence: {err}",
                      file=sys.stderr)
                continue
            if sequence < since:
                skipped += 1
                continue

        out.write(struct.pack("<I", len(cbor)))
        out.write(cbor)
        written += 1

    return seen, written, skipped


def main(argv: Optional[Sequence[str]] = None) -> int:
    parser = argparse.ArgumentParser(
        description="Export stored survey records over MCUmgr fs_mgmt.",
    )
    parser.add_argument("output", help="file to write the record stream to; '-' for stdout")
    parser.add_argument("--port", required=True, help="serial port, e.g. /dev/cu.usbmodem102")
    parser.add_argument("--baud", type=int, required=True, help="baud rate for --port")
    parser.add_argument("--since", type=int, default=None,
                        help="only export records with sequence >= this value")
    parser.add_argument("--stats-only", action="store_true",
                        help="report the live record count and exit without downloading records")
    args = parser.parse_args(argv)

    try:
        smp = SmpSerial(args.port, args.baud)
    except OSError as err:
        print(f"could not open {args.port}: {err}", file=sys.stderr)
        return 1

    fs = FsMgmt(smp)

    try:
        if args.stats_only:
            read_offset, write_offset = read_header(fs)
            print(f"live records: {write_offset - read_offset} "
                  f"(read_offset={read_offset}, write_offset={write_offset})")
            return 0

        try:
            out = sys.stdout.buffer if args.output == "-" else open(args.output, "wb")
        except OSError as err:
            print(f"could not open {args.output}: {err}", file=sys.stderr)
            return 1
        try:
            seen, written, skipped = export_records(fs, args.since, out)
        finally:
            if out is not sys.stdout.buffer:
                out.close()
    except ExportError as err:
        print(f"export failed: {err}", file=sys.stderr)
        return 1
    finally:
        smp.close()

    print(f"records seen: {seen}, written: {written}, skipped (--since): {skipped}",
          file=sys.stderr)
    return 0


if __name__ == "__main__":
    sys.exit(main())
