#!/usr/bin/env python3
#
# Copyright (c) 2026 Nordic Semiconductor ASA
#
# SPDX-License-Identifier: LicenseRef-Nordic-5-Clause

"""Tests for the CP8 export tool (scripts/survey_export.py).

FakeFs below fakes FsMgmt itself, one layer up from FakeSmpSerial in test_survey_smp.py:
survey_export.py's functions only ever call .download()/.checksum_crc32(), so a fake at
that level exercises the ring-index arithmetic, slot decoding and CRC verification without
re-driving SMP framing already covered by test_survey_smp.py.

Run with:  python3 -m unittest discover -s tests/host
"""

import io
import pathlib
import struct
import sys
import unittest
import zlib

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent.parent.parent / "scripts"))

from survey_export import (  # noqa: E402
    ENTRIES_PER_BLOCK,
    MOUNT_POINT,
    SLOT_SIZE,
    STORAGE_TYPE,
    ExportError,
    export_records,
    iter_live_slots,
    read_header,
    slot_to_record,
    verified_download,
)

FIXTURES = pathlib.Path(__file__).resolve().parent / "fixtures"


def load_fixture(name: str) -> bytes:
    return (FIXTURES / f"{name}_v1.cbor").read_bytes()


def pack_slot(cbor: bytes) -> bytes:
    """Build one on-flash slot: a 4-byte length prefix, the CBOR, then zero pad to SLOT_SIZE."""
    body = struct.pack("<I", len(cbor)) + cbor
    if len(body) > SLOT_SIZE:
        raise ValueError(f"record is {len(body)} bytes, too big for a {SLOT_SIZE}-byte slot")
    return body + b"\x00" * (SLOT_SIZE - len(body))


def pack_file(slots: list) -> bytes:
    return b"".join(slots)


class FakeFs:
    """Stands in for FsMgmt: survey_export.py's functions only call .download() and
    .checksum_crc32(), so this needs no SmpSerial, no framing and no fs_mgmt CBOR -- just an
    in-memory path -> bytes map.
    """

    def __init__(self) -> None:
        self.files: dict = {}
        self.download_counts: dict = {}
        self.corrupt_paths: set = set()

    def put(self, path: str, data: bytes) -> None:
        self.files[path] = data

    def download(self, path: str) -> bytes:
        self.download_counts[path] = self.download_counts.get(path, 0) + 1
        return self.files[path]

    def checksum_crc32(self, path: str) -> int:
        if path in self.corrupt_paths:
            return 0xBAADF00D
        return zlib.crc32(self.files[path]) & 0xFFFFFFFF


class TestReadHeader(unittest.TestCase):
    def test_reads_offsets(self) -> None:
        fs = FakeFs()
        fs.put(f"{MOUNT_POINT}/{STORAGE_TYPE}.header", struct.pack("<II", 3, 9))
        self.assertEqual(read_header(fs), (3, 9))

    def test_wrong_length_is_an_error(self) -> None:
        fs = FakeFs()
        fs.put(f"{MOUNT_POINT}/{STORAGE_TYPE}.header", b"\x00\x00\x00")
        with self.assertRaises(ExportError):
            read_header(fs)

    def test_write_offset_before_read_offset_is_an_error(self) -> None:
        fs = FakeFs()
        fs.put(f"{MOUNT_POINT}/{STORAGE_TYPE}.header", struct.pack("<II", 9, 3))
        with self.assertRaises(ExportError):
            read_header(fs)


class TestVerifiedDownload(unittest.TestCase):
    def test_matching_checksum_returns_data(self) -> None:
        fs = FakeFs()
        fs.put("/att_storage/SURVEY_0.bin", b"hello world")
        self.assertEqual(verified_download(fs, "/att_storage/SURVEY_0.bin"), b"hello world")

    def test_checksum_mismatch_is_an_error(self) -> None:
        fs = FakeFs()
        fs.put("/att_storage/SURVEY_0.bin", b"hello world")
        fs.corrupt_paths.add("/att_storage/SURVEY_0.bin")
        with self.assertRaises(ExportError):
            verified_download(fs, "/att_storage/SURVEY_0.bin")


