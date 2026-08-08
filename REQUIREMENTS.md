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
| `pci` | Physical cell ID | **neighbors only** — see FW-1 |
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

- **`struct location_wifi_ap_info`** has only `rssi`, `mac`, `mac_length`. Add **`channel`** and
  **`band`**. (Do not add SSID.)

  **`frequency` is derived, not stored.** `struct wifi_scan_result` reports channel and band but
  never a frequency, and frequency is a pure function of the two, so storing it would cost two
  bytes per AP to hold a value that can always be recomputed. The firmware derives it for display
  and the host decoder applies the same mapping to emit the ground-fix `frequency` field. `band` is
  captured alongside `channel` because channel numbers repeat across bands, so only the pair
  identifies a frequency.

- **`struct location_cell_info`** — used for the serving cell *and* every entry of `gci_cells[]` —
  has no PCI. **Deliberately left that way.** `mcc`/`mnc`/`eci`/`tac` already identify those cells
  globally, so PCI adds nothing a lookup can use, and adding the field would mean editing an
  upstream struct for no gain. Neighbors in `nmr[]` have no identity and *do* carry `pci`
  (`location_neighbor_cell_info` already has it) — it is the only thing distinguishing them.

  Accepted consequence: without PCI on GCI cells there is no way to build a PCI→ECI map, so
  `nmr[]` neighbors in DEEP records cannot be retroactively resolved to identified cells. Judged
  recoverable later if it ever matters.

Plumb the Wi-Fi fields through `location.c`'s event handler (`copy_wifi_data()` in
`location_helper.c`) and the copy logic in `app/src/modules/cloud/cloud_location.c`
(`wifi_ap_data_construct`), which currently drops them.

`struct location_cloud_request_data` already carries `current_cell`, `neighbor_cells[]`,
`gci_cells[]`, and `wifi_aps[]` — **reuse it as the capture payload.** Do not define a parallel
struct.

Existing caps are acceptable and need **no change**: `CONFIG_APP_LOCATION_WIFI_APS_MAX`=10,
`CONFIG_APP_LOCATION_NEIGHBOR_CELLS_MAX`=8, `CONFIG_NRF_WIFI_SCAN_MAX_BSS_CNT`=10,
`CONFIG_LOCATION_METHOD_WIFI_SCANNING_RESULTS_MAX_CNT`=10. Note in docs that dense urban
environments will exceed 10 visible APs and be truncated — an accepted tradeoff.

#### Wi-Fi must scan both bands (fixed during CP2)

`app/boards/thingy91x_nrf9151_ns.conf` sets `CONFIG_NRF_WIFI_2G_BAND=y` to keep scan time down for
the asset tracker. The effect on a survey is that **5 GHz access points are invisible**, which
biases the highest-value observation — 5 GHz APs are numerous and are the better locationing
reference, since shorter range means a tighter position bound.

`overlay-survey.conf` therefore sets `CONFIG_NRF_WIFI_ALL_BAND=y`. This is purely a Kconfig
override; no devicetree change is involved, and the board files are untouched. Note the board
overlay's `nordic,nrf7000-spi` compatible is *not* what restricted the band — `NRF70_2_4G_ONLY`,
the only symbol that steers the band choice, is `def_bool y if WIFI_NRF7001` and was never set. The
compatible only defaults the usage mode to scan-only, which is wanted, so it stays as it is.

Measured at one bench position: 10 APs before, all 2.4 GHz; 10 APs after, of which 3 were 5 GHz
(channels 116 and 157, cross-checked against a host scan). Flash and RAM unchanged.

