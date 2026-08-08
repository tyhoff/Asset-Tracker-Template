# Host-side tests

Tests for the host tooling in `scripts/` — currently the survey record decoder
(`scripts/survey_decode.py`, HOST-1).

## Running

No dependencies and no virtualenv: the decoder deliberately uses only the standard
library, so these run on whatever Python is on the machine.

```sh
python3 -m unittest discover -s tests/host
```

These are **not** part of `scripts/run_unit_tests.sh`, which runs the firmware's
`native_sim` suites inside Docker. The two answer different questions and have different
prerequisites.

## Fixtures are generated, not hand-written

`fixtures/*.cbor` are the exact bytes the **firmware** encoder produced, captured from the
`tests/module/survey_record` suite. That is what makes the golden-file test a cross-check
between encoder and decoder rather than two independent readings of the CDDL — if the
schema changes and only one side is updated, these tests fail.

To regenerate after a schema or encoder change:

```sh
scripts/run_unit_tests.sh tests/module/survey_record

# The suite prints delimited fixture blocks; this lifts them out.
scripts/survey_fixture.py \
    <twister-out>/native_sim_native_64/host_gnu/project/tests/module/survey_record/\
asset_tracker_template.fw.survey_record/handler.log
```

`fixtures/*.expected.json` are the decoder's output for those bytes. Regenerate them only
when a change to the output shape is intended, and **read the diff** — that file is the
record of what the dataset looks like downstream.

A fixture that changes when you did not intend it to means the encoder moved under you.
