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

**Open question (raised at CP6): the cell and Wi-Fi windows are currently the same window.** CP1's
`LOCATION_SCAN_SEARCH_TRIGGER` issues WIFI+CELLULAR as **one** `location_request()`, and the Location
library reports one result for it — there is no callback, event or detail field that separates when
the cell measurement ran from when the Wi-Fi scan ran. CP6 therefore writes the observed combined
window into **both** pairs of offsets. That is conservative rather than wrong: the host's uncertainty
model widens with scan duration, so an over-wide window under-claims accuracy. Splitting it into two
sequenced single-method requests would give true per-leg windows, and at the 60 s cadence the extra
few seconds are affordable. Deferred because it doubles the number of ways a cycle can partially
fail; revisit once CP9 shows whether the combined window is actually limiting the analysis.

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

**Measured capacity (CP5, built and asserted).** The estimate below the strikethrough was close on
records but wrong about the mechanism, so the derivation is worth stating exactly.

The LittleFS backend does not pack records end to end. It writes each type at a **fixed stride** of
`data_size` bytes and never lets an entry straddle a block, so a variable-length record occupies a
fixed slot and the per-block waste is `block_size % slot_size`. That makes the slot size a real
design choice rather than a rounding of the record size:

- Slot = **816 B** (`uint32_t len` + 812 B payload). At the 4096 B erase block that is **5 slots per
  block, 4080/4096 used — 0.4 % waste**. Rounding the slot to a tidy 1024 B would fit 4 per block
  and throw away 20 % of the partition, roughly a day of driving.
- Payload 812 B ≥ the 700 B record budget (`survey_store.c` BUILD_ASSERTs this; the measured DEEP
  record is 585 B).

**The binding resource is blocks, not bytes.** The math below (`get_file_index()` is
`index / entries_per_block`, one file per block) was the original design and is still what
`CONFIG_APP_STORAGE_LITTLEFS_TARGET_FILE_SIZE` defaults to (0) for any type that does not override
it. **The survey build no longer uses it**: `app/overlay-survey.conf` sets that Kconfig to 65536 (16
blocks/file), so `get_file_index()` is now `index / entries_per_file` with `entries_per_file = 80`,
not 5, and 25,000 records live in **~313 files**, not 5,000. This exists specifically because 5,000
files in one flat LittleFS directory made `fs_mgmt` downloads (and the console's own `fs ls`) time
out at near-full capacity — see "A near-full partition makes the export transport hang, not just
fail" in `docs/common/dev_workflow.md`. The per-block waste analysis below (why 816 B, not 819 or
1024) is unaffected by this — it is about slot size within a block, not files per block — but the
file-count and metadata-overhead numbers that follow describe the pre-fix, one-block-per-file
layout and are stale for the survey build specifically.

- Partition **24 MiB** = 6144 blocks.
- 25,000 records → **5,000 files → 5,000 data blocks** *(pre-fix; survey build is now ~313 files,
  same 5,000 data blocks)*, plus directory metadata pairs for 5,000 entries in one directory (order
  200–250 blocks) *(now ~313 entries, well under that)*, the superblock pair, and free blocks for
  copy-on-write and `block-cycles` relocation. Roughly **15 % spare**, now more given the reduced
  directory-metadata cost.
- `CONFIG_APP_STORAGE_MAX_RECORDS_PER_TYPE=25000`.

**One data block per file only holds while the file stays under 4088 bytes** — this constraint
applied to the original one-block-per-file layout above; it no longer constrains the survey build,
which groups 16 blocks per file. LittleFS keeps a file in a single block only up to `block_size - 8`
— the CTZ skip-list reserves two 4-byte pointers — so the 16 bytes that 816 B slots leave at the end
of each block were not slack, they were what kept each file single-block *when each file was exactly
one block*. 4080 clears 4088 by **eight bytes**. A slot size chosen to pack the block harder (819 B →
4095 of 4096, which looks strictly better on a waste-percentage basis) would have put every file on
two blocks and doubled the partition's block cost under the old layout. The margin is documented at
`SURVEY_STORE_SLOT_SIZE` in `survey_store.h`; anyone retuning the slot size for a build that still
uses the one-block-per-file default has to re-check it.

The backend's own `verify_partition_size()` **cannot** check this. It computes
`ceil(data_size × RECORDS_PER_TYPE / block_size)` — densely packed records — plus a flat 3 blocks,
which for 30,000 records gives 5,980 of 6,144 and passes. The real cost of 30,000 records is 6,000
data blocks *before* any metadata, so a configuration that passes the assert at boot can still run
LittleFS out of blocks mid-drive. That is why the cap is 25,000 rather than the 30,705 slots the
byte arithmetic suggests.

| Cadence | Records/day | Days of data |
|---|---|---|
| 10 s | 8,640 | **2.9** |
| 20 s | 4,320 | **5.8** |
| 30 s | 2,880 | **8.7** |
| 60 s | 1,440 | **17.4** |

**A week of data therefore requires a capture interval of about 25 s or slower.** This is a
constraint on FW-7/CP10, not a footnote: at 10 s this partition fills over a long weekend, and with
`FULL_STOP` configured the device then stops recording rather than eating the start of the run.

**Settled: 60 s.** The stakeholder relaxed the cadence explicitly ("once every minute is fine"),
which buys 17 days and leaves the measured 7.8 s warm cycle (CP6) using 13 % of the interval. The
headroom is what makes the per-leg-window split in FW-4 affordable if CP9 shows it is needed.

~~Expected capacity at ~600 B/record (~75% usable after filesystem and framing overhead): 1 MiB ≈
1,300 records; 8 MiB ≈ 10,500; 24 MiB ≈ 31,000.~~ The 24 MiB figure was wrong for two reasons that
happened to partly cancel: the 600 B record guess and the 75 % overhead guess, and then the
one-file-per-block layout that neither accounted for.