`CONFIG_LOCATION_REQUEST_DEFAULT_WIFI_TIMEOUT` is raised from the board conf's 5000 to 30000. The
5 s budget was sized for a 2.4 GHz-only sweep; scanning both bands adds roughly sixteen DFS channels
that must be dwelled on passively. This matters more than it looks: the nRF70 offloads the scan and
emits **all** results only once the full sweep completes, so exceeding the timeout cancels the scan
and yields `wifi_cnt = 0` rather than a truncated list — the request then proceeds with cellular
data only and the record looks like a place with no visible Wi-Fi. 30000 matches the Location
library default and the driver's own scan timeout, and costs nothing when the scan completes
normally.

`CONFIG_WIFI_NRF70_SKIP_LOCAL_ADMIN_MAC` is set to `n`; the board conf enables it. It drops APs with
locally-administered BSSIDs on the assumption they are virtual interfaces co-located with a real
one — a pre-filter on raw data, which FW-5 forbids.

**Consequence for FW-6:** the 10-AP cap now binds harder, and its bias is worse than a plain
truncation. The RPU keeps an RSSI-ranked list capped at `CONFIG_NRF_WIFI_DISPLAY_SCAN_BSS_LIMIT`
(10) and replaces weaker entries as it scans, so what survives is the strongest 10 — and 5 GHz APs
are weaker at equal range, so a strongest-10 filter systematically under-represents the band just
added. Raise `NRF_WIFI_DISPLAY_SCAN_BSS_LIMIT`, `NRF_WIFI_SCAN_MAX_BSS_CNT`,
`LOCATION_METHOD_WIFI_SCANNING_RESULTS_MAX_CNT` and `APP_LOCATION_WIFI_APS_MAX` together when sizing
the record, and `CONFIG_NRF_WIFI_CTRL_HEAP_SIZE` with them.

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
- **Do not use `LOCATION_SEARCH_CANCEL`.** Resolved in CP1: the template's own event handler fired
  it after every cloud request. Scan-only requests now answer the library with
  `location_cloud_location_ext_result_set(LOCATION_EXT_RESULT_UNKNOWN, NULL)` instead, which winds
  the request down through the library's own state machine. The asset-tracker path is unchanged.
  `location.h` documents that cancelling cannot truly cancel
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

**`alt`, `spd` and `hdg` need no new plumbing** (established in CP1). They are absent from
`struct location_data`, but `CONFIG_LOCATION_DATA_DETAILS` — already selected by `Kconfig.location` —
carries the full `nrf_modem_gnss_pvt_data_frame` alongside every fix, which has altitude, speed,
heading and their accuracies plus satellite counts. Read them from
`location_data.details.gnss.pvt_data`.

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
- **`rsrp` and `rsrq` are 3GPP index values, not dBm/dB** (established in CP1). `lte_lc` passes the
  modem's raw indices through unconverted, while the ground-fix API expects dBm/dB — index 55 is
  −86 dBm. Decide explicitly whether the record stores indices or converted values, state it in the
  schema, and convert on exactly one side. Getting this wrong offsets every signal measurement in
  the study by ~140. Conversion formulas: `RSRP_IDX_TO_DBM` / `RSRQ_IDX_TO_DB` in
  `modem/modem_info.h`. Note index **0 means "not used"** for both, so it is an absent value, not a
  measurement — as are `LTE_LC_CELL_RSRP_INVALID` / `..._RSRQ_INVALID` (both 255).
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
| Unit tests | `tests/module/<module>/` | **`native_sim`, inside Docker** (see below) | logic, state machines, encode/decode |
| Shared test scaffolding | `tests/common/` | same | common harness |
| Hardware-in-the-loop | `tests/on_target/tests/` (pytest: `test_functional`, `test_gnss`, `test_ppk`, `test_provisioning`) | real device | end-to-end, deferred until hardware |

Unit tests use **Twister + ztest/Unity + FFF fakes** (`zephyr/fff.h`, `DEFINE_FFF_GLOBALS`,
`FAKE_VALUE_FUNC`), mocking the Location library, `task_wdt`, `date_time`, and `lte_lc` — see
`tests/module/location/` for the pattern to copy. `CONFIG_SHELL=y` is already set in
`app/prj.conf:82`, so shell commands are available as a first-class debugging surface.

