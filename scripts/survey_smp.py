"""Minimal MCUmgr SMP client over a serial transport.

This is deliberately not a general SMP library. It implements exactly the subset CP8 needs --
the fs_mgmt group's download, status and checksum commands -- and it exists for two reasons
beyond running on a laptop:

1. It is the reference for the browser client. The single-page Web Serial app in CP9 has to
   speak this same framing, and there is no mature JavaScript SMP implementation to lean on
   (mcumgr-web is Web Bluetooth only). Getting the framing right once here, under test,
   means the JS port is a translation rather than a second protocol bring-up.
2. It is the technical fallback. When the browser path breaks in the field, the answer is
   "run this script", not "wait for a fix".

Framing is the SMP console/serial encoding from the Zephyr docs: each line is base64 of a
payload prefixed with its big-endian total length and suffixed with a CRC16-XMODEM, split
across frames of at most 127 bytes and introduced by a two-byte start or continuation
marker. The same encoding is used by both transports this build enables -- SMP-over-shell on
uart0 and raw SMP on uart1 -- which is why one client can probe for whichever answers.
"""

from __future__ import annotations

import base64
import struct
from dataclasses import dataclass
from typing import Final

import serial

# Serial framing markers, from Zephyr's smp_transport spec.
FRAME_START: Final[bytes] = b"\x06\x09"
FRAME_CONT: Final[bytes] = b"\x04\x14"

# Maximum bytes on one physical line, marker included.
FRAME_MAX: Final[int] = 127

# SMP operation codes.
OP_READ: Final[int] = 0
OP_READ_RSP: Final[int] = 1
OP_WRITE: Final[int] = 2
OP_WRITE_RSP: Final[int] = 3

# Management group and command ids used here.
GROUP_OS: Final[int] = 0
CMD_OS_ECHO: Final[int] = 0
GROUP_FS: Final[int] = 8
CMD_FS_FILE: Final[int] = 0
CMD_FS_STATUS: Final[int] = 1
CMD_FS_CHECKSUM: Final[int] = 2

SMP_HEADER_LEN: Final[int] = 8


class SmpError(Exception):
    """A protocol-level failure: bad framing, a CRC mismatch, or a non-zero rc."""


def crc16_xmodem(data: bytes) -> int:
    """CRC16 with polynomial 0x1021 and zero initial value, as SMP serial framing uses."""
    crc = 0
    for byte in data:
        crc ^= byte << 8
        for _ in range(8):
            crc = ((crc << 1) ^ 0x1021) & 0xFFFF if crc & 0x8000 else (crc << 1) & 0xFFFF
    return crc