**Open questions for on-target verification.**

1. ~~**Metadata cost is estimated, not measured.**~~ **Answered on target (CP5); 25,000 stands, the
   20,000 fallback is not needed.** Measured with `fs statvfs /att_storage` while adding directory
   entries, on the real 6,144-block partition:

   | Directory entries | `bfree` |
   |---|---|
   | 71 | 6072 |
   | 252 | 6068 |
   | 264 | 6066 |
   | 392 | 6062 |

   **+321 entries cost 10 blocks — about one block per 32 entries.** Those were 1-byte pad files,
   which LittleFS inlines into the directory (`inline_max = min(block_size/8, cache_size)` = 64 here),
   so each costs only its name and a short inline record; a `SURVEY_<n>.bin` entry carries a CTZ
   struct instead and costs somewhat more. Scaling 5,003 entries off the inline rate gives ~155
   blocks, and the heavier per-entry record puts the real figure in the **200–250 block** range the
   arithmetic predicted. **~14–15 % spare is confirmed.**

   **Superseded by a direct measurement, which agrees.** The estimate above was later replaced by
   filling the partition with real `SURVEY_<n>.bin` records rather than inlined pad files, tracking
   `bfree` from 743 records to 12,007 — half the cap:

   | SURVEY records | 743 | 2743 | 5143 | 8007 | 12007 |
   |---|---|---|---|---|---|
   | `bfree` | 5975 | 5553 | 5049 | — | 3602 |

   **42.14 blocks per 200 records**, and that rate did not drift by so much as a block across 11,264
   records — 40 data blocks plus ~2 of directory metadata, exactly the one-file-per-block model.
   Extrapolated to 25,000 records: **5,279 of 6,144 blocks, 14.1 % spare.** The estimate said
   14–15 %; the measurement says 14.1 %. 25,000 stands.

   A separate scare during this measurement was wrong and is recorded so it is not re-derived: the
   BATTERY and ENVIRONMENTAL types do **not** reserve `MAX_RECORDS_PER_TYPE` blocks each.
   `lfs_storage_store()` opens with `FS_O_CREATE` and seeks, so files are created lazily and
   `MAX_RECORDS_PER_TYPE` is a cap, not a reservation — a 280-byte `BATTERY_0.bin` on a device
   configured for 25,000 records per type is the proof.

   Not an open question: Partition Manager does **not** own this layout. The build generates no
   `partitions.yml`, and `build-survey/app/zephyr/zephyr.dts` shows `littlefs_storage` at
   `reg = <0x4d2000 0x1800000>` sourced from `overlay-survey.overlay`, with the `zephyr,fstab`
   node pointing at it. The overlay is what takes effect.
2. ~~**Store latency as the partition fills.**~~ **Measured on target (CP5) to 12,007 records —
   about half the cap. It grows, it is affordable, and it is two separate costs, not one.**

   Method: `CONFIG_APP_STORAGE_LOG_LEVEL_DBG=y` makes the backend log one timestamped line per
   record (`Storing data in file ... at offset ...`), so per-record cost is a difference of two
   *device* timestamps with nothing on the host competing with the thread being measured. Records
   were added in completion-driven bursts of 8 — see `docs/common/dev_workflow.md` for why no timed
   producer is safe. The numbers below are medians over 200-record windows.

   **Cost 1 — the directory lookup, which grows linearly.** `fs_open()` per store is O(entries):

   | Records | 1200 | 2400 | 3600 | 4800 | 5000 | 6000 | 8000 | 10000 | 12000 |
   |---|---|---|---|---|---|---|---|---|---|
   | Median (ms) | 202 | 274 | 341 | 406 | 163 | 221 | 340 | 437 | 530 |

   Two straight segments — **57 ms per 1,000 records** before the drop, **52 ms** after — with one
   drop back to the floor at ~5,000. The drop did **not** repeat at 10,000, so it is not periodic and
   nothing should be designed around it. Taking the pessimistic reading (no further drop), the median
   at the 25,000 cap is **~1.2–1.5 s**. That is the figure to plan against.

   **Cost 2 — LittleFS metadata compaction, which does not grow.** About **3.0–3.7 %** of stores
   stall for 8–35 s (worst single write observed: 43.9 s), flat from 100 files to 2,400 files. These land mid-file as often as at file
   boundaries, so they are metadata-pair compaction, not file creation, and they are bounded by the
   metadata pair rather than by the directory. Mean stall drifts up only mildly (14.3 s → 17.5 s
   across the run).

   **Aggregate, including stalls: 0.79 → 1.11 s per record** over the same span. At the FW-7 cadence
   of 10–30 s that is a **3–11 % duty cycle** (0.79 s at 30 s through 1.11 s at 10 s), so FW-6 is not
   latency-bound and the one-file-per-block
   layout stands as built. What the measurement does rule out is any *bursting* producer: the tail,
   not the median, is what a producer has to survive, and nothing may publish faster than the storage
   thread retires — see the zbus net_buf hazard in `app/overlay-survey.conf`.

   One anomaly is recorded so it is not re-derived as a filesystem property: records 600–1,000 sat on
   a flat ~1,230 ms plateau, 5× the surrounding trend. That window is the first ~16 minutes of
   uptime, when the modem is attaching and the cloud module is retrying CoAP; it never recurred over
   the following 2.5 hours. It correlates with boot-time contention, not with record count.

   **Acted on: the LittleFS cache went from 64 to 256 bytes.** An 816-byte record through a 64-byte
   cache is thirteen program operations, and the directory metadata `fs_open()` walks is read
   through the same cache. Re-running the identical fill on an empty partition:

   | Records | 1200 | 2400 | 3600 | 4800 | Growth | At 25,000 |
   |---|---|---|---|---|---|---|
   | cache 64 | 202 | 274 | 341 | 406 | 57 ms/1k | ~1560 ms |
   | cache 256 | 166 | 214 | 268 | 307 | 41 ms/1k | ~1130 ms |

   **18–24 % faster per store, 29 % off the growth rate, for 1,152 bytes of RAM** — 384 B of bss
   (`read_buffer_0` and `prog_buffer_0`, 0x40 → 0x100 each) and 768 B of noinit (the file-cache heap,
   `(256+32)*4 − (64+32)*4`), measured from the two link maps; text and data are unchanged. It does nothing for cost 2 — the stall rate and magnitude are identical
   in both builds — which is consistent with the two costs being independent. Now in
   `overlay-survey.overlay` (the `&lfs1` property) plus `overlay-survey.conf`
   (`CONFIG_FS_LITTLEFS_CACHE_SIZE`); both are required, and changing one alone boot-loops the
   device with `-ENOMEM` on the second header file.

   If this ever does need fixing further, the lever is the backend's file layout — subdirectories,
   so the lookup is not O(total entries) — not the slot size and not more cache.