class TestSlotToRecord(unittest.TestCase):
    def test_extracts_the_cbor_and_drops_the_pad(self) -> None:
        record = load_fixture("record")
        self.assertEqual(slot_to_record(pack_slot(record)), record)

    def test_too_short_for_a_length_prefix_is_an_error(self) -> None:
        with self.assertRaises(ExportError):
            slot_to_record(b"\x01\x02\x03")

    def test_zero_length_is_an_error(self) -> None:
        with self.assertRaises(ExportError):
            slot_to_record(struct.pack("<I", 0) + b"\x00" * (SLOT_SIZE - 4))

    def test_declared_length_past_end_of_slot_is_an_error(self) -> None:
        with self.assertRaises(ExportError):
            slot_to_record(struct.pack("<I", SLOT_SIZE) + b"\x00" * (SLOT_SIZE - 4))


class TestIterLiveSlots(unittest.TestCase):
    def test_downloads_each_file_at_most_once(self) -> None:
        record = load_fixture("record")
        slot = pack_slot(record)
        file0 = pack_file([slot] * ENTRIES_PER_BLOCK)
        file1 = pack_file([slot] * ENTRIES_PER_BLOCK)

        fs = FakeFs()
        fs.put(f"{MOUNT_POINT}/{STORAGE_TYPE}_0.bin", file0)
        fs.put(f"{MOUNT_POINT}/{STORAGE_TYPE}_1.bin", file1)

        # read_offset=3, write_offset=8 spans slots 3,4 of file 0 and slots 0,1,2 of file 1.
        found = list(iter_live_slots(fs, 3, 8))
        self.assertEqual([index for index, _ in found], [3, 4, 5, 6, 7])
        for _, slot_bytes in found:
            self.assertEqual(slot_to_record(slot_bytes), record)

        self.assertEqual(fs.download_counts[f"{MOUNT_POINT}/{STORAGE_TYPE}_0.bin"], 1)
        self.assertEqual(fs.download_counts[f"{MOUNT_POINT}/{STORAGE_TYPE}_1.bin"], 1)

    def test_empty_range_yields_nothing(self) -> None:
        fs = FakeFs()
        self.assertEqual(list(iter_live_slots(fs, 5, 5)), [])


class TestExportRecords(unittest.TestCase):
    def _fs_with_one_file(self, record: bytes) -> "FakeFs":
        slot = pack_slot(record)
        fs = FakeFs()
        fs.put(f"{MOUNT_POINT}/{STORAGE_TYPE}.header", struct.pack("<II", 0, 3))
        fs.put(f"{MOUNT_POINT}/{STORAGE_TYPE}_0.bin", pack_file([slot] * ENTRIES_PER_BLOCK))
        return fs

    def test_writes_every_record_when_since_is_none(self) -> None:
        record = load_fixture("record")
        fs = self._fs_with_one_file(record)
        out = io.BytesIO()

        seen, written, skipped = export_records(fs, None, out)

        self.assertEqual((seen, written, skipped), (3, 3, 0))
        expected = (struct.pack("<I", len(record)) + record) * 3
        self.assertEqual(out.getvalue(), expected)

    def test_since_at_or_below_sequence_keeps_the_record(self) -> None:
        # The fixture record's sequence is 42 (tests/host/fixtures/record_v1.expected.json).
        fs = self._fs_with_one_file(load_fixture("record"))
        out = io.BytesIO()

        seen, written, skipped = export_records(fs, 42, out)

        self.assertEqual((seen, written, skipped), (3, 3, 0))

    def test_since_above_sequence_skips_the_record(self) -> None:
        fs = self._fs_with_one_file(load_fixture("record"))
        out = io.BytesIO()

        seen, written, skipped = export_records(fs, 43, out)

        self.assertEqual((seen, written, skipped), (3, 0, 3))
        self.assertEqual(out.getvalue(), b"")

    def test_corrupt_slot_is_skipped_not_fatal(self) -> None:
        good = pack_slot(load_fixture("record"))
        bad = struct.pack("<I", 0) + b"\x00" * (SLOT_SIZE - 4)
        fs = FakeFs()
        fs.put(f"{MOUNT_POINT}/{STORAGE_TYPE}.header", struct.pack("<II", 0, 2))
        fs.put(f"{MOUNT_POINT}/{STORAGE_TYPE}_0.bin", pack_file([good, bad] + [good] * 3))
        out = io.BytesIO()

        seen, written, skipped = export_records(fs, None, out)

        self.assertEqual((seen, written, skipped), (2, 1, 0))


if __name__ == "__main__":
    unittest.main()
