# Requirements: Thingy:91 X radio-survey data exporter

## Context

**Why:** We need to quantify how accurate nRF Cloud Location Services is when resolving position
from cell and Wi-Fi observations, measured against GNSS ground truth — and to be able to feed the
same observations to other location providers for comparison. Today that data is collected with an
Android phone, which is a poor instrument for it: it exposes one serving cell with a throttled
neighbor list, applies fused-location processing that contaminates ground truth, and cannot be
told to perform a deliberate multi-cell search. It measures a phone vendor's stack, not ours.

**What we're building:** a fork of the Asset Tracker Template that turns a Thingy:91 X into a
**radio-survey data exporter**. Every cycle it captures a GNSS fix together with the cell and Wi-Fi
observations visible at that moment, stores the record locally, and dumps the batch to a host over
USB when convenient.

**Scope boundary:** the firmware is a **capture and export device only**. It must never resolve
position from cell/Wi-Fi and must never call ground-fix. Analysis, provider comparison, and any
conversion to other formats happen off-device, on hosts or servers, from the exported dataset.

**Deferred:** LTE/CoAP live upload, on-device location resolution, Web Bluetooth, 5G NR.

### Design principles

1. **Reuse Nordic nomenclature.** Field names come from the nRF Cloud ground-fix API
   (<https://api-docs.nrfcloud.com/>). Do not invent a parallel vocabulary.
2. **Reuse what the template already has.** The storage module, its batch-session protocol, the
   location module, the CBOR helpers, and the LED/watchdog infrastructure all exist. Extend them.
3. **The on-device format is ours to choose** — it only has to be compact and unambiguous. Host
   scripts convert to whatever a consumer needs. But it **must carry a schema version** so future
   readers know how to interpret and migrate it.

---

## Development environment

The repository is **not** a west workspace on its own. The SDK lives in a sibling workspace so this
repo never moves:

| Thing | Value |
|---|---|
| NCS version | **v3.4.0** (matches the `nrf` revision pinned in `west.yml`) |
| Toolchain | installed via `nrfutil toolchain-manager install --ncs-version v3.4.0` |
| SDK workspace | `/Users/tyler/junk/ncs-3.4.0` (follows the existing `ncs-<version>` convention) |
| Board target | `thingy91x/nrf9151/ns` |

Note: macOS is case-insensitive, so the workspace name `asset-tracker-template` used in
`docs/common/getting_started.md` collides with this repo's own directory name. Hence the
`ncs-3.4.0` sibling layout and out-of-tree app builds.

Clone the SDK shallow — it is vastly faster and sufficient for building:

```shell
west init -m https://github.com/nrfconnect/sdk-nrf --mr v3.4.0 /Users/tyler/junk/ncs-3.4.0
cd /Users/tyler/junk/ncs-3.4.0 && west update --narrow -o=--depth=1
```

Build (out-of-tree app, from inside the SDK workspace):

```shell
cd /Users/tyler/junk/ncs-3.4.0
nrfutil toolchain-manager launch --ncs-version v3.4.0 -- \
  west build -b thingy91x/nrf9151/ns --sysbuild /Users/tyler/junk/Asset-Tracker-Template/app
```

### Capture priority (from stakeholder)

1. **Wi-Fi APs** — highest value; most customers rely on Wi-Fi, not multi-cell.
2. **Serving cell + neighbors** (light measurement) — cheap, always on.
3. **A few full-identity GCI multi-cell samples** — expensive in time and power; only when
   slow/stationary *and* on external power.
4. **GNSS with every sample** — non-negotiable; it is the ground truth.

---

## Terminology (use these names everywhere)

Cell observations, matching the ground-fix `lte[]` array:

| Field | Meaning | Scope |
|---|---|---|
| `mcc`, `mnc` | Mobile country / network code | identified cells |
| `eci` | E-UTRAN cell ID | identified cells |
| `tac` | Tracking area code | identified cells |
| `earfcn` | Carrier frequency | all cells |
| `pci` | Physical cell ID | all cells |
| `rsrp`, `rsrq` | Signal power / quality | all cells |
| `adv` | Timing advance — **not engineered for**, see FW-3; usually absent | serving cell only |
| `nmr[]` | Neighbor measurements — `pci`, `earfcn`, `rsrp`, `rsrq`, `timeDiff` | neighbors |

Wi-Fi observations, matching `wifi.accessPoints[]`: `macAddress`, `signalStrength`, `channel`,
`frequency`. **`ssid` is deliberately NOT captured** — not worth the storage. `band` is added
(not a ground-fix field, but cheap and useful for analysis).

Position, matching nRF Cloud GNSS device-message naming: `lat`, `lon`, `acc`, `alt`, `spd`, `hdg`.

---

## Firmware requirements

Base: this repository, a fork of the Asset Tracker Template (Zephyr / nRF Connect SDK, zbus + SMF
modules). All paths below are repo-relative. Target board: `thingy91x_nrf9151_ns`.

### FW-1 — Extend the location data contract (blocking gap)

`app/src/modules/location/location.h` drops fields we need:

- **`struct location_wifi_ap_info`** has only `rssi`, `mac`, `mac_length`. Add **`channel`**,
  **`frequency`** (MHz), and **`band`**. (Do not add SSID.)
- **`struct location_cell_info`** — used for the serving cell *and* every entry of `gci_cells[]` —
  has no PCI. Add **`phys_cell_id`**. (`location_neighbor_cell_info` already has it.)

Plumb both through `location.c`'s event handler and the copy logic in
`app/src/modules/cloud/cloud_location.c:23-83` (`cellular_cell_data_construct`,
`wifi_ap_data_construct`), which currently drops the same fields.

`struct location_cloud_request_data` already carries `current_cell`, `neighbor_cells[]`,
`gci_cells[]`, and `wifi_aps[]` — **reuse it as the capture payload.** Do not define a parallel
struct.

Existing caps are acceptable and need **no change**: `CONFIG_APP_LOCATION_WIFI_APS_MAX`=10,
`CONFIG_APP_LOCATION_NEIGHBOR_CELLS_MAX`=10, `CONFIG_NRF_WIFI_SCAN_MAX_BSS_CNT`=10,
`CONFIG_LOCATION_METHOD_WIFI_SCANNING_RESULTS_MAX_CNT`=10. Note in docs that dense urban
environments will exceed 10 visible APs and be truncated — an accepted tradeoff.

### FW-2 — Two capture profiles

`CONFIG_LOCATION_SERVICE_EXTERNAL=y` is already set (`prj.conf:217`), which makes the Location
library hand raw cell + Wi-Fi data back to the app as a `LOCATION_CLOUD_REQUEST` message instead of
resolving it. **That is the capture hook.**

Critically, the Location library **stops at the first successful method** — so a default
GNSS-first request returns a fix and the cell/Wi-Fi scan never happens. Each cycle must issue
**separate, sequenced requests**. `LOCATION_GNSS_SEARCH_TRIGGER` already exists in `location.h`
for a GNSS-only request — reuse it.

**Profile FAST** (default; moving or on battery):
1. GNSS-only fix → timestamp
2. Wi-Fi scan + **light** cell measurement (no GCI) → scan start/end timestamps
3. GNSS-only fix → timestamp *(bracket; see FW-4)*

**Profile DEEP** (only when slow/stationary **and** on external power): same, but the cell step uses
**GCI multi-cell search** (`gci_count` > 1) to obtain full-identity neighbors. This is the
10–40 s, power-hungry path — hence the gating.

Requirements:
- The modem **cannot** run GNSS and LTE simultaneously (single RF front end). Steps must be
  strictly sequenced, never overlapped. The Wi-Fi scan is on the separate nRF7002 and *may* overlap
  the cell step.
- Every record is **tagged with the profile used**, so analysis can separate the populations.
- DEEP gating is Kconfig-tunable: a speed threshold (from GNSS `spd`) plus external-power
  detection. Check how VBUS/charger state is exposed in `app/src/modules/power/`; if it isn't,
  fall back to a shell/Kconfig toggle rather than blocking this work.
- A shell command must force either profile on demand for bench testing.
- **Do not use `LOCATION_SEARCH_CANCEL`** — `location.h` documents that it cannot truly cancel
  Wi-Fi scans and causes `-EBUSY` on subsequent requests.

### FW-3 — Timing advance (`adv`) — not a goal

`adv` is measured **only in RRC-connected state**; in idle/PSM the modem reports
`LTE_LC_CELL_TIMING_ADVANCE_INVALID` (65535). Obtaining it reliably would mean holding the modem
connected around every cell measurement, which costs power we would rather spend on capture rate.

**Decision: do not engineer for `adv`.** No connected-state mode, no gating, no verification
requirement.

The field already exists in `struct location_cell_info` and is already copied at
`cloud_location.c:53`, so pass it through when the modem happens to report a valid value (zero
additional cost) and **omit it** otherwise. Never store 65535 or 0 as though it were a measurement.
Analysis must treat `adv` as usually-absent.

### FW-4 — Timestamps and motion (scientific-validity requirement)

At 70 mph a vehicle covers ~31 m/s. A light cell measurement takes ~3–8 s; a GCI search ~10–40 s.
Treating one GNSS fix as "the position of the scan" therefore injects 90–1200 m of error — larger
than the accuracy being measured.

Every record must carry **separate timestamps**, not one:

- Two **bracketing** GNSS fixes (before and after the scan), each with `lat`, `lon`, `acc`, `spd`,
  `hdg`, `alt`
- Cell measurement start and end
- Wi-Fi scan start and end

Bracketing lets host-side analysis **interpolate** ground truth to the scan midpoint, which is
accurate to metres on straight constant-speed segments regardless of how long the gap is — far
better than extrapolating from a single fix. The residual limit is the scan's own duration (a 4 s
Wi-Fi scan at 70 mph smears across ~125 m); that is physics, so we **record it and report it as
uncertainty** rather than hide it.