#### Running unit tests on macOS

**`native_sim` is Linux-only** — Zephyr's POSIX arch refuses to configure on macOS with
*"The POSIX architecture only works on Linux."* Unit tests therefore run in a small Linux container:

```shell
scripts/run_unit_tests.sh                    # all module tests
scripts/run_unit_tests.sh tests/module/location   # one suite
scripts/run_unit_tests.sh --rebuild          # force image rebuild
```

The image (`tests/unit_docker/Dockerfile`) is deliberately **not** the ~20 GB x86 CI image: because
`native_sim` builds with the *host* compiler, no Zephyr SDK is needed, so a slim Python 3.12 base
runs natively on arm64. Hard-won details baked in, do not regress them:

- **Python ≥ 3.12** — Zephyr 4.4 requires it; Debian bookworm ships 3.11.
- **`ruby`** — required by NCS's `test_runner_generate` (Unity/CMock generators).
- **`zcbor==0.9.1` + `cbor2==5.6.5`** — must match the zcbor bundled in NCS v3.4.0
  (`modules/lib/zcbor`). Newer `cbor2` breaks zcbor with
  `ImportError: cannot import name 'CBORDecodeValueError'`. `app/src/cbor/CMakeLists.txt:28` invokes
  the `zcbor` CLI at configure time, so any suite that compiles the app fails without this.
- **Platform must be `native_sim/native/64`** on arm64 hosts. Plain `native_sim` sets
  `CONFIG_64BIT=n` and aborts with *"this Aarch64 machine has a 64-bit userspace"*.

CI (`.github/workflows/sonarcloud.yml`) uses `ghcr.io/zephyrproject-rtos/ci` with plain
`native_sim` on x86 Linux; the container above is the local equivalent, not a replacement.

### Rules for every checkpoint (non-negotiable)

1. **It builds.** `west build -b thingy91x/nrf9151/ns --sysbuild` succeeds. A checkpoint that does
   not build is not a checkpoint.
2. **It is flashable and boots.** Never leave the device in a state that fails to boot or hangs a
   module thread. The task watchdog must stay satisfied.
3. **It does not regress the template.** New behaviour lives behind **`CONFIG_APP_SURVEY`**,
   defaulting to `n` until the feature is complete. Existing asset-tracker behaviour must remain
   intact and testable at every commit. This is what guarantees flashability.
4. **Existing tests still pass.** `scripts/run_unit_tests.sh`.
5. **It carries its own proof** — at least one of:
   - a `native_sim` unit test asserting the new behaviour, **and/or**
   - a shell command that lets a human observe the behaviour directly on hardware.
6. **Commits are small and single-purpose.** One checkpoint per commit, message stating what is now
   provable and how to prove it.

### Observability first

Build the debugging surface **before** the features that need it, so nothing is ever a black box.
A `survey` shell command group is checkpoint CP1 — ahead of all data-format and storage work:

| Command | Purpose | Lands in |
|---|---|---|
| `survey scan` | request a Wi-Fi + cellular scan (no GNSS) | CP1 ✅ |
| `survey gnss` | request a GNSS-only fix | CP1 ✅ |
| `survey show` | pretty-print the cached observation: every cell, every AP | CP1 ✅ |
| `survey stats` | observation counters and cache age | CP1 ✅ |
| `survey clear` | discard the cached observation | CP1 ✅ |
| `survey selftest` | render a synthetic observation; needs no radio | CP1 ✅ |
| `survey hex` | hexdump the encoded CBOR of the **cached** observation — proves the encoder without a host | CP3 ✅ |
| `survey hex <n>` | hexdump a **stored** record by index; needs somewhere to store one | CP5 |
| `survey timing` | last measured durations for GNSS / cell / Wi-Fi steps | CP7 |
| `survey profile <fast\|deep>` | force a profile, bypassing the gating logic | CP7 |

