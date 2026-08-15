#!/usr/bin/env python3
#
# Copyright (c) 2026 Nordic Semiconductor ASA
#
# SPDX-License-Identifier: LicenseRef-Nordic-5-Clause

"""Tests for the host-side SMP client (scripts/survey_smp.py).

No real serial port and no pyserial import required: SmpSerial defers `import serial` to
its own __init__ specifically so this module -- framing, the fs_mgmt CBOR encode/decode,
and FsMgmt driven by a fake in place of a real SmpSerial -- stays testable under the same
no-third-party-dependencies policy as scripts/survey_decode.py.

Run with:  python3 -m unittest discover -s tests/host
"""

import pathlib
import sys
import unittest

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent.parent.parent / "scripts"))

from survey_smp import (  # noqa: E402
    FRAME_CONT,
    FRAME_START,
    CMD_FS_CHECKSUM,
    CMD_FS_FILE,
    CMD_FS_STATUS,
    GROUP_FS,
    OP_READ,
    OP_READ_RSP,
    FsMgmt,
    SmpError,
    SmpResponse,
    build_packet,
    cbor_decode_map,
    cbor_encode_map,
    crc16_xmodem,
    decode_frames,
    encode_frames,
    parse_packet,
)


class TestCrc16(unittest.TestCase):
    def test_known_vector(self) -> None:
        # CRC16-XMODEM (poly 0x1021, init 0) of "123456789" is a standard reference value.
        self.assertEqual(crc16_xmodem(b"123456789"), 0x31C3)

    def test_empty_input(self) -> None:
        self.assertEqual(crc16_xmodem(b""), 0)


class TestFraming(unittest.TestCase):
    def test_round_trips_a_short_packet(self) -> None:
        packet = build_packet(OP_READ, GROUP_FS, CMD_FS_STATUS, 7, b"\x01\x02\x03")
        frames = encode_frames(packet)
        self.assertEqual(len(frames), 1)
        self.assertTrue(frames[0].startswith(FRAME_START))
        self.assertEqual(decode_frames(frames), packet)

    def test_splits_a_long_packet_across_frames(self) -> None:
        packet = build_packet(OP_READ, GROUP_FS, CMD_FS_FILE, 3, b"x" * 500)
        frames = encode_frames(packet)
        self.assertGreater(len(frames), 1)
        self.assertTrue(frames[0].startswith(FRAME_START))
        for frame in frames[1:]:
            self.assertTrue(frame.startswith(FRAME_CONT))
        self.assertEqual(decode_frames(frames), packet)

    def test_wrong_start_marker_is_an_error(self) -> None:
        with self.assertRaises(SmpError):
            decode_frames([FRAME_CONT + b"AAAA\n"])

    def test_crc_mismatch_is_detected(self) -> None:
        packet = build_packet(OP_READ, GROUP_FS, CMD_FS_STATUS, 1, b"data")
        frames = encode_frames(packet)
        # Flip a bit inside the base64 body, after the marker, to corrupt the CRC without
        # touching the framing itself.
        corrupted = frames[0][:4] + bytes([frames[0][4] ^ 0x01]) + frames[0][5:]
        with self.assertRaises(SmpError):
            decode_frames([corrupted])


class TestPacketHeader(unittest.TestCase):
    def test_round_trips_header_fields(self) -> None:
        packet = build_packet(OP_READ_RSP, GROUP_FS, CMD_FS_CHECKSUM, 42, b"\xaa\xbb")
        response = parse_packet(packet)
        self.assertEqual(response.op, OP_READ_RSP)
        self.assertEqual(response.group, GROUP_FS)
        self.assertEqual(response.sequence, 42)
        self.assertEqual(response.command, CMD_FS_CHECKSUM)
        self.assertEqual(response.payload, b"\xaa\xbb")

    def test_short_packet_is_an_error(self) -> None:
        with self.assertRaises(SmpError):
            parse_packet(b"\x00\x00\x00")

    def test_length_mismatch_is_an_error(self) -> None:
        packet = build_packet(OP_READ, GROUP_FS, CMD_FS_STATUS, 0, b"data")
        # Truncate the payload without correcting the header's declared length.
        with self.assertRaises(SmpError):
            parse_packet(packet[:-1])