Prefer modem GNSS time. Note that `location_msg.timestamp` falls back to **uptime** if the system
clock was never synchronised — records must flag which time base was used so host tooling can
reject unusable records.

### FW-5 — Record format: versioned CBOR

One CBOR record per capture cycle, using the existing `app/src/cbor/` (zcbor) helpers. Add a CDDL
definition alongside `device_shadow.cddl`.

- **`version` field is mandatory** on every record, and a session header carries the schema
  version too. Host tooling dispatches on it. Bump it on any incompatible change.
- Integer map keys, compact encoding. Budget **≤ 700 B** per FAST record.
- **Omit absent fields entirely** rather than encoding zeros or sentinels — `0` is not a plausible
  value for `rsrq`, `earfcn`, `pci`, `adv`, `frequency`, or `signalStrength`, and a stored zero is
  indistinguishable from a real measurement.
- Store MAC addresses as 6-byte binary strings, not text.
- Timestamp compaction: one absolute epoch-ms base per record plus small deltas for the other five
  timestamps.
- Record contents: version, sequence number, profile tag, time base flag, both GNSS fixes, all six
  timestamps, serving cell, `nmr[]` neighbors, GCI cells (DEEP), Wi-Fi APs
  (`macAddress`/`signalStrength`/`channel`/`frequency`/`band`), and the network mode (LTE-M vs
  NB-IoT).