`scan` and `gnss` are separate commands because the Location library treats its method list as a
**fallback chain and stops at the first method that succeeds** — a request including GNSS returns a
fix and never scans. This is the same reason FW-2 requires sequenced requests.

Verbose per-step logging behind a Kconfig log level, so a field failure can be diagnosed from a
serial capture alone.

### Checkpoints

Each row states what lands, how it is proven **without hardware**, and how it will be proven **on
hardware**. `native_sim` proofs are the gate for merging; on-target proofs are run later in one pass.

| # | Lands | Host proof (no hardware) | On-target proof |
|---|---|---|---|
| **CP0** ✅ | Environment + baseline + `scripts/run_unit_tests.sh`. No functional change. | **Done.** Baseline builds for `thingy91x/nrf9151/ns` (`merged.hex` produced); `scripts/run_unit_tests.sh` = **11/11 passed, 0 filtered**. Baseline size: **text 400,640 / data 155,473 / bss 190,982**. | Flash unmodified app, confirm it boots |
| **CP1** ✅ | `survey` shell group + `CONFIG_APP_SURVEY` (default n); `show` prints observations from the **existing** pipeline (no new fields yet). Adds `LOCATION_SCAN_SEARCH_TRIGGER` + `CONFIG_APP_LOCATION_SCAN_TRIGGER` to the location module. | **Done.** `tests/module/survey` = **29/29**; `tests/module/location` extended with 4 tests for the new trigger. Survey-off build **byte-identical to CP0** (text 400,640 / data 155,473 / bss 190,982); survey-on +2,276 text / +2,376 bss, RAM 83.3%. Both produce `merged.hex`. | `survey selftest` first (no radio needed), then `survey scan` → `survey show` prints real cells + APs; `survey gnss` → `survey show` prints a fix |
| **CP2** ✅ | FW-1 struct fields: `channel` + `band` on Wi-Fi, plumbed through `location_helper.c` and `cloud_location.c`; `frequency` derived for display. No cell-struct change — PCI deliberately omitted on identified cells. | **Done.** `tests/module/survey` = **33/33** (frequency derivation across 2.4/5 GHz, absent channel, unrecognised band, channel-outside-band); `tests/module/location` Wi-Fi verifier asserts `channel`/`band` survive onto the zbus message. Survey build FLASH 65.63% / RAM 79.11%; default build also verified so the `cloud_location.c` path is compiled. Storage pipe raised 512→576 (`struct location_msg` grew 492→512, +4 header = 516 required). | **Done.** `survey selftest` renders `channel 6 frequency 2437 MHz band 2.4GHz`; real scan returned 9 APs with channels 1/4/6/11 → 2412/2427/2437/2462 MHz, channels cross-checked against an independent host Wi-Fi scan |
| **CP3** ✅ | FW-5 CBOR encoder + CDDL + `version` field; `survey hex` encodes the cached observation | **Done.** `tests/module/survey_record` = **23/23**, full `tests/module` = **13/13 configurations**. Absent-not-zero asserted per optional field. **Measured worst case: 583 B** (10 APs + 10 neighbours + 3 GCI + both bracket fixes + all six timestamps), asserted against the 700 B budget rather than printed. Survey build text 384,980 / data 148,794 / bss 186,971; default build also verified. | `survey hex` prints a decodable dump |
| **CP4** ✅ | HOST-1 decoder `scripts/survey_decode.py` (reads CP3 output) | **Done.** `tests/host` = **58/58**, stdlib only. Golden files are the **exact bytes CP3's encoder emitted**, lifted from the suite by `scripts/survey_fixture.py`, so encoder and decoder are proven against each other rather than against two readings of the CDDL — which is how the `time-base` enum disagreement below was caught. Covers indefinite-length CBOR (what zcbor actually emits), version dispatch, RSRP/RSRQ and Wi-Fi frequency conversion, interpolation, and the quality gate. | Decode a real `survey hex` dump from the device |
| **CP5** | FW-6 storage: new record type, LittleFS backend, grown partition | Unit test: store N records, read back, assert count and content. Assert configured full-behaviour at capacity | `survey stats`; store records, power-cycle, confirm count survives |
| **CP6** | FW-2/FW-4 paired capture orchestrator: GNSS bracket + scan, all six timestamps | Unit test with faked Location library: assert request **sequence** (GNSS → scan → GNSS), assert no overlap, assert all timestamps populated and ordered | `survey scan` then `survey show` shows two GNSS fixes bracketing the scan; `survey timing` reports real durations |
| **CP7** | FW-2 profiles FAST/DEEP + gating | Unit test: gating decisions across a speed/power truth table; assert DEEP requests GCI | `survey profile deep` yields multiple full-identity GCI cells; FAST does not |
| **CP8** | FW-8 export protocol over USB CDC | Unit test: framing, length prefix, checksum, resume-from-sequence | Export to host, verify checksums, interrupt mid-transfer and confirm resume is lossless and non-destructive |
| **CP9** | HOST-2 Web Serial page | Manual: point it at a recorded dump/fixture stream | Full export from device via browser |
| **CP10** | FW-7 cadence, FW-9 LEDs, FW-10 hygiene | Unit test: timer reschedule maths; assert ground-fix is never called | Headless cold-boot run; LED states legible; storage-full behaviour correct |