### FW-7 — Cadence ✅

`CONFIG_APP_SAMPLING_INTERVAL_SECONDS` defaults to 600 s (`app/src/Kconfig.main:8-13`) — far too
slow for driving. Before this, `survey_capture_request()` had exactly one caller in the whole
codebase — the `survey capture` shell command — so a headless device driving around with no
console attached would record nothing, ever. That gap was more fundamental than cadence tuning
and is what this closes.

**Not** built on `main.c`'s existing trigger machinery as originally sketched
(`trigger_sampling()`, `timer_sample_data_work`) — that path routes through the
`LOCATION_SEARCH_TRIGGER` zbus message, which `CONFIG_APP_SURVEY_CAPTURE_OWNS_SEARCH` already
discards for survey builds, so reusing it would mean plumbing a message through a channel built to
throw it away. Instead, `survey_capture.c` owns a self-rescheduling `k_work_delayable`
(`cadence_work`) that calls `survey_capture_request(SURVEY_PROFILE_FAST)` directly and reschedules
itself from its own handler rather than using `k_timer`'s periodic mode — a cycle that runs long
(GNSS can take up to `SURVEY_CAPTURE_WORST_CASE_SECONDS`) is a dropped tick, not a queued one, so
the configured interval is a floor on the gap between cycle *starts*, not a strict period that
would otherwise drift short under `-EALREADY` retries.

- `CONFIG_APP_SURVEY_CAPTURE_INTERVAL_SECONDS` (Kconfig, default 20 s, range 5–3600) sets the
  boot-time interval within the 10–30 s target.
- `survey interval [seconds]` (shell) reads or changes it at runtime with no reboot, per FW-7;
  values outside 10–30 s are accepted with a warning rather than rejected, since a bench session
  driving one cycle at a time deliberately sets this far outside the driving target.
- `CONFIG_APP_SURVEY_CAPTURE_CADENCE_AUTOSTART` (hidden Kconfig, default y) exists purely so
  `tests/module/survey_capture` — which links `survey_capture.c` whole and would otherwise really
  run this timer against wall-clock time during the suite — can leave it undefined and compile the
  autostart `SYS_INIT` out, keeping the suite deterministic.

### FW-8 — Export over USB (Web Serial) ✅

Primary path: **USB CDC**, driven from a browser via Web Serial. No LTE upload.

**Built on MCUmgr's `fs_mgmt` group instead of a purpose-built batch-session protocol.** The
LittleFS backend already stores records as a fixed 816-byte stride in files under
`/att_storage`, so a host that can read those files byte-for-byte can slice records out of them
with arithmetic alone — see `app/overlay-survey-export.conf`'s header comment for the full
reasoning. This gets download-with-offset (resume is a property of the protocol, not something
this application implements and tests), whole-file CRC32 verification, and file-size reporting
for free, all against a maintained upstream implementation rather than a hand-rolled one:

- `CONFIG_MCUMGR_GRP_FS_FILE_ACCESS_HOOK` + `survey_export.c` make the filesystem **read-only and
  scoped to `/att_storage`** over this transport — there is no upload primitive reachable from
  USB, so a non-technical operator's browser page cannot be turned into a write path by accident.
  "Clear" is not exposed here at all; it stays a console-only, explicit-confirmation operation.
- Length-prefix and checksum: each record is length-prefixed on flash (unchanged from CP5), and
  each downloaded data file is verified against the device's own `fs_mgmt` CRC32 before the host
  trusts its bytes — see `scripts/survey_export.py::verified_download`.
- Both transports enabled at once, so a non-technical operator never has to know which
  `/dev/cu.usbmodem*` is which: SMP-over-shell on uart0 (proven-good, log-interleaved) and a
  dedicated SMP-over-UART port on uart1 at 1 Mbaud (the shipped path, no interleaving). See
  `app/overlay-survey-export.conf` and `app/overlay-survey-export.overlay`.
- **Known limitation, accepted for this pass:** there is no session header. `struct
  survey_record_session` and its encoder (`survey_record_session_encode()`) exist and are unit
  tested, but nothing calls them on this path yet. This is *not* a modem-version plumbing
  problem — `AT+CGMR` is a plain, already-documented AT command (`nrf_modem_at_cmd(buf, len,
  "AT+CGMR")` is all it takes), and `network.c`'s `state_running_entry()` calls
  `nrf_modem_lib_init()` unconditionally before any LTE connection attempt, so the modem answers
  AT commands within the first seconds of boot, not gated on network registration or a capture
  cycle. The real gap is that the survey module has no "session started" boundary to call it
  from at all (no `survey_store_init()`, no equivalent SYS_INIT hook) — the store is written to
  lazily, per capture, with no single place a once-per-boot session header naturally belongs.
  Closing this means adding that boundary and deciding where the session header lives on flash
  and how both export clients (`survey_export.py`, `web/export.html`) discover and prepend it —
  real, scoped work, just smaller and different than a missing modem accessor. A session header
  is a follow-up, not a CP8 blocker.