- **Do not pre-filter observations on device.** Log every AP and cell seen, including
  locally-administered/randomized MACs. Filtering is a host-side analysis decision, and filtering
  early destroys data we cannot recover.
- A **session header** records schema version, device ID, app version, and modem firmware version
  so datasets are attributable and reproducible.

### FW-6 — Storage

Reuse the existing storage module — it already has a LittleFS backend and a clean type-registration
mechanism. Do not write a new one.

- Register a new record type in `DATA_SOURCE_LIST`
  (`app/src/modules/storage/storage_data_types.h`) following the existing pattern. Note that
  `location_check()` in `storage_data_types.c:66` already stores both `LOCATION_GNSS_DATA` and
  `LOCATION_CLOUD_REQUEST`, but as **separate** records — we need them **paired into one**, so the
  survey orchestrator assembles the pair and publishes a single record.
- Select `CONFIG_APP_STORAGE_BACKEND_LITTLEFS` — data must survive power loss mid-drive.
- **Grow the LittleFS partition.** `app/boards/att_flash_partitions.dtsi:162` gives
  `littlefs_storage` only 1 MiB, while external flash is 32 MiB with ~26.2 MiB unallocated
  (`external_flash_partition` at `0x005d_2000`). Target **8 MiB minimum, 24 MiB preferred**.
- Raise `CONFIG_APP_STORAGE_MAX_RECORDS_PER_TYPE` to match (LittleFS default is 256).
- Commit records durably as produced — never buffer in RAM until the end of a drive.
- Behaviour when full must be explicit, configurable, and loudly logged: **stop** (preserve oldest)
  vs **wrap**. Default **stop** — silent data loss would corrupt a study.

Expected capacity at ~600 B/record (10 APs, 10 neighbors, no SSID; ~75% usable after filesystem
and framing overhead):

| Partition | Records | @10 s | @30 s |
|---|---|---|---|
| 1 MiB (current) | ~1,300 | 3.6 h | 11 h |
| 8 MiB | ~10,500 | 29 h | 87 h |
| 24 MiB | ~31,000 | 3.6 days | 10 days |

