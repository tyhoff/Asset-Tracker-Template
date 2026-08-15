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
from typing import TYPE_CHECKING, Final, Union

if TYPE_CHECKING:
    import serial

CborScalar = Union[int, str, bytes]

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
        # Deferred so that everything else in this module -- the framing, the fs_mgmt CBOR
        # encode/decode, and FsMgmt driven by a fake in place of a real SmpSerial -- stays
        # importable and unit-testable without pyserial installed, matching tests/host's
        # no-third-party-dependencies policy. Only opening a real port needs it.
        import serial

        self.ser: "serial.Serial" = serial.Serial(port, baudrate, timeout=timeout)
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


# --- Minimal CBOR for fs_mgmt request/response maps ---------------------------------------
#
# fs_mgmt's own maps are shallow: text-string keys, and values that are only unsigned
# integers, text strings or byte strings. A general decoder (survey_decode.py already has
# one, for the record schema) would be the wrong tool here -- it would accept containers
# and negative integers that fs_mgmt never sends, silently widening what this client treats
# as well-formed.


def _cbor_encode_uint(value: int) -> bytes:
    if value < 24:
        return bytes([value])
    if value < 256:
        return bytes([24, value])
    if value < 65536:
        return bytes([25]) + struct.pack(">H", value)
    if value < 2**32:
        return bytes([26]) + struct.pack(">I", value)
    return bytes([27]) + struct.pack(">Q", value)


def _cbor_encode_head(major: int, value: int) -> bytes:
    """Encode a head for a major type whose argument is a length or a uint value."""
    head = _cbor_encode_uint(value)
    return bytes([(major << 5) | head[0]]) + head[1:]


def cbor_encode_map(fields: dict[str, CborScalar]) -> bytes:
    """Encode a definite-length map with text-string keys, matching what fs_mgmt decodes."""
    out = _cbor_encode_head(5, len(fields))
    for key, value in fields.items():
        key_bytes = key.encode("utf-8")
        out += _cbor_encode_head(3, len(key_bytes)) + key_bytes
        if isinstance(value, str):
            value_bytes = value.encode("utf-8")
            out += _cbor_encode_head(3, len(value_bytes)) + value_bytes
        elif isinstance(value, bytes):
            out += _cbor_encode_head(2, len(value)) + value
        elif isinstance(value, int):
            out += _cbor_encode_head(0, value)
        else:
            raise TypeError(f"unsupported CBOR value type for key {key!r}: {type(value)}")
    return out


def _cbor_decode_item(data: bytes, pos: int) -> tuple[CborScalar | dict, int]:
    initial = data[pos]
    major = initial >> 5
    minor = initial & 0x1F
    pos += 1

    if minor < 24:
        arg = minor
    elif minor in (24, 25, 26, 27):
        width = 1 << (minor - 24)
        arg = int.from_bytes(data[pos : pos + width], "big")
        pos += width
    else:
        raise SmpError(f"unsupported CBOR additional-information {minor} at offset {pos - 1}")

    if major == 0:
        return arg, pos
    if major == 2:
        return bytes(data[pos : pos + arg]), pos + arg
    if major == 3:
        return data[pos : pos + arg].decode("utf-8"), pos + arg
    if major == 5:
        result: dict[str, CborScalar] = {}
        for _ in range(arg):
            key, pos = _cbor_decode_item(data, pos)
            if not isinstance(key, str):
                raise SmpError(f"map key at offset {pos} is not a text string")
            value, pos = _cbor_decode_item(data, pos)
            result[key] = value
        return result, pos

    raise SmpError(f"unsupported CBOR major type {major} in an fs_mgmt response")


def cbor_decode_map(data: bytes) -> dict[str, CborScalar]:
    """Decode one definite-length top-level map, as every fs_mgmt response is."""
    value, pos = _cbor_decode_item(data, 0)
    if not isinstance(value, dict):
        raise SmpError(f"expected a CBOR map at the top level, got {type(value).__name__}")
    if pos != len(data):
        raise SmpError(f"{len(data) - pos} trailing byte(s) after the top-level map")
    return value


def _check_rc(response: dict[str, CborScalar], context: str) -> None:
    rc = response.get("rc")
    if isinstance(rc, int) and rc != 0:
        raise SmpError(f"{context}: device returned rc={rc}")


class FsMgmt:
    """fs_mgmt group commands: file download, status and checksum. Read-only by design --
    there is no upload method here, matching survey_export.c's deny-by-default policy.
    """

    def __init__(self, smp: SmpSerial) -> None:
        self.smp = smp

    def stat(self, path: str) -> int:
        """Return a file's length in bytes, via the fs_mgmt "stat" command."""
        request = cbor_encode_map({"name": path})
        response = self.smp.request(OP_READ, GROUP_FS, CMD_FS_STATUS, request)
        result = cbor_decode_map(response.payload)
        _check_rc(result, f"fs stat {path}")
        length = result.get("len")
        if not isinstance(length, int):
            raise SmpError(f"fs stat {path}: response has no numeric 'len'")
        return length

    def checksum_crc32(self, path: str) -> int:
        """Return the IEEE CRC32 (zlib.crc32 convention) of a whole file."""
        request = cbor_encode_map({"name": path, "type": "crc32"})
        response = self.smp.request(OP_READ, GROUP_FS, CMD_FS_CHECKSUM, request)
        result = cbor_decode_map(response.payload)
        _check_rc(result, f"fs checksum {path}")
        output = result.get("output")
        if not isinstance(output, int):
            raise SmpError(f"fs checksum {path}: response has no numeric 'output'")
        return output

    def download(self, path: str) -> bytes:
        """Download a whole file, one MCUMGR_GRP_FS_DL_CHUNK_SIZE chunk per request.

        The total length is only carried in the response to the offset-0 request, so it is
        read from there rather than from a separate "stat" round trip.
        """
        chunks: list[bytes] = []
        offset = 0
        total: int | None = None

        while total is None or offset < total:
            request = cbor_encode_map({"name": path, "off": offset})
            response = self.smp.request(OP_READ, GROUP_FS, CMD_FS_FILE, request)
            result = cbor_decode_map(response.payload)
            _check_rc(result, f"fs download {path} at offset {offset}")

            data = result.get("data")
            if not isinstance(data, bytes):
                raise SmpError(f"fs download {path}: response has no byte-string 'data'")

            if offset == 0:
                length = result.get("len")
                if not isinstance(length, int):
                    raise SmpError(f"fs download {path}: first response has no numeric 'len'")
                total = length

            if not data and offset < total:
                raise SmpError(
                    f"fs download {path}: empty chunk at offset {offset} of {total}"
                )

            chunks.append(data)
            offset += len(data)

        return b"".join(chunks)