- Chunk size and buffer sizing were measured, not guessed, against real RAM pressure and a real
  crash (a 2 KB download chunk on a 2 KB workqueue stack) — see `app/overlay-survey-export.conf`.

**Verified early, as flagged:** USB on Thingy:91 X is on the nRF5340 connectivity bridge, and both
the shell and a dedicated uart1 CDC port answer SMP correctly (see
`app/overlay-survey-export.overlay`'s header comment for how uart1 was routed there without
colliding with modem tracing, which also wants it).

### FW-9 — Field usability

Operators are employees driving vehicles, with no console attached.

- LED status must distinguish: searching, capture OK, GNSS fix poor/absent, storage full, error.
  Reuse the existing `led` module.
- Must run headless from cold boot with no host interaction.
- Keep the task-watchdog coverage the template already provides on every module thread.

**All 5 states are now implemented, without changing the `led` module.** The earlier CP10 note
below assumed closing the gap needed the module to expose more than "set this pattern" — it
didn't: the 5 states are temporally exclusive (a cycle is either searching or finished, and a
finished cycle is in exactly one of the other 4), so `led`'s existing last-write-wins semantics
are already sufficient. `survey_capture.c`'s `survey_led_signal()` takes an `enum
survey_led_state` and publishes one of 5 dim (~55/255) colors via `led_chan`, still with
`repetitions = -1` so the pattern holds until the next cycle overwrites it:

- **SEARCHING** (blue, fast blink) publishes at the start of every cycle, so a cycle that hangs
  reads as "still searching" rather than showing the previous cycle's stale result.
- **CAPTURE_OK** (green, slow blink) — a record was stored and at least one GNSS bracket had a fix.
- **GNSS_POOR** (amber, slow blink) — a record was stored, but neither bracket got a fix. The radio
  observation is saved regardless (see `survey_store_publish_record`'s `-EINVAL` rationale), but
  the operator should know positioning is degraded. Amber is `{red: 55, green: 45}` rather than a
  duller green — pre-commit review flagged the original `{red: 55, green: 25}` as reading like "dim
  red" next to ERROR at a glance, which matters since the two states call for different operator
  actions (move to open sky vs. something is actually broken).
- **STORAGE_FULL** (magenta, slow blink) — the survey storage type is at capacity under
  `APP_STORAGE_FULL_STOP`, so further records are being silently dropped. Checked ahead of
  GNSS_POOR/CAPTURE_OK, since an operator who can only see one color needs to know recording has
  stopped before they need to know fix quality.
- **ERROR** (red, fast blink) — a cycle produced nothing to store, or the store failed for a reason
  other than being full.

The gap this closes was more than cosmetic: before this pass, `survey_store_publish_record()`'s
return value reflected only whether the record reached the storage module's queue, not whether the
asynchronous flash write that follows actually succeeded — `storage.c`'s `handle_data_message()`
logs a failed `backend->store()` but never propagates it. A full partition under
`APP_STORAGE_FULL_STOP` therefore showed **green** (success) on the exact cycle whose record was
silently dropped. Rather than editing upstream-owned `storage.c` to propagate that failure, the fix
is proactive and additive: `survey_store_is_full()` (new, in `survey_store.c`) asks the backend's
current record count synchronously via the existing `storage_backend_get()->count()` API and
compares it against `CONFIG_APP_STORAGE_MAX_RECORDS_PER_TYPE`, so the LED can be right before a
store is even attempted. It is a no-op under `APP_STORAGE_FULL_OVERWRITE`, which never actually
stops accepting records. Covered by two new cases in `tests/module/survey_store` (one per
full-behaviour build) and a stub in `tests/module/survey_capture`, which fixes it to `false` since
that suite's own concern is step sequencing, not the storage-full path. A backend read error
(negative count) is treated as full rather than as "not full" — this function exists so the LED
never claims success when it can't confirm capacity, and failing open would defeat that.

`survey_store_is_full()` is the first caller of a storage backend function from a thread other than
storage's own (it runs on the survey capture thread). Pre-commit review caught that this raced with
`storage.c`'s own thread over the LittleFS backend's permanently-open header file handle
(`type_state[idx].header_file` in `littlefs_backend.c`) — both `read_storage_file_header()` and
`write_storage_file_header()` do an unsynchronized `fs_seek` then `fs_read`/`fs_write`, and nothing
prevented the two threads' seek+read/write pairs from interleaving. A torn read is not just
cosmetic here: the header's `write_offset - read_offset` is an unsigned subtraction, so a torn read
could underflow to a huge value and make `survey_store_is_full()` report "definitely full" when it
isn't, or worse, a stomped seek position between the two threads could corrupt the on-flash header.
Fixed with a `k_mutex` added directly in `littlefs_backend.c` (upstream-owned; this is a deliberate,
minimal exception to preferring additive-only changes, made because the race is a genuine
correctness bug rather than a place upstream divergence was worth avoiding), guarding the
seek+read/seek+write+sync pairs in both functions. Re-verified: `tests/module/survey_store`,
`tests/module/survey_capture`, and `tests/module/storage/littlefs_backend` all still pass, and the
survey-overlay firmware build still succeeds (see updated numbers below).

Not hardware-verified: this pass had no Thingy:91 X available, so verification is build +
`tests/module/survey_capture` + `tests/module/survey_store` (native_sim/Twister) only. The color
and blink-rate choices have not been visually confirmed on a real LED.

`main.c` publishes to `led_chan` on its own schedule for the asset-tracker's own states (blue
"sampling", green "sending", red "disconnected", purple "FOTA download") — all four ungated
against survey mode before this change, so a survey build's LEDs were showing whichever of the two
modules published most recently rather than either coherently. Fixed by gating all four behind
`!IS_ENABLED(CONFIG_APP_SURVEY)` in `main.c` (`trigger_sampling()`, `cloud_send_now()`,
`disconnected_waiting_entry()`, `fota_entry()`). FOTA was initially left ungated on the theory that
purple doesn't collide with survey's green/red palette, but pre-commit review caught that it does
in practice: FOTA's pattern is meant to hold `repetitions = -1` for the whole download, and the
FW-7 cadence timer republishes green/red to the same channel every `cadence_interval_s` (≤30 s), so
an ungated purple pattern would be overwritten within one cadence tick and never actually be
visible. Survey builds have no FOTA indicator; this is a known gap, not a design choice.

### FW-10 — Data hygiene

- **Never call `nrf_cloud_coap_location_get()`.** Prefer compiling the cloud module's location
  path out entirely so it cannot fire by accident.
- A-GNSS is allowed and recommended (faster fixes). It is the only meaningful data consumer:
  ~1–4 KB per fetch, cached — well under 1 MB/month. **Radio scanning itself uses zero cellular
  data**: `%NCELLMEAS` is an internal modem RF measurement and Wi-Fi scanning is local to the
  nRF7002. If A-GNSS is disabled, expect slower TTFF and note that in the dataset.
- **Drop locally-administered BSSIDs** (`CONFIG_APP_SURVEY_DROP_LOCAL_MAC`, default y). An access
  point whose BSSID has the IEEE 802 U/L bit set (bit 1 of the first octet) is a randomised MAC —
  a phone or laptop hotspot, a Wi-Fi Direct link, CarPlay. It re-randomises per session, so no
  positioning service can resolve it, and the survey exists to be checked against one. In a train
  carriage or a car park most of what is in the air is randomised.

  **Measured on target: 4 of 10 access points in an ordinary indoor scan were randomised — 40 %.**
  Filtering does *not* recover the real access points those four crowded out: the Location library
  caps its result set at `CONFIG_LOCATION_METHOD_WIFI_SCANNING_RESULTS_MAX_CNT` (10 on this board)
  before publishing, and `location_helper.c` rejects a longer set with `-ENOMEM` rather than
  truncating, so anything the randomised BSSIDs displaced is gone before the application sees the
  scan. Recovering it is a separate FW item — raise that cap and `CONFIG_APP_LOCATION_WIFI_APS_MAX`
  with it, and pay the heap and record-size cost.
  Filtered in `survey_obs_update()`, where every scan lands, so `survey show`, `survey hex` and the
  stored record agree; filtering in the encoder instead would leave the console listing APs that
  are not in the record. The count of what was dropped is kept and rendered, so a genuinely quiet
  location stays distinguishable from an over-eager filter. Only the U/L bit — the multicast bit
  would make the address malformed rather than unstable, and nothing has been seen to report one.

  **The count is also persisted, at CDDL key 18 (`ap-dropped`), decoded as `wifi.apDropped`.** The
  filter is destructive and the unfiltered scan lives only for the duration of one zbus callback,
  so without the count a train carriage where two thirds of the APs were phone hotspots and a quiet
  lab where none were are byte-identical in the exported dataset, and the AP density of the survey
  becomes unexplainable. Added as an **optional** key with **no schema version bump**: absent
  correctly describes both "nothing was dropped" and "this record predates the filter", so the
  records already on the device stay valid and the host decoder omits the key rather than reporting
  a fabricated zero.

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
| `survey store` | encode the cached observation and commit it to flash | CP5 ✅ |
| `survey capture [fast\|deep]` | run one full GNSS → scan → GNSS cycle and store the record | CP6 ✅ |
| `survey timing` | last measured durations for the GNSS / scan / GNSS steps | CP6 ✅ |
| `survey hex <n>` | hexdump a **stored** record by index | CP8 — see below |
| `survey profile <fast\|deep>` | force a profile, bypassing the gating logic | CP7 |

`scan` and `gnss` are separate commands because the Location library treats its method list as a
**fallback chain and stops at the first method that succeeds** — a request including GNSS returns a
fix and never scans. This is the same reason FW-2 requires sequenced requests.

**Why `survey hex <n>` moved to CP8.** The storage backend's read interface is `peek` and `retrieve`
on the head of a FIFO — there is no addressing by index, and the on-flash offset of record *n* is
only derivable from the type's header offsets, which live behind file handles the storage thread
owns exclusively. Adding random access at CP5 would mean either a second reader of those handles (a
real concurrency hazard for a nice-to-have) or a new backend API. CP8's export protocol has to walk
every stored record anyway, and resume-from-sequence gives it a real reason to address them; that is
where indexed access belongs. Until then, counts come from the upstream `att_storage stats` command.

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
| **CP3** ✅ | FW-5 CBOR encoder + CDDL + `version` field; `survey hex` encodes the cached observation | **Done.** `tests/module/survey_record` = **23/23**, full `tests/module` = **13/13 configurations**. Absent-not-zero asserted per optional field. **Measured worst case: 585 B** (10 APs + 10 neighbours + 3 GCI + both bracket fixes + all six timestamps + the dropped-AP count; 583 B before key 18 was added), asserted against the 700 B budget rather than printed. Survey build text 384,980 / data 148,794 / bss 186,971; default build also verified. | `survey hex` prints a decodable dump |
| **CP4** ✅ | HOST-1 decoder `scripts/survey_decode.py` (reads CP3 output) | **Done.** `tests/host` = **58/58**, stdlib only. Golden files are the **exact bytes CP3's encoder emitted**, lifted from the suite by `scripts/survey_fixture.py`, so encoder and decoder are proven against each other rather than against two readings of the CDDL — which is how the `time-base` enum disagreement below was caught. Covers indefinite-length CBOR (what zcbor actually emits), version dispatch, RSRP/RSRQ and Wi-Fi frequency conversion, interpolation, and the quality gate. | Decode a real `survey hex` dump from the device |
| **CP5** ✅ | FW-6 storage: `SURVEY` record type on the LittleFS backend, 24 MiB partition (`overlay-survey.overlay`), `APP_STORAGE_FULL_STOP`, `survey store` | **Done.** `tests/module/survey_store` = **11 tests × 2 configurations** (`.stop` / `.overwrite`, 0 failures); full `tests/module` = **15/15 configurations**; `tests/host` still **58/58**. The suite drives the real path — `survey_store_publish()` → zbus → storage thread → LittleFS — re-decodes a record read back off flash rather than comparing it to the buffer that wrote it, and reads the ring's offsets back out of the header file *through the filesystem* to show they are on flash rather than cached in RAM. **816 B fixed-stride slot**, chosen to pack the 4096 B erase block 5-up (0.4 % waste). Cap **25,000 records ≈ 2.9 days @ 10 s / 5.8 @ 20 s / 8.7 @ 30 s** — set by the file count, not the byte count; see FW-6 above. Survey build text 412,408 / data 155,998 / bss 191,811 (+27 KB of filesystem code over CP3); default build also verified (text 400,732 / data 155,597 / bss 191,334) since four of the touched files are upstream-owned. Review caught two data-destroying defects, both fixed here: `cloud.c` consumed (deleted) storage records it had no handler for, and `storage.c` re-announced the buffer threshold after a failed store, which under `FULL_STOP` meant one "send now" per capture forever. | **Run on target.** `att_storage stats` reports the SURVEY count, records survive a power cycle, and the metadata block cost is measured — see FW-6 open question 1 above, which this closes: **42.14 blocks per 200 records**, held to within a block from 743 to 12,007 records, which extrapolates to **5,279 of 6,144 blocks at the 25,000 cap — 14.1 % spare**. 25,000 records per type stands and the 20,000 fallback is not needed. The second measurement (store latency at thousands of files) also surfaced a real defect, documented in `app/overlay-survey.conf` and `docs/modules/storage.md`: a producer that outruns the storage thread drains the shared zbus `net_buf` pool, and zbus asserts on the failed allocation instead of returning an error, so the device reboots. Normal capture cannot reach it (one record in flight, 10–30 s apart, against a 12,288-byte heap that holds a dozen — the heap binds well before the 64-entry descriptor pool), and the three available fixes were each measured to be worse than the problem — including `CONFIG_ZBUS_MSG_SUBSCRIBER_NET_BUF_POOL_ISOLATION`, which is broken upstream in Zephyr 4.4 (`zbus.h:307` is missing the `CONFIG_` prefix, so the per-channel pool stays `NULL` and the first publish boot-loops the device). The build keeps upstream defaults and contains the hazard in the bench-only `survey fill` |
| **CP6** ✅ | FW-2/FW-4 paired capture orchestrator: GNSS bracket + scan, all six timestamps; `survey capture`, `survey timing`; `CONFIG_APP_SURVEY_CAPTURE_OWNS_SEARCH` suppresses the default `LOCATION_SEARCH_TRIGGER` so nothing else competes for the modem | **Done.** New `tests/module/survey_capture` = **16/16 named tests**, verified individually in the handler log rather than trusted from Twister's one-case-per-suite summary. Full `tests/module` = **16/16 configurations** (608 s); `tests/host` still **58/58**. The suite needs `CONFIG_EVENTS=y` and — uniquely — `CONFIG_NATIVE_SIM_SLOWDOWN_TO_REAL_TIME=y`, because it asserts on measured durations rather than on order alone. The orchestrator runs on its own lowest-priority thread with the observation and record in bss, waits each step out on timeout rather than issuing `LOCATION_SEARCH_CANCEL` (which cannot cancel a Wi-Fi scan and returns `-EBUSY`), and records a timestamp delta that does not fit `int32_t` as **absent** rather than as a wrapped number the host would silently interpolate against. Review of the first attempt found the suite could not fail if the sequencing were deleted: the fake Location library was a zbus *message subscriber*, which **queues** an overlapping trigger where the real module **discards** it, so `overlap_detected` was unreachable and the start/finish ordering assertion tautological. The fake now serves asynchronously on a `k_work_delayable` that holds a `busy` flag across dequeues, with a gap between publishing the result and `LOCATION_SEARCH_DONE` — which is also where the real library's result-before-done hazard lives. Proven by mutation: deleting `wait_for_idle()` from the success path fails **5** tests. Builds: survey overlay FLASH 71.05 % / RAM 87.03 %, default build 68.97 % / 82.47 % (`location.c` is upstream-owned, so both matter), plus a third build with `CONFIG_APP_SURVEY_SHELL_FILL=y` — that option is `n` by default and neither of the other two compiles `survey fill`, which is how a stale `store_sequence++` survived the sequence-counter consolidation. Second review verdict was NOT APPROVED on the main.c blocker above; re-verified after the fixes. A third review then found the fix itself was a should-fix: the synthetic DONE is not private to main.c. It reaches `survey_capture`, where it would end a step early with the radio still working (a record carrying one step's timestamps around the next step's measurements), and via the location module's own subscriber FIFO it could return that module to INACTIVE mid-search, so the next request answers `-EBUSY` and `SEND_FATAL_ERROR()` **resets the device**. Closed on both sides: `location.c` publishes the synthetic DONE only when no cycle is in flight, and the capture listener drops any DONE that arrives before its own search has reported started. A fourth review approved with nits and found the first placement of that drop insufficient -- clearing the event after `run_step()`'s one-second started handshake leaves a stray DONE latched whenever started is slower than that -- so the gate moved into the zbus listener, where the ordering is the channel's rather than the thread's. Proven by mutation: with the gate removed, `test_a_stray_done_before_started_does_not_end_the_step` fails. Also closed: `APP_SURVEY_CAPTURE` now **selects** `LOCATION_DATA_DETAILS`, which the started handshake silently depends on; `survey store` refuses mid-cycle like `survey capture` does; a `methods_count == 0` divide guard. Final verification: three builds clean (survey FLASH 71.05 % / RAM 87.03 %, fill 71.19 % / 87.03 %, default **unchanged** at 68.97 % / 82.47 %, which is what proves the guarded include and the new DONE cost the non-survey build nothing), `tests/module` 16/16 configurations in 530 s, `tests/host` 58/58, A fifth review, on the changes the fourth one prompted, found a should-fix in the publisher-side gate itself: `survey_capture_busy()` is held from admission until *after* the record is written, but the cycle's last `LOCATION_SEARCH_DONE` is published well before that, so a suppressed application trigger arriving during the store -- tens of seconds once the partition fills -- got no DONE and none was coming. That is the same main.c stall the second review found, reintroduced through the fix for the third. "Busy" was two questions: the shell asks whether work would interleave with the cycle, `location.c` asks whether the *radio* is spoken for. Split into `survey_capture_radio_busy()`, raised by the requester (not by the lowest-priority capture thread, whose scheduling delay would reopen the reordering window) and dropped when step 3 ends. Proven by mutation: the store fake samples both predicates on the capture thread at the moment the store begins, and deleting the clear fails `test_the_radio_is_reported_free_before_the_store_begins`. Also closed: a `while (steps_seen < 3)` poll with no deadline, which would have turned the very regression it guards against into a Twister hang that takes the other fifteen named results with it. Final verification after the fifth review: three builds clean (survey FLASH 71.06 % / RAM 87.03 %, fill 71.20 % / 87.03 %, default still **unchanged** at 68.97 % / 82.47 %), `tests/module` 16/16 configurations in 552 s, `tests/host` 58/58, harness loopback 6/6. | **Run on target. 22 passed, 0 skipped, 0 failed**, on a partition holding 5,053 records. Earlier runs of the same suite skipped `gnss_fix` and the capture ordering assertion for want of sky view; the final image was re-run with a view and both passed (153.0 s and 10.4 s), so no test in the suite has gone unexecuted on the code as committed. The ordering test skips indoors only after requiring `accepted == 3`, so the sequencing claim -- the thing the stray-DONE and radio-busy gates protect, since either failure drops a step and lowers that count -- is enforced on every run regardless of sky view. Warm cycle **65.7 s** with a cold first fix (gnss_before 59255 ms, scan 5387 ms, gnss_after 1089 ms, store 2 ms, total 65740 ms); a fully warm cycle completes in **10.4 s**, and an indoor cycle that never fixes costs 367 s and still stores scan-only. All comfortably inside the 60 s cadence once the first fix is paid for. A second review found a blocker no test covered: `LOCATION_SEARCH_DONE` is the **only** exit from main.c's two SAMPLING states, so `OWNS_SEARCH` swallowing the trigger left main in SAMPLING with its sample timer already stopped -- periodic power and environmental sampling dead, and silent, because main feeds its watchdog *before* waiting on zbus rather than after. The suppression branch now answers with a synthetic DONE, which is honest: no search ran, so a search taking zero time is what happened. Proven in the boot log -- `disconnected_sampling_entry` at 01.993 followed by `disconnected_waiting_entry` 2.1 ms later, where the pre-fix log had nothing at all. Five further findings closed: per-method timeouts now **divide** the step budget instead of handing each method the whole of it (a two-method scan could run 120 s inside a 60 s step, and the orchestrator would publish step 3 into a busy radio); a `BUILD_ASSERT` on `CONFIG_TASK_WDT_CHANNELS >= 9` turns the ninth-channel boot loop into a compile error naming the file to edit; the watchdog margin assert now covers the trigger-publish and STARTED waits it omitted; the wind-down budget is printed by the device rather than recomputed in Python; and two unit assertions that could not fail (`started_ms >= finished_ms`, structurally true because the fake refuses overlapping triggers) were replaced with a bound on the inter-step gap. Kconfig ints gained `range`s. Every function in the survey host tooling and both harnesses is now type-annotated |
| **CP7** | FW-2 profiles FAST/DEEP + gating | Unit test: gating decisions across a speed/power truth table; assert DEEP requests GCI | `survey profile deep` yields multiple full-identity GCI cells; FAST does not |
| **CP8** ✅ | FW-8 export over `fs_mgmt` (MCUmgr): `scripts/survey_smp.py` (SMP serial framing + fs_mgmt CBOR + `FsMgmt.stat/checksum_crc32/download`) and `scripts/survey_export.py` (CLI, walks the ring buffer, CRC32-verifies each file, writes a length-prefixed CBOR stream); `scripts/survey_decode.py` extended to parse that stream | **Done.** `tests/host` = **103/103** (up from 58 at CP4), stdlib only — `import serial` is deferred into `SmpSerial.__init__` so the framing, CBOR and `FsMgmt` stay testable via a fake transport without pyserial installed. `survey_export.py` itself carries PEP 723 metadata (`# /// script` dependency block) so `uv run scripts/survey_export.py ...` provisions pyserial into an isolated environment on first run — no system-Python `pip install` fight. Framing verified against Zephyr 4.4's own `fs_mgmt.c`/`crc32_sw.c` source rather than assumed field names; the CRC32 used is confirmed bit-identical to `zlib.crc32`. No firmware changes this checkpoint — CP6's on-flash layout is read as-is. Known limitation: no session header (see FW-8). | **Run on target.** `uv run scripts/survey_export.py --port /dev/cu.usbmodem105 --baud 1000000 ...` against a real Thingy:91 X: `--stats-only` reported 5,083 live records; a full export downloaded and CRC32-verified all of them, and every one decoded cleanly (5,083/5,083, all bytes consumed, no trailing garbage). This run also caught a real client bug the host tests never could — mcumgr's zcbor encoder returns **indefinite-length** CBOR maps (additional-info 31, terminated by a `0xFF` break byte) on real fs_mgmt responses, which the hand-rolled decoder in `survey_smp.py` didn't handle; fixed, host suite still 103/103 green. (Also observed: the record `sequence` field resets on device reboot rather than staying globally monotonic — real device behaviour, not a decoder defect; `--since` filtering does not assume global monotonicity, so this is not a problem for it.) |
| **CP9** ✅ | HOST-2: `web/export.html`, a self-contained Chrome/Edge Web Serial page — connect, run the export, show progress and record count, save to a local file (browser download, no upload endpoint), surface CRC/length failures in a visible log | **Done.** The framing/CBOR/CRC16/CRC32/ring-walk port was extracted and run under Node against the same vectors as `tests/host/test_survey_smp.py` and `test_survey_export.py` (CRC16-XMODEM and CRC32 known vectors, multi-frame packet round trips, CBOR uint-width round trips including ≥2³², a two-file ring-buffer walk with per-file download caching) — all passing. Pre-commit review found and fixed two UI-state bugs (Disconnect left enabled during an in-flight export, racing the transport's pending read/write; button state not reset when the post-connect record-count read fails) and hardened the CBOR 8-byte-width decode path against silent 32-bit truncation (unreachable today, but wrong-not-erroring is worse than throwing). No "clear" control exists on the page. Opened in real Chrome via chrome-devtools MCP against the connected device: page loads with no console errors of its own, and the `#unsupported` Web-Serial-not-available branch correctly stays hidden (Chrome supports it). Clicking Connect correctly invokes the real `navigator.serial.requestPort()` and opens the OS-native device chooser — which is what real-browser testing caught and Node testing structurally could not: with no filter, that chooser listed every serial-capable device on the machine, including Bluetooth-serial peripherals (AirPods, a mouse), not just the Thingy:91 X. Fixed with a `usbVendorId: 0x1915` (Nordic Semiconductor, confirmed via `ioreg -p IOUSB -l` against the connected device) filter on `requestPort()`. Storage capacity — record count against `RECORDS_PER_TYPE` (25,000, mirroring the firmware's `CONFIG_APP_STORAGE_MAX_RECORDS_PER_TYPE`) — is now shown on connect (`"5083 / 25000 slots used (20.3%)"`) and flagged in red past 90%, since FW-6's default `APP_STORAGE_FULL_STOP` means a full partition silently stops recording rather than wrapping the oldest data — an operator sending the device out for a trip needs that visible before leaving, not discovered afterward as a gap in the data. No time-to-full estimate: the capture interval is runtime-settable (FW-7's `survey interval`), so this page has no fixed cadence to project from without a new device read. | **Done.** Full export completed end-to-end through the real browser UI against the connected device: page reported "Connected. 5083 record(s) stored," Start Export ran to "Done: 5083/5083 record(s) exported to `survey_export_2026-08-15T05-45-12-998Z.bin`," 0 failed, 0 console errors. (The OS-level device-chooser sheet itself is outside what chrome-devtools MCP can drive, so a human click selected the device; everything from Connect onward — the record-count read, the export loop, and the download — ran unassisted and was observed by the tooling.) The downloaded 659,098-byte file was independently re-verified against `scripts/survey_decode.py`: decodes cleanly, `records in: 5083`, `decode_error: 0`. Of those, the default quality gate (`require_bracket=True`) passed only 40 through, dropping 5043 for `missing_bracket` (no paired before/after GNSS fix) and 14 for `uptime_timebase` — expected on this dataset (a `--allow-missing-bracket`/scan-only pass recovers the rest) and a GNSS-bracket-completion-rate question for the field data itself, not a defect in the export or decode path. |
| **CP10** ⚠️ partial | FW-7 cadence, FW-9 LEDs, FW-10 hygiene | `tests/module/survey_capture` passes with the cadence timer linked in (`CADENCE_AUTOSTART` compiled out so the suite's own assertions aren't raced by a real timer firing mid-test); no dedicated reschedule-maths test yet. `tests/module/survey_store` passes both full-behaviour configurations, now including `survey_store_is_full()` coverage, plus `tests/module/storage/littlefs_backend` (re-verified after the header-file mutex fix below). Survey build FLASH 72.95 % / RAM 91.99 %, default build unchanged at 68.97 % / 82.47 % (confirms the LED gating is inert outside survey mode). | **Not yet run on target.** FW-7 (auto-capture, runtime-settable interval) and FW-9's 5-state LED model are implemented and build-verified but not hardware-verified — see FW-9 above for the color/pattern scheme, the header-file race pre-commit review caught and its fix, and why no `led` module change was needed. FW-10 hygiene not started. RAM headroom is getting tight (91.99 % on the survey build) and is worth watching on future additions. |

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

**Eliminated at CP6.** `CONFIG_APP_SURVEY_CAPTURE_OWNS_SEARCH` (default y) makes `location.c` drop
`LOCATION_SEARCH_TRIGGER` outright, so the survey orchestrator is the only thing that ever issues a
location request. The drop happens in the location module rather than by not publishing from
`main.c`, so the suppression is one decision in one place and `main.c` stays upstream. Dropped
triggers are counted and visible in `survey show` alongside accepted ones. A measured side effect:
the radio hardware tests no longer need retries, because the startup sample is no longer competing
for the modem's single RF front end.