An 8-hour driving day at 10 s cadence ≈ 2,880 records ≈ 1.7 MB. DEEP records are ~1.0 KB.
**Verify the real figure by measuring encoded size** rather than trusting this estimate.

### FW-7 — Cadence

`CONFIG_APP_SAMPLING_INTERVAL_SECONDS` defaults to 600 s (`app/src/Kconfig.main:8-13`) — far too
slow for driving. Add a survey interval targeting **10–30 s** for FAST, runtime-settable via shell.
Reuse the existing trigger machinery in `main.c` (`trigger_sampling()` ~line 394,
`K_WORK_DELAYABLE_DEFINE(timer_sample_data_work)` line 122) by adding a survey trigger rather than
replacing the asset-tracker one.

### FW-8 — Export over USB (Web Serial)

Primary path: **USB CDC**, driven from a browser via Web Serial. No LTE upload.

- Reuse the storage module's **existing batch-session protocol** (`STORAGE_BATCH_REQUEST` →
  `STORAGE_BATCH_AVAILABLE` → `storage_batch_read()` → `STORAGE_BATCH_CONSUME` →
  `STORAGE_BATCH_CLOSE`, `app/src/modules/storage/storage.h:38-163`). It already provides sessions
  and incremental reads — do not reinvent it.
- Commands, mirroring the existing `att_storage` shell pattern
  (`app/src/modules/storage/storage_shell.c`): `dump [--since <seq>]`, `stats` (record count, bytes
  used, free space), `clear` (explicit confirmation required).
- Export must be **resumable and non-destructive**; clearing is a separate explicit step so a
  failed transfer can never lose data.
- Length-prefix and checksum each record so truncated transfers are detectable.
- Emit the session header first.
- **Verify early:** on Thingy:91 X, USB is on the nRF5340 connectivity bridge, not the nRF9151.
  Confirm which UART/CDC endpoint the shell is reachable on and whether throughput is adequate
  (see `app/src/modules/uart_power_control/`). **This is the highest-uncertainty item in the spec** —
  settle it before building on top of it.

### FW-9 — Field usability

Operators are employees driving vehicles, with no console attached.

- LED status must distinguish: searching, capture OK, GNSS fix poor/absent, storage full, error.
  Reuse the existing `led` module.
- Must run headless from cold boot with no host interaction.
- Keep the task-watchdog coverage the template already provides on every module thread.

### FW-10 — Data hygiene

- **Never call `nrf_cloud_coap_location_get()`.** Prefer compiling the cloud module's location
  path out entirely so it cannot fire by accident.
- A-GNSS is allowed and recommended (faster fixes). It is the only meaningful data consumer:
  ~1–4 KB per fetch, cached — well under 1 MB/month. **Radio scanning itself uses zero cellular
  data**: `%NCELLMEAS` is an internal modem RF measurement and Wi-Fi scanning is local to the
  nRF7002. If A-GNSS is disabled, expect slower TTFF and note that in the dataset.

---

## Host-side requirements

### HOST-1 — Export reader / converter

A script that reads an exported dataset and emits a documented, analysis-friendly form.

- **Dispatch on the schema `version`** field; fail loudly on unknown versions rather than
  guessing.
- Decode CBOR → JSON/JSONL using **ground-fix field names** (`lte[]` with `mcc`/`mnc`/`eci`/`tac`/
  `earfcn`/`pci`/`adv`/`rsrp`/`rsrq`/`nmr[]`, and `wifi.accessPoints[]` with `macAddress`/
  `signalStrength`/`channel`/`frequency`) plus position as `lat`/`lon`/`acc`/`alt`/`spd`/`hdg`.
  Conversion should be close to identity by design.
- **Interpolate ground truth** to the scan midpoint from the bracketing fixes, and emit a
  per-record `interp_uncertainty_m` derived from speed × gap plus scan duration. Retain the raw
  bracketing fixes — never discard them in favour of the interpolated value.
- **Quality gating must be configurable and reported, never silent**: flag/drop records with poor
  GNSS accuracy, uptime-based timestamps, a missing bracket fix, or excessive interpolation
  uncertainty. Print a summary of records in, records out, and dropped-by-reason counts.
- Verify checksums and report per-record failures instead of silently truncating.

Because the on-device format is versioned and uses Nordic's own field names, downstream consumers
(any provider-comparison tooling, dashboards, ad-hoc analysis) can be written independently against
this output.

### HOST-2 — Web Serial export page