**Note on CP4 — why the golden files are generated, not hand-written.** The first decoder was written
from the CDDL and disagreed with the firmware on `time-base`: the schema documented `0 = Unix epoch,
1 = uptime`, while the encoder casts `enum survey_time_base` (`NONE=0, UPTIME=1, UNIX=2`) straight
onto the wire. Every record would have been mislabelled, and no test written against the CDDL alone
would have caught it. The CDDL now documents the C enum, which is the source of truth because the
encoder writes it through unchanged. Keep the fixtures generated from real encoder output
(`tests/host/README.md`) — that property is the whole value of the test.

**Note on schema types — three defects the pre-commit review caught, all of the same shape.** Each
was a type or sentinel that looked right against a synthetic record and was wrong against a real one:

- **RSRP and RSRQ were typed `uint`.** `lte_lc.h` documents the 3GPP indices as RSRP −17…97 and
  RSRQ −30…46, and zcbor **range-guards** rather than clipping — so one negative neighbour reading
  fails `cbor_encode_survey_record` and discards the **entire** record, GNSS bracket included. It
  would only ever have fired at the edge of coverage, which is the population this survey exists to
  measure. Now `int .size 1`, with a round-trip test at both range bottoms.
- **`timeDiff` of 0 was stored as a measurement.** `LTE_LC_CELL_TIME_DIFF_INVALID` **is** 0, so that
  fabricated perfect alignment out of a neighbour the modem could not align. Now absent, via
  `survey_absent.h` so the console and the encoder cannot disagree.
- **`survey selftest` fabricated four GNSS detail fields as zero.** The synthetic `location_msg` left
  `details` zeroed while the encoder writes altitude/speed/heading/satsUsed whenever a fix carries
  details — the first command the operator is told to run emitted four fake measurements. The
  synthetic fixture now carries non-zero details.