def encode_frames(packet: bytes) -> list[bytes]:
    """Wrap one SMP packet into the newline-terminated base64 frames the device expects.

    The length prefix counts the packet plus its CRC, not the base64 expansion, and it
    appears once for the whole packet rather than once per frame -- which is why a truncated
    transfer is detectable rather than merely short.
    """
    body = struct.pack(">H", len(packet) + 2) + packet
    body += struct.pack(">H", crc16_xmodem(packet))
    encoded = base64.b64encode(body)

    frames: list[bytes] = []
    marker = FRAME_START
    pos = 0
    while pos < len(encoded):
        # Budget: marker (2) + payload + newline (1), all within FRAME_MAX. Base64 is only
        # splittable on 4-character boundaries without re-encoding, hence the rounding.
        room = ((FRAME_MAX - 3) // 4) * 4
        chunk = encoded[pos : pos + room]
        frames.append(marker + chunk + b"\n")
        marker = FRAME_CONT
        pos += len(chunk)
    return frames


def decode_frames(lines: list[bytes]) -> bytes:
    """Reassemble base64 frames into the SMP packet, verifying the length and CRC."""
    encoded = b""
    for index, line in enumerate(lines):
        marker = line[:2]
        expected = FRAME_START if index == 0 else FRAME_CONT
        if marker != expected:
            raise SmpError(f"frame {index} has marker {marker!r}, expected {expected!r}")
        encoded += line[2:].strip()

    body = base64.b64decode(encoded)
    if len(body) < 4:
        raise SmpError(f"reassembled body is {len(body)} bytes, too short to carry a CRC")

    declared = struct.unpack(">H", body[:2])[0]
    payload = body[2:]
    if declared != len(payload):
        raise SmpError(f"length prefix says {declared} bytes, got {len(payload)}")

    packet, crc = payload[:-2], struct.unpack(">H", payload[-2:])[0]
    actual = crc16_xmodem(packet)
    if crc != actual:
        raise SmpError(f"CRC mismatch: frame says {crc:#06x}, computed {actual:#06x}")
    return packet


@dataclass(frozen=True)
class SmpResponse:
    """One decoded SMP response: its header fields and the raw CBOR payload."""

    op: int
    group: int
    sequence: int
    command: int
    payload: bytes


def build_packet(op: int, group: int, command: int, sequence: int, payload: bytes) -> bytes:
    """Assemble the 8-byte SMP header and its CBOR body."""
    return (
        struct.pack(
            ">BBHHBB",
            op,
            0,  # flags; unused by every command here
            len(payload),
            group,
            sequence,
            command,
        )
        + payload
    )


def parse_packet(packet: bytes) -> SmpResponse:
    """Split a received packet into header fields and payload, checking the declared length."""
    if len(packet) < SMP_HEADER_LEN:
        raise SmpError(f"packet is {len(packet)} bytes, shorter than an SMP header")

    op, _flags, length, group, sequence, command = struct.unpack(
        ">BBHHBB", packet[:SMP_HEADER_LEN]
    )
    payload = packet[SMP_HEADER_LEN:]
    if length != len(payload):
        raise SmpError(f"header declares {length} payload bytes, got {len(payload)}")
    return SmpResponse(op=op, group=group, sequence=sequence, command=command, payload=payload)


class SmpSerial:
    """An SMP session over one serial port.

    Sequence numbers are checked rather than ignored. On a port shared with the shell, log
    output and command echoes arrive interleaved with responses, so a client that accepted
    the first thing that decoded would happily pair a response with the wrong request -- and
    during a 5,000-file download that misalignment would corrupt the output silently.
    """

    def __init__(self, port: str, baudrate: int, timeout: float = 5.0) -> None:
        self.ser: serial.Serial = serial.Serial(port, baudrate, timeout=timeout)
        self._sequence: int = 0

    def close(self) -> None:
        self.ser.close()

    def _next_sequence(self) -> int:
        sequence = self._sequence
        self._sequence = (self._sequence + 1) % 256
        return sequence

    def request(self, op: int, group: int, command: int, payload: bytes) -> SmpResponse:
        """Send one command and return its matching response.

        Lines that are not SMP frames are discarded, which is what makes this work on the
        shell port: boot banners, log lines and the prompt all fail the marker check and are
        skipped rather than treated as a malformed response.
        """
        sequence = self._next_sequence()
        packet = build_packet(op, group, command, sequence, payload)

        self.ser.reset_input_buffer()
        for frame in encode_frames(packet):
            self.ser.write(frame)
        self.ser.flush()

        collected: list[bytes] = []
        while True:
            line = self.ser.readline()
            if not line:
                raise SmpError(
                    f"timed out waiting for a response to group {group} command {command}"
                )

            if not collected:
                if not line.startswith(FRAME_START):
                    continue  # shell noise, not our response
                collected = [line]
            else:
                collected.append(line)

            try:
                response = parse_packet(decode_frames(collected))
            except SmpError:
                # Either the packet is incomplete and the next line continues it, or the
                # frames were noise. Continuation lines carry the marker; anything else means
                # the collection is unsalvageable and should restart.
                if collected[-1].startswith(FRAME_CONT) or len(collected) == 1:
                    continue
                collected = []
                continue

            if response.sequence != sequence:
                collected = []
                continue
            return response