Self-contained static HTML page, no external dependencies (Web Serial needs a secure context; a
local file or simple local server is fine):

- Connect to the device, run the export, show progress and record count.
- Save the dataset to disk.
- Surface checksum/length failures rather than hiding them.

---

## Verification strategy

The overriding constraint: **no checkpoint may depend on a later checkpoint to prove it works.**
Hardware is not currently available to the author, so every checkpoint must be provable on the host,
and must additionally ship the on-target command that will prove it once hardware is in hand.

### Existing frameworks (use these — do not invent new ones)

Reconnaissance confirmed three usable layers already in the repo:

| Layer | Location | Runs on | Use for |
|---|---|---|---|
| Unit tests | `tests/module/<module>/` | **`native_sim`** — host, no hardware | logic, state machines, encode/decode |
| Shared test scaffolding | `tests/common/` | `native_sim` | common harness |
| Hardware-in-the-loop | `tests/on_target/tests/` (pytest: `test_functional`, `test_gnss`, `test_ppk`, `test_provisioning`) | real device | end-to-end, deferred until hardware |

Unit tests use **Twister + ztest/Unity + FFF fakes** (`zephyr/fff.h`, `DEFINE_FFF_GLOBALS`,
`FAKE_VALUE_FUNC`), mocking the Location library, `task_wdt`, `date_time`, and `lte_lc` — see
`tests/module/location/` for the pattern to copy. `CONFIG_SHELL=y` is already set in
`app/prj.conf:82`, so shell commands are available as a first-class debugging surface.

### Rules for every checkpoint (non-negotiable)

1. **It builds.** `west build -b thingy91x/nrf9151/ns --sysbuild` succeeds. A checkpoint that does
   not build is not a checkpoint.
2. **It is flashable and boots.** Never leave the device in a state that fails to boot or hangs a
   module thread. The task watchdog must stay satisfied.
3. **It does not regress the template.** New behaviour lives behind **`CONFIG_APP_SURVEY`**,
   defaulting to `n` until the feature is complete. Existing asset-tracker behaviour must remain
   intact and testable at every commit. This is what guarantees flashability.
4. **Existing tests still pass.** `west twister -T tests/module --platform native_sim`.
5. **It carries its own proof** — at least one of:
   - a `native_sim` unit test asserting the new behaviour, **and/or**
   - a shell command that lets a human observe the behaviour directly on hardware.
6. **Commits are small and single-purpose.** One checkpoint per commit, message stating what is now
   provable and how to prove it.

### Observability first

Build the debugging surface **before** the features that need it, so nothing is ever a black box.
A `survey` shell command group is checkpoint CP1 — ahead of all data-format and storage work:

| Command | Purpose |
|---|---|
| `survey scan` | trigger one capture now, synchronously |
| `survey show [n]` | pretty-print the last (or nth) observation: every cell, every AP, every timestamp |
| `survey hex [n]` | hexdump the encoded CBOR record — proves the encoder without a host |
| `survey stats` | record count, bytes used, free space, dropped counters |
| `survey timing` | last measured durations for GNSS / cell / Wi-Fi steps |
| `survey profile <fast\|deep>` | force a profile, bypassing the gating logic |
| `survey selftest` | encode → decode → compare in place on device; prints PASS/FAIL |

Verbose per-step logging behind a Kconfig log level, so a field failure can be diagnosed from a
serial capture alone.

### Checkpoints

Each row states what lands, how it is proven **without hardware**, and how it will be proven **on
hardware**. `native_sim` proofs are the gate for merging; on-target proofs are run later in one pass.

