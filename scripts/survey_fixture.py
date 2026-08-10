#!/usr/bin/env python3
#
# Copyright (c) 2026 Nordic Semiconductor ASA
#
# SPDX-License-Identifier: LicenseRef-Nordic-5-Clause

"""Extract host test fixtures from the survey_record unit test's output.

The fixtures under tests/host/fixtures/ are the exact bytes the firmware encoder
produced, which is what makes the host decoder's golden-file test a cross-check of the
encoder rather than a second reading of the CDDL. They are therefore generated, never
hand-edited.

Regenerate after any schema or encoder change:

    scripts/run_unit_tests.sh tests/module/survey_record
    scripts/survey_fixture.py <twister-out>/native_sim_native_64/host_gnu/project/\\
        tests/module/survey_record/asset_tracker_template.fw.survey_record/handler.log

Then re-run the host tests and review the diff: a fixture that changes without a
deliberate schema change means the encoder moved under you.
"""

import argparse
import pathlib
import re
import sys
from typing import Optional, Sequence

FIXTURE_RE = re.compile(
    r"-----BEGIN FIXTURE (\w+)-----\s*(.*?)\s*-----END FIXTURE \1-----",
    re.DOTALL,
)

DEFAULT_OUT = pathlib.Path(__file__).resolve().parent.parent / "tests" / "host" / "fixtures"


def main(argv: Optional[Sequence[str]] = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("handler_log", help="twister handler.log from the survey_record suite")
    parser.add_argument("-o", "--out-dir", type=pathlib.Path, default=DEFAULT_OUT,
                        help=f"where to write the fixtures (default: {DEFAULT_OUT})")
    args = parser.parse_args(argv)

    text = pathlib.Path(args.handler_log).read_text()
    found = FIXTURE_RE.findall(text)
    if not found:
        print("no fixture blocks found; did the suite run test_emit_host_fixture?",
              file=sys.stderr)
        return 1

    args.out_dir.mkdir(parents=True, exist_ok=True)

    for name, body in found:
        blob = bytes.fromhex("".join(body.split()))

        # The suffix comes from the bytes, never from a hardcoded 1. Both formats put the
        # schema version at map key 1, and getting this wrong is quiet and expensive: a
        # v2 blob written over record_v1.cbor leaves the version-dispatch tests passing
        # against a corpus that is no longer v1, which is the one thing they exist to
        # catch.
        version = _schema_version(blob)
        path = args.out_dir / f"{name}_v{version}.cbor"
        path.write_bytes(blob)
        print(f"wrote {path} ({len(blob)} bytes)")

    return 0


def _schema_version(blob: bytes) -> int:
    """Read the schema version out of an encoded record or session header."""
    sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
    from survey_decode import cbor_decode_one

    raw = cbor_decode_one(blob)
    if not isinstance(raw, dict) or 1 not in raw:
        raise ValueError("fixture has no version at map key 1")

    return raw[1]


if __name__ == "__main__":
    sys.exit(main())