The pattern to carry into CP5: a field's *sentinel* is part of its type, and a synthetic test record
that only uses comfortable values proves nothing about either.

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

   Partial data already measured on target (one bench position, host-clock round trip from issuing
   the trigger to the result being stamped, so each figure includes a little console latency):

   | Step | Measured | Notes |
   |---|---|---|
   | Wi-Fi scan, both bands | **4.66 / 4.66 / 4.87 s** | 10 APs each run, 3 of them 5 GHz |
   | Scan leg (Wi-Fi **and** cell together) | **4.61 / 4.86 s** | 10 APs + serving + nmr + **3 GCI cells** |
   | GNSS leg, hot | **0.55 / 2.13 s** | Immediately after a previous fix |
   | Warm GNSS fix | ~4.1 s | Single sample; 6 satellites, 6.6 m |
   | Cold GNSS fix | ~47.7 s | Single sample; no A-GNSS (`NRF_CLOUD_AGNSS=n`) |

   **Full sequential cycle (GNSS then scan): 5.2 s and 7.0 s.** Comfortably inside a 10 s cadence
   when GNSS is hot; a cold fix is what breaks the budget, not the radio work.

   Note the scan leg covers Wi-Fi *and* the whole cell measurement, including a GCI search — the
   Wi-Fi scan runs on the nRF7002 and overlaps the cell step, exactly as FW-2 anticipates.

   **This contradicts FW-2's assumption that a GCI search costs 10–40 s.**
   `CONFIG_LOCATION_REQUEST_DEFAULT_CELLULAR_CELL_COUNT` is already 3, and three full-identity cells
   are being returned inside the same ~4.7 s. The FAST/DEEP split may therefore be unnecessary at
   `cell_count = 3`, and the real question for CP7 is where the cost curve turns as the count rises.
   Measure before building the gating machinery.

   Two conclusions. The Wi-Fi scan is **not** the cadence constraint — under 5 s against a 10–30 s
   target — and the board conf's 5000 ms Wi-Fi timeout had only ~3% margin over the measured time,
   which is why it was raised.

   Dwell-time tuning was tried and **rejected on measurement**: with
   `LOCATION_METHOD_WIFI_SCANNING_PARAMS_OVERRIDE` and passive dwell cut 260→130 ms, active 50→40 ms,
   four runs gave 4.27 / 5.04 / 4.42 / 4.44 s — a ~4% mean change, inside the run-to-run spread, with
   one tuned run slower than every baseline run. Scan time is therefore not dwell-dominated, so the
   default 260 ms (two beacon intervals, better for weak APs) is kept.

   What would genuinely shorten it all costs data: `WIFI_NRF70_SCAN_DISABLE_DFS_CHANNELS` would drop
   channel 116, which carried 3 of the 10 APs seen here, and restricting the channel list is the same
   pre-filtering FW-5 forbids. `max_bss_cnt` is not an option — Zephyr documents that it cannot be
   relied on to limit scan time.

   The cycle-time work therefore belongs in FW-2's sequencing, not in the scan: the Wi-Fi scan runs
   on the nRF7002 and may overlap the cell measurement, and the remaining dominators are the GCI
   search (10–40 s) and a cold GNSS fix.
3. **Storage soak.** Run to partition-full; confirm full-behaviour, no corruption, and that export
   after power-cycle returns every record.
4. **Drive test.** Short loop with overlapping passes; confirm repeat visits produce consistent
   observations and that interpolation behaves under real motion.
5. Add a `survey` suite under `tests/on_target/tests/` once the above passes manually.

## Risks

| Risk | Mitigation |
|---|---|
| USB CDC path via nRF5340 bridge unclear or slow | Settle in the first work item (FW-8) before depending on it |
| GNSS↔LTE contention causes long cycles or `-EBUSY` | Strict sequencing; scan-only requests answer the library instead of cancelling (done in CP1); characterise in verification 5 |
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

---

## Configuration decisions

Rationale for the non-obvious settings in `app/overlay-survey.conf`. The overlay itself
carries one-line comments; the reasoning lives here.

### No cloud communication (`APP_CLOUD_LOCATION=n`, `APP_CLOUD_PROVISIONING=n`)

`CONFIG_APP_CLOUD=n` does not build: `main.c` includes `cloud.h` and `fota.h`
unconditionally and references them throughout. Rather than patch `main.c`, two new
Kconfig options gate the parts that matter, following the module's existing
`APP_LOCATION` / `APP_ENVIRONMENTAL` pattern:

- **`APP_CLOUD_LOCATION`** excludes `cloud_location.c`, which is the only caller of
  `nrf_cloud_coap_location_get()`. This is what actually enforces FW-10. Relying on the
  device having no credentials is not enforcement: a coworker's previously-claimed
  Thingy:91 X would resolve position on every scan.
- **`APP_CLOUD_PROVISIONING`** prevents entry into `STATE_PROVISIONING`. That state
  publishes `LOCATION_SEARCH_CANCEL` (provisioning needs offline LTE mode), which is the
  `-EBUSY` hazard FW-2 warns about, and without a provisioning service to respond the
  module would wait there for the rest of the boot.

`CONFIG_NRF_PROVISIONING` is deliberately left `y`. Setting it `n` orphans ten
`CONFIG_NRF_PROVISIONING_*` assignments in `prj.conf`, which aborts the Kconfig stage on a
clean build. The client is built but never triggered.

`APP_CLOUD_BACKOFF_INITIAL_SECONDS` must not exceed `APP_CLOUD_BACKOFF_MAX_SECONDS`;
`cloud.c` asserts on it, and with `CONFIG_RESET_ON_FATAL_ERROR=y` a violation reboots the
device.

### Time (`DATE_TIME_NTP` left enabled)

NTP is a data consumer but the wrong one to remove: correlating a GNSS fix with the scan
beside it is the point of the dataset, so the clock is load-bearing. `date_time` prefers
modem NITZ time, verified present on target — `AT+CCLK?` returns a timezone offset, which
NTP cannot supply. NTP only fires when NITZ is absent and the clock has gone stale.

The GNSS fix itself is already a time source: `apply_gnss_time()` in `location.c` calls
`date_time_set()` from the PVT frame's UTC on every fix that reports valid datetime, so the
device does not depend on
the network for time once it has seen the sky. Two consequences for record assembly: the
clock can be **stepped mid-record**, so a record's time base is not guaranteed monotonic
across its own timestamps, and the PVT sub-second field is discarded — `date_time_set()` is
fed whole seconds. Both are open questions for the record schema (FW-5).

### Raw GNSS (`NRF_CLOUD_AGNSS=n`)

Deviates from FW-10, which recommends A-GNSS. The device runs on vehicle power, so the
energy saved on a fast first fix does not matter, and the modem retains ephemeris for
hours, making tunnels and GNSS/LTE interleaving warm starts. Cost is the first fix after
power-on. Reversible: delete the line and claim the device on nRF Cloud.
`CONFIG_NRF_CLOUD_PGPS` is worth evaluating first — one download gives roughly two weeks
of predicted ephemeris, so the device contacts the cloud only fortnightly.

### GNSS tuning

`LOCATION_REQUEST_DEFAULT_GNSS_VISIBILITY_DETECTION=n` is the important one. It aborts a
request after 3 s when fewer than 3 satellites are visible. On target that turned a
perfectly obtainable fix into three consecutive failures: with detection off, the same
device fixed in **33.7 s having acquired 5 satellites**. For a survey device the feature
converts obstructed sky into missing data.

`GNSS_TIMEOUT=600000` with `TIMEOUT=660000` (the overall timeout must exceed the
per-method one). `GNSS_NUM_CONSECUTIVE_FIXES=2`, the minimum the symbol allows — its range
is [2, 256].

Consequence: a GNSS request can hold the location module for minutes, and triggers
arriving meanwhile are dropped. `location.c` logs that at warning level and
`survey_shell.c` says so, because otherwise `survey show` returns a stale scan that looks
fresh.

### Sampling interval

`APP_SAMPLING_INTERVAL_SECONDS=86400` suppresses the asset-tracker's repeat sampling but
**not** the sample it takes at startup, which still issues one GNSS-first location request
per boot. Its results are indistinguishable from the survey's on `location_chan`.
Eliminating the second driver is FW-7 / CP6 work.