| # | Lands | Host proof (no hardware) | On-target proof |
|---|---|---|---|
| **CP0** | Environment + baseline. No functional change. | Baseline app builds for `thingy91x/nrf9151/ns`; `west twister -T tests/module` green. Record the baseline flash/RAM figures. | Flash unmodified app, confirm it boots |
| **CP1** | `survey` shell group + `CONFIG_APP_SURVEY`; `show` prints observations from the **existing** pipeline (no new fields yet) | Unit test: shell command handlers invoked, formatting correct against a synthetic observation | `survey scan` then `survey show` prints real cells + APs |
| **CP2** | FW-1 struct fields: `channel`/`frequency`/`band` on Wi-Fi, `phys_cell_id` on cells; plumbed through `location.c` and `cloud_location.c` | Unit test in `tests/module/location/`: inject a fake Location event with known channel/freq/PCI, assert they survive onto the zbus message | `survey show` now displays channel/freq/band/PCI; cross-check against an independent Wi-Fi scan |
| **CP3** | FW-5 CBOR encoder + CDDL + `version` field | Unit test: encode → decode round-trip; assert absent fields are **absent, not zero**; assert size ≤ 700 B with 10 APs + 10 neighbors and **record the measured size** | `survey hex` + `survey selftest` prints PASS |
| **CP4** | HOST-1 decoder (reads CP3 output) | Golden-file test: fixture CBOR → expected JSON. Feed it the exact bytes from CP3's unit test so encoder and decoder are proven against each other | Decode a real `survey hex` dump from the device |
| **CP5** | FW-6 storage: new record type, LittleFS backend, grown partition | Unit test: store N records, read back, assert count and content. Assert configured full-behaviour at capacity | `survey stats`; store records, power-cycle, confirm count survives |
| **CP6** | FW-2/FW-4 paired capture orchestrator: GNSS bracket + scan, all six timestamps | Unit test with faked Location library: assert request **sequence** (GNSS → scan → GNSS), assert no overlap, assert all timestamps populated and ordered | `survey scan` then `survey show` shows two GNSS fixes bracketing the scan; `survey timing` reports real durations |
| **CP7** | FW-2 profiles FAST/DEEP + gating | Unit test: gating decisions across a speed/power truth table; assert DEEP requests GCI | `survey profile deep` yields multiple full-identity GCI cells; FAST does not |
| **CP8** | FW-8 export protocol over USB CDC | Unit test: framing, length prefix, checksum, resume-from-sequence | Export to host, verify checksums, interrupt mid-transfer and confirm resume is lossless and non-destructive |
| **CP9** | HOST-2 Web Serial page | Manual: point it at a recorded dump/fixture stream | Full export from device via browser |
| **CP10** | FW-7 cadence, FW-9 LEDs, FW-10 hygiene | Unit test: timer reschedule maths; assert ground-fix is never called | Headless cold-boot run; LED states legible; storage-full behaviour correct |

**Note on CP8:** USB on Thingy:91 X routes through the nRF5340 connectivity bridge, not the
nRF9151. That cannot be fully validated on the host. Prove the *protocol* over the existing serial
shell first (which is transport-agnostic), so only the physical transport remains unproven — this
keeps the risk contained to one checkpoint instead of blocking the project.

### Final validation (requires hardware)

1. **Bench, stationary at a surveyed point.** GNSS accuracy sane; Wi-Fi MACs/channels/frequencies
   correct against an independent scan; serving cell matches the network; DEEP returns multiple
   full-identity GCI cells.
2. **Timing characterisation.** Real durations for hot GNSS, light cell measurement, GCI search,
   Wi-Fi scan — via `survey timing`. These set the actual interpolation error budget; measure rather
   than trusting the estimates in FW-4.
3. **Storage soak.** Run to partition-full; confirm full-behaviour, no corruption, and that export
   after power-cycle returns every record.
4. **Drive test.** Short loop with overlapping passes; confirm repeat visits produce consistent
   observations and that interpolation behaves under real motion.
5. Add a `survey` suite under `tests/on_target/tests/` once the above passes manually.

## Risks

| Risk | Mitigation |
|---|---|
| USB CDC path via nRF5340 bridge unclear or slow | Settle in the first work item (FW-8) before depending on it |
| GNSS↔LTE contention causes long cycles or `-EBUSY` | Strict sequencing; never cancel; characterise in verification 5 |
| Time smear at highway speed floors achievable validation | Bracket + report uncertainty; validate tight accuracy claims at low speed |
| 10-AP cap truncates dense urban scans | Accepted; document it so analysis can account for it |
| Format churn invalidates earlier datasets | Mandatory `version` field; freeze the schema before large collection campaigns |

## Build order

Follow the checkpoints CP0 → CP10 in order. That ordering is deliberate:

- **Observability precedes features** (CP1 before everything) so no later checkpoint is a black box.
- **Encoder and decoder are adjacent** (CP3, CP4) and proven against each other's bytes, so the
  schema-critical path is settled early and cheaply.
- **Storage precedes the orchestrator** (CP5 before CP6) so captured records have somewhere durable
  to land the moment pairing works.
- **The riskiest transport work is last but pre-proven** (CP8) — the protocol is validated over the
  existing serial shell, leaving only the physical USB path unverified.

FW-3 requires no work — it exists only to record the decision not to pursue `adv`.

FW-1 (CP2), FW-5 (CP3), and HOST-1 (CP4) are the schema-critical path and should be reviewed
together before anything is built on top of them.