class TestCborMap(unittest.TestCase):
    def test_round_trips_str_int_and_bytes_values(self) -> None:
        encoded = cbor_encode_map({"name": "/att_storage/SURVEY.header", "off": 1024})
        self.assertEqual(
            cbor_decode_map(encoded), {"name": "/att_storage/SURVEY.header", "off": 1024}
        )

    def test_encodes_a_byte_string_value(self) -> None:
        encoded = cbor_encode_map({"data": b"\x00\x01\x02"})
        self.assertEqual(cbor_decode_map(encoded), {"data": b"\x00\x01\x02"})

    def test_uint_widths_round_trip(self) -> None:
        for value in (0, 23, 24, 255, 256, 65535, 65536, 2**32 - 1, 2**32):
            with self.subTest(value=value):
                self.assertEqual(cbor_decode_map(cbor_encode_map({"n": value})), {"n": value})

    def test_decode_rejects_a_non_map(self) -> None:
        with self.assertRaises(SmpError):
            cbor_decode_map(b"\x01")

    def test_decode_rejects_trailing_bytes(self) -> None:
        encoded = cbor_encode_map({"n": 1})
        with self.assertRaises(SmpError):
            cbor_decode_map(encoded + b"\x00")


class FakeSmpSerial:
    """Stands in for SmpSerial: FsMgmt only calls .request(), so this needs no port, no
    framing and no pyserial -- just canned fs_mgmt responses keyed by command.
    """

    def __init__(self) -> None:
        self.requests: list[tuple[int, int, int, bytes]] = []
        self.responses: dict[int, list[bytes]] = {}

    def queue(self, command: int, payload: bytes) -> None:
        self.responses.setdefault(command, []).append(payload)

    def request(self, op: int, group: int, command: int, payload: bytes) -> SmpResponse:
        self.requests.append((op, group, command, payload))
        queued = self.responses[command].pop(0)
        return SmpResponse(op=op + 1, group=group, sequence=0, command=command, payload=queued)


class TestFsMgmt(unittest.TestCase):
    def test_stat_returns_length(self) -> None:
        fake = FakeSmpSerial()
        fake.queue(CMD_FS_STATUS, cbor_encode_map({"len": 8}))
        self.assertEqual(FsMgmt(fake).stat("/att_storage/SURVEY.header"), 8)

    def test_stat_raises_on_nonzero_rc(self) -> None:
        fake = FakeSmpSerial()
        fake.queue(CMD_FS_STATUS, cbor_encode_map({"rc": 5}))
        with self.assertRaises(SmpError):
            FsMgmt(fake).stat("/att_storage/missing.bin")

    def test_checksum_crc32_returns_output(self) -> None:
        fake = FakeSmpSerial()
        fake.queue(
            CMD_FS_CHECKSUM,
            cbor_encode_map({"type": "crc32", "len": 4080, "output": 0xDEADBEEF}),
        )
        self.assertEqual(FsMgmt(fake).checksum_crc32("/att_storage/SURVEY_0.bin"), 0xDEADBEEF)

    def test_download_reassembles_multiple_chunks(self) -> None:
        fake = FakeSmpSerial()
        fake.queue(CMD_FS_FILE, cbor_encode_map({"off": 0, "data": b"AAAA", "len": 8}))
        fake.queue(CMD_FS_FILE, cbor_encode_map({"off": 4, "data": b"BBBB"}))
        self.assertEqual(FsMgmt(fake).download("/att_storage/SURVEY_0.bin"), b"AAAABBBB")
        # Confirms the offset walks forward request-to-request rather than re-reading 0.
        offsets = [cbor_decode_map(req[3])["off"] for req in fake.requests]
        self.assertEqual(offsets, [0, 4])

    def test_download_empty_file(self) -> None:
        fake = FakeSmpSerial()
        fake.queue(CMD_FS_FILE, cbor_encode_map({"off": 0, "data": b"", "len": 0}))
        self.assertEqual(FsMgmt(fake).download("/att_storage/SURVEY.header"), b"")

    def test_download_raises_on_empty_chunk_before_end(self) -> None:
        fake = FakeSmpSerial()
        fake.queue(CMD_FS_FILE, cbor_encode_map({"off": 0, "data": b"", "len": 8}))
        with self.assertRaises(SmpError):
            FsMgmt(fake).download("/att_storage/SURVEY_0.bin")


if __name__ == "__main__":
    unittest.main()
