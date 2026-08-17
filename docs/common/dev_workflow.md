# Development workflow (macOS bench)

How to build, flash and exercise this fork on a Thingy:91 X from a macOS host. Every command here
has been run successfully on this bench; the failure modes described are ones actually hit, not
hypothetical.

Upstream's [Getting Started](getting_started.md) covers the nRF Connect for VS Code path. This
document covers the command-line path plus the things specific to this fork: the survey overlay, the
serial-recovery flash, the modem update, and running the unit tests on macOS.

## The environment

| Thing | Value |
|---|---|
| west workspace | `/Users/tyler/junk/ncs-3.4.0` |
| NCS / SDK version | `v3.4.0` (pinned in `west.yml`) |
| Board target | `thingy91x/nrf9151/ns` |
| Modem firmware | `mfw_nrf91x1_2.0.4` |
| Repo | `/Users/tyler/junk/Asset-Tracker-Template` |

**Build output lives in the workspace, not in the repo.** Build directories are created under
`/Users/tyler/junk/ncs-3.4.0/` (for example `build-att-survey`), which keeps the repo clean and
keeps `west` happy about its workspace root.

### Rule 1: west only runs inside the toolchain manager

`west build` invoked from a plain shell fails at CMake configure time with:

```
CMake Error ... Missing jsonschema dependency
```

That is not a real missing dependency — it means the command ran outside the NCS toolchain's Python
environment. Always wrap it:

```sh
nrfutil toolchain-manager launch --ncs-version v3.4.0 -- bash -c '
    cd /Users/tyler/junk/ncs-3.4.0 && west build ...
'
```

### Rule 2: zsh does not word-split unquoted variables

This is the single most repeated mistake on this bench. In `bash`, `SC="nrfutil foo"; $SC bar` runs
`nrfutil foo bar`. In `zsh` it tries to execute a program *literally named* `nrfutil foo`, and fails
with `no such file or directory`.

Use a shell function instead of a variable:

```sh
sc() { nrfutil toolchain-manager launch --ncs-version v3.4.0 -- "$@"; }
sc python3 scripts/survey_console.py --port /dev/cu.usbmodem102 "survey show"
```

## Building

Run from the workspace root, inside the toolchain manager.

**Survey build** — the radio-survey exporter, which is what gets flashed for capture work:

```sh
nrfutil toolchain-manager launch --ncs-version v3.4.0 -- bash -c '
    cd /Users/tyler/junk/ncs-3.4.0 &&
    west build -b thingy91x/nrf9151/ns --sysbuild -d build-att-survey \
        ../Asset-Tracker-Template/app -- \
        -DEXTRA_CONF_FILE="overlay-survey.conf;overlay-survey-export.conf" \
        -DEXTRA_DTC_OVERLAY_FILE="overlay-survey.overlay;overlay-survey-export.overlay"
'
```

All four overlays are required. `overlay-survey.overlay` grows `littlefs_storage` from 1 MiB to
24 MiB, which is what `CONFIG_APP_STORAGE_MAX_RECORDS_PER_TYPE=25000` in `overlay-survey.conf` is
sized against. Building with `overlay-survey.conf` alone links and flashes, then fails an
`__ASSERT` inside the LittleFS backend at boot — the assert message names the block counts, so it
is diagnosable, but the device is dead until it is reflashed. Confirm the partition took by
checking the generated devicetree rather than trusting the command line:

```sh
grep -A3 'littlefs_storage:' build-att-survey/app/zephyr/zephyr.dts
# reg = < 0x4d2000 0x1800000 >;
```

`overlay-survey-export.conf`/`.overlay` carry every MCUMGR/fs_mgmt Kconfig — leave them off and
the build still links and flashes cleanly, but `CONFIG_MCUMGR` (and everything under it, including
`CONFIG_UART_MCUMGR`) is silently absent, so `survey_smp.py`/`survey_export.py`/the web exporter
all fail with `timed out waiting for a response to group 8 command 0` against a device that
otherwise looks completely healthy (shell responds, capture cadence logs, `kernel reboot cold`
shows a clean boot). This is the same "unmet `depends on` vanishes without a word" failure mode
described below, so the fix is the same: grep the generated `.config` before trusting a build that
merely succeeded:

```sh
grep -E '^CONFIG_MCUMGR=|^CONFIG_MCUMGR_GRP_FS=|^CONFIG_UART_MCUMGR=' build-att-survey/app/zephyr/.config
```

**Fill build** — the survey build plus the bench load generator, which nothing else compiles:

```sh
nrfutil toolchain-manager launch --ncs-version v3.4.0 -- bash -c '
    cd /Users/tyler/junk/ncs-3.4.0 &&
    west build -b thingy91x/nrf9151/ns --sysbuild -d build-att-fill \
        ../Asset-Tracker-Template/app -- \
        -DEXTRA_CONF_FILE=overlay-survey.conf \
        -DEXTRA_DTC_OVERLAY_FILE=overlay-survey.overlay \
        -DCONFIG_APP_SURVEY_SHELL_FILL=y
'
```

`CONFIG_APP_SURVEY_SHELL_FILL` is `n` by default, so `survey fill`'s code is invisible to both builds
above. Deleting the file-static `store_sequence` broke it and neither build noticed — the compiler
reports one undeclared-identifier error per function and the fill function was preprocessed away
entirely. Any refactor that touches something `survey fill` uses needs this third build. The same
applies to every other default-off option in `Kconfig.survey`: "it builds" means "the configurations
you built" and no more.

**Default build** — plain asset-tracker, `CONFIG_APP_SURVEY=n`:

```sh
nrfutil toolchain-manager launch --ncs-version v3.4.0 -- bash -c '
    cd /Users/tyler/junk/ncs-3.4.0 &&
    west build -b thingy91x/nrf9151/ns --sysbuild -d build-att-default \
        ../Asset-Tracker-Template/app
'
```

Build **both** before committing. The survey overlay compiles out the cloud module's location path,
so a change to `cloud_location.c` is not covered by the survey build at all — only the default build
proves it still compiles. Check for the object file if in doubt:

```sh
find /Users/tyler/junk/ncs-3.4.0/build-att-default -name 'cloud_location.c.obj'
```

Add `-p always` when changing Kconfig or devicetree. Sysbuild caches aggressively and a stale cache
produces confusing results — a changed Kconfig default in particular may not be picked up.

Use `-p always`, not a bare `--pristine`: the long form takes a value, so `--pristine <source_dir>`
consumes the source directory as its argument and fails with `invalid choice`.

Keep the `bash -c 'cd ... && west build ...'` wrapper, or make every path absolute. Passing the
build straight to the launcher — `toolchain-manager launch -- west build ... -DEXTRA_DTC_OVERLAY_FILE=../Asset-Tracker-Template/app/overlay-survey.overlay` —
resets the working directory, and the relative overlay path then fails inside the devicetree
preprocessor rather than at argument parsing:

```
failed to preprocess devicetree files (error code 1):
  .../thingy91x_nrf9151_ns.dts;.../app/boards/thingy91x_nrf9151_ns.overlay;../Asset-Tracker-Template/app/overlay-survey.overlay;...
```

The message lists the files and says nothing about which one it could not find, so it reads like a
syntax error in an overlay that is in fact fine.

### When a BUILD_ASSERT about buffer size fires

Growing `struct location_msg` can break the storage pipe:

```
CONFIG_APP_STORAGE_BATCH_BUFFER_SIZE is too small to hold the largest
storage_data_item plus its pipe header
```

Do not guess the new value. Make the compiler print the real one by temporarily adding a bad
declaration to `app/src/modules/storage/storage.c`:

```c
char (*__size_probe)[STORAGE_MAX_DATA_SIZE] = 1;
```

The resulting diagnostic names the actual size — it prints as `char (*)[512]`. Note it is a
*warning* (`-Wint-conversion`), not an error, since Zephyr does not set `-Werror` by default, so
look for it in the build output rather than expecting the build to stop. Add the 4-byte
`struct storage_pipe_header`, raise
`APP_STORAGE_BATCH_BUFFER_SIZE` in `Kconfig.storage`, and revert the probe. The default belongs in
`Kconfig.storage`, not the survey overlay, because `struct location_msg` grows for every
configuration.

## Flashing

### Check whether hardware is attached before deciding anything is unverifiable

Do this at the **start** of a session, not when you get to a step that needs it. `nrfutil device
list` is instant and needs no toolchain wrapper:

```sh
nrfutil device list
```

```
THINGY91X_4694ACC7099
Product         Thingy:91 X UART
Ports           /dev/tty.usbmodem102, vcom: 0
                /dev/tty.usbmodem105, vcom: 1
Traits          mcuBoot, modem, nordicUsb, serialPorts, usb

Supported devices found: 1
```

`Supported devices found: 0` means no board. Anything else means the board is there and every
on-target acceptance item is available — do not write "needs hardware, not run" without having run
this. That mistake was made once already: CP5 was reported complete with on-target verification
deferred while the board had been plugged in the whole time.

The Thingy:91 X has no on-board debugger, so flashing goes through MCUboot serial recovery.

```sh
cd /Users/tyler/junk/ncs-3.4.0
nrfutil device program \
    --firmware build-att-survey/app/zephyr/zephyr.signed.hex \
    --traits mcuBoot \
    --options target=nRF91,mcu_end_state=NRFDL_MCU_STATE_APPLICATION
```

Notes:

- Use `zephyr.signed.hex`. The unsigned `zephyr.hex` will not boot — MCUboot rejects it.
- `mcu_end_state=NRFDL_MCU_STATE_APPLICATION` is what leaves the nRF9151 running the application
  instead of sitting in the bootloader. Omit it and the device enumerates but answers nothing.
- Takes roughly 40 s. `nrfutil` writes progress to a TTY, so piping the output can leave you with
  nothing on stdout — a silent run is not a failed run. Verify by talking to the device.

### Updating the modem firmware

The modem image is flashed with a different trait, and **this leaves the device unusable until you
reflash the application**:

```sh
nrfutil device program --firmware mfw_nrf91x1_2.0.4.zip --traits modem
```

`--traits modem` does **not** accept `mcu_end_state`, so when it finishes (~2 m 15 s) the nRF9151 is
still in bootloader state. Symptoms: `--identify` reports "no response" on both CDC ports, and
`nrfutil device reset --traits mcuBoot` says "operation not available". This is expected.

**Reflashing the application is a required step of the modem update, not a recovery action.** Run
the flash command from the previous section immediately afterwards.

Verify the modem version over the shell:

```sh
sc python3 scripts/survey_console.py --port /dev/cu.usbmodem102 "at AT+CGMR"
sc python3 scripts/survey_console.py --port /dev/cu.usbmodem102 "at AT%XMODEMUUID"
```

`AT+CGMR` should report `mfw_nrf91x1_2.0.4`. Cross-check `AT%XMODEMUUID` against the UUID in the
release notes — for 2.0.4 it is `11866dbb-3d51-4226-8098-59dc6b9b5a50`. Check the release notes' MD5
against the downloaded `.zip` before flashing it.

## Serial ports

The Thingy:91 X exposes two CDC ports through the nRF5340 connectivity bridge:

| Port | vcom | What it is |
|---|---|---|
| `/dev/cu.usbmodem102` | 0 | **nRF9151 application console** — the one you want |
| `/dev/cu.usbmodem105` | 1 | nRF5340 |

The numbers can change across reconnects; confirm with `nrfutil device list`, which prints the vcom
mapping, or `scripts/survey_console.py --identify`, which probes both and reports which one answers
the `survey` command.

**USB enumeration is not proof the application is running.** The bridge enumerates both ports even
when the nRF9151 is silent or stuck in the bootloader. Always confirm with an actual command.

## Driving the shell

`scripts/survey_console.py` sends shell commands over CDC and prints the reply, which makes bench
work scriptable. See [the survey module doc](../modules/survey.md) for the full command list.

```sh
sc() { nrfutil toolchain-manager launch --ncs-version v3.4.0 -- "$@"; }

sc python3 scripts/survey_console.py --identify
sc python3 scripts/survey_console.py --port /dev/cu.usbmodem102 "survey selftest"
sc python3 scripts/survey_console.py --port /dev/cu.usbmodem102 -n 2 -d 20 "survey scan"
```

`survey selftest` is the right first command after any flash: it renders a synthetic observation
with no SIM, antenna or sky view, so a sensible result proves everything from the observation cache
to the console works, and narrows a subsequent empty `survey show` down to the radio.

### The first scan after boot is usually dropped

The application takes its own location sample shortly after boot, and the location module runs
**one search at a time** — a `survey scan` issued during that window is discarded, with a warning in
the log. It looks like the command did nothing: `survey show` reports `gnss #1, scan #0`.

This has been observed on essentially every reflash. It is not a bug in the shell command. Wait for
the boot sample to finish (check `survey stats`) and re-issue the scan. A cold GNSS fix can hold the
module for minutes.

Note that `survey clear` resets the counters too, so a `clear` followed by a dropped scan reads as
`gnss #0, scan #0` and looks like a dead device when it is merely busy.

### Do not diagnose from an absence of log output

Originally written as "no log lines appear on either CDC port". That was true of the build at the
time and is no longer: `overlay-survey.conf` raises the storage, location and network modules to
`INF`, and the survey module is at `INF` by default (`CONFIG_APP_SURVEY_LOG_LEVEL=3` via
`..._LOG_LEVEL_DEFAULT`, not an explicit `..._INF` — checked in `build-att-survey/app/zephyr/.config`,
per the rule below). The console carries their output — the hardware integration tests depend on
it. What survives is the weaker and still useful form: **absence of logging says nothing about
whether the device is healthy**, because a module can be silent at the configured level while
wedged. Diagnose from the shell:

| Command | Answers |
|---|---|
| `survey stats` | Did an observation actually land? How stale is it? |
| `kernel thread list` | Is any module thread blocked rather than pending? |
| `kernel uptime` | Did the device silently reset? |
| `at AT+CEREG?` | Is the modem registered? |
| `at AT+CFUN?` | Is the modem even switched on? |

`survey selftest` remains the fastest way to separate a radio problem from a firmware one.

### A command that reports through the log needs a settle delay before you close the port

`att_storage stats` looks broken from a script. It prints `Storage statistics request initiated.`
and nothing else, because that is all it does synchronously: the command publishes `STORAGE_STATS`
on `storage_chan` and returns. The record counts are `LOG_INF` calls in `handle_storage_stats()`,
which runs later on the storage thread and lands *after* the next prompt:

```
uart:~$ att_storage stats
Storage statistics request initiated.
uart:~$ [00:01:16.072,448] <inf> storage: SURVEY: 325 records
```

A capture that stops reading when the prompt comes back sees the first line and misses the answer.
Drain the port for a second or two after any command whose real output is logged rather than
`shell_print`ed. This cost an afternoon of chasing a log-level problem that was not there — the
level did have to go up (`CONFIG_APP_STORAGE_LOG_LEVEL_INF=y`; at `WRN` the block is compiled in but
filtered out), but that was only half of it.

### Do not drive the shell on a fixed timer

`fs write` was fired at one command per 60 ms to create directory entries in bulk. Of 600 commands,
about 181 landed and one pair overlapped into a mangled filename (`padfs`). The shell has no flow
control on input, so anything faster than the device can retire is silently lost, and the damage is
invisible unless you count what actually got created.

The rate is not constant either, which is what makes a fixed delay unfixable: LittleFS compacts a
directory's metadata pair on **every** append, so appending is O(entries) and the write rate falls
as the directory grows. Wait for the `uart:~$` prompt after each command instead. Allow a generous
per-command timeout — a write that triggers a metadata-pair split takes far longer than the median.

### Bulk `survey store` is rate-limited by the observation cache, not by storage

Repeating `survey store` in a loop stores far fewer records than commands sent, and the misses are
not storage failures: the cached observation ages out after about a minute and the command then
prints `Nothing cached. Run "survey scan", "survey gnss" or "survey selftest" first.` Batches of 50
degraded to 14 landing per batch as the run went on. Re-issue `survey selftest` every few stores to
refresh the cache, and count what landed with `att_storage stats` rather than trusting the number of
commands sent.

### `survey fill` has to be driven in bounded bursts, and no fixed interval is safe

The bench fill command publishes onto a zbus channel whose subscriber is the storage module, so
every record in flight holds a `net_buf` — drawn from a 64-entry descriptor pool but backed by the
shared 12,288-byte system heap, which is what actually runs out, at roughly a dozen 816-byte records
in flight — until the storage thread retires it. Exhaust the heap and zbus **asserts** rather than
returning an error — `ASSERTION FAIL @
zephyr/subsys/zbus/zbus.c:252`, then a usage fault and a reboot. The full analysis, including the
three configurations tried against it and why each is worse, is in `app/overlay-survey.conf` and
`docs/modules/storage.md`.

What matters for driving the bench: **pacing does not fix it.** Unpaced, and 10, 20, 30, 40 and
250 ms pacing all panic; they differ only in how long they survive first. The reason is that the
number to outrun is not constant — LittleFS compacts the record directory on append, so the
per-record cost grows with the entry count. 250 ms is fine on an empty partition and panics in 8 s
at 446 entries.

So grow the directory from the host, adaptively:

1. `survey fill 10 0` — a burst that stays under the ~dozen the heap can back, so no allocation
   fails. Note the bound is the heap, not the 64-entry descriptor pool; sizing a burst off the
   descriptor count gives an answer that panics.
2. Read the per-record cost out of the device's own log (below).
3. `survey fill 90 <3x that cost>` to grow, then repeat.

A run that ends in a panic has still stored everything the command acknowledged, so it can simply be
restarted; the sequence numbering restarts, which does not matter for synthetic records.

### Measure store latency from the storage log, not by polling `att_storage stats`

Polling `att_storage stats` to see when records land does not work: `stats` is serviced by the
storage thread, so a poll loop competes with the exact thread it is trying to measure — and the run
that tried it produced no rows in fifteen minutes while a single manual store completed fine.

Build with `CONFIG_APP_STORAGE_LOG_LEVEL_DBG=y` instead. `littlefs_backend.c` then emits one
timestamped line per record:

```
[00:00:28.790,000] <dbg> lfs_backend: Storing data in file /att_storage/SURVEY_89.bin at offset ...
```

The per-record cost is a difference of two device timestamps, with no host involvement at all. First
measurement, at 447 records / 89 files: **min 140 ms, median 172 ms, max 203 ms**, with the larger
values falling on the record that opens a new file. `scratchpad/burst.py` measures one burst;
`scratchpad/curve5.py` walks the directory up in completion-driven bursts and prints the curve — it
is the one that works, and it is what produced the FW-6 numbers in `REQUIREMENTS.md`.

Three things about reading that curve, each of which cost a wrong conclusion first:

**Bin the raw log, do not trust the summary rows.** The driver prints a median every 200 records,
and at that resolution the curve looks like a clean straight line — a regression over twelve rows
fit to within 2 % and predicted 1.5 s at the record cap. Re-binning the same raw log at 100 records
showed the median is *not* monotonic: it climbs for thousands of records, then drops back to the
floor. Fit a line to the rows and the drop hides inside the residuals.

**A pattern seen once is not a period.** That drop happened at ~5,000 records, which made a
sawtooth look obvious and would have made the latency bounded and the whole question moot. Running
7,000 records further showed it does *not* repeat at 10,000. One reset is an event, not a period;
the conservative model — keep climbing — is the one to plan against until a second reset is seen.

**Separate cost that grows from cost that does not.** The median (directory lookup, O(entries)) and
the multi-second compaction stalls (~3 % of stores) move independently, and averaging them together
hides both. Bin the medians excluding outliers, then count and average the outliers separately.

Anything measured in the first ~15 minutes of uptime is contaminated. A window at 600–1,000 records
sat 5× above the surrounding trend and looked like a filesystem property; it was the modem attaching
and the cloud module retrying CoAP, and it never recurred over the next 2.5 hours. Let the device
settle before starting a run, or discard the opening window.

### Changing the LittleFS cache size takes a devicetree change *and* a Kconfig change

The `cache-size` property on the `zephyr,fstab,littlefs` node sets the cache each open file
allocates. It does **not** size the heap those allocations come from — that is derived from
`CONFIG_FS_LITTLEFS_CACHE_SIZE` × `CONFIG_FS_LITTLEFS_NUM_FILES` (plus
`CONFIG_FS_LITTLEFS_HEAP_PER_ALLOC_OVERHEAD_SIZE` per file) whenever
`CONFIG_FS_LITTLEFS_FC_HEAP_SIZE` is left at its default of 0.

Raise only the devicetree property and the build succeeds, flashes, and boot-loops. The backend
opens the first header file, then fails the second with `-ENOMEM`:

```
init_header_files: Opened header file /att_storage/BATTERY.header (read_offset=0, write_offset=0)
<err> fs: file open error (-12)
<err> lfs_backend: Failed to open header file /att_storage/ENVIRONMENTAL.header: -12
ASSERTION FAIL @ .../storage/backends/littlefs_backend.c:499
```

Nothing in that output mentions caches, so it reads as filesystem corruption. Confirm the two
numbers agree instead — the boot banner prints the devicetree side:

```
<inf> littlefs: partition sizes: rd 16 ; pr 16 ; ca 256 ; la 32
$ grep FS_LITTLEFS_CACHE_SIZE build-*/app/zephyr/.config
```

`CONFIG_FS_LITTLEFS_NUM_FILES` is 4 here, and the backend needs exactly 4 — three header files plus
the data file it opens per record — so there is no slack to absorb the mismatch.

### Wiping the LittleFS partition

Use the shell command, not the flash driver:

```
att_storage clear
```

`flash erase GD25LE255E@0 0x4d2000 0x1800000` reports success and does not wipe anything — the files
are still there after a cold reboot. `att_storage clear` (`storage_shell.c`) runs
`lfs_storage_clear()`, which closes the header files, unlinks every entry in the mount, and re-opens
them. Unlinking during an `fs_readdir()` walk is safe despite appearing not to be: LittleFS fixes up
open directory cursors on delete (`lfs.c`, `d->id -= 1` in the `LFS_TYPE_DELETE` fixup inside
`lfs_dir_commit`), so the single pass does not skip entries. It is slow on a full partition — it is
one `fs_unlink()` per file — so allow minutes, not seconds: **229 s to remove 2,400 files**, ending
at `bfree 6140` of 6144 and 0 records for every type. Give the driver a timeout in the hundreds of
seconds or it will look like a hang.

Two related traps. `CONFIG_RESET_ON_FATAL_ERROR=n` does not keep the device up here — it still
reboots, and the log shows `Reset Cause: Software Reset`. And a device that reboots before the shell
comes up cannot be diagnosed with a prompt-synced driver at all: the driver hangs waiting for a
prompt that never arrives. Kill it and read raw serial for ten seconds instead. That is how the
`NET_BUF_POOL_ISOLATION` boot loop was identified.

### A near-full partition makes the export transport hang, not just fail

This is a second, distinct cost from the clear-is-slow trap above: at real trip fill levels (~5,000
`SURVEY_*.bin` files at 60 s cadence and the FW-6 5-records-per-file layout — a matter of days, not
an edge case) `fs_mgmt`/`survey_export.py`/the web exporter time out on every single-file `download`,
not just on the bulk clear. Confirmed it is not transport-specific or code-specific: the console's
own `fs ls /att_storage` took over 15 seconds and did not finish printing, and a clean `kernel reboot
cold` reproduced it identically afterward. Root cause is that LittleFS resolves a path with a linear
scan of its flat directory, so per-file open/lookup cost grows with file count — at the
~2,700-file/~55%-full state this was still fast enough to stay under `mcumgr`'s command timeout, but
at ~5,000 files it is not.

Fixed by `CONFIG_APP_STORAGE_LITTLEFS_TARGET_FILE_SIZE` (`app/src/modules/storage/Kconfig.storage`,
`littlefs_backend.c`'s `get_entries_per_file()`): each data file now holds a whole number of flash
blocks instead of exactly one, so the same 25,000-record capacity is spread over far fewer files.
The survey overlay sets it to 64 KiB (16 blocks), cutting the file count at full capacity from ~5,000
to ~313 — comfortably under where the hang was observed. This only changes how records are grouped
into files; `verify_partition_size()` counts blocks, not files, so total flash usage and capacity are
unaffected. Three places duplicate the resulting entries-per-file arithmetic and must be kept in sync
if either `CONFIG_APP_STORAGE_LITTLEFS_TARGET_FILE_SIZE` or the block size (4096 bytes) ever changes:
`littlefs_backend.c` (the source of truth), `scripts/survey_export.py`'s `ENTRIES_PER_FILE`, and
`web/export.html`'s `ENTRIES_PER_FILE` — fs_mgmt has no "describe your layout" command, so the two
export clients have no way to ask the device instead of hardcoding it.

The deeper issue this does *not* fix: records are stored in fixed-size slots sized for the worst case
(one record with 5 cell towers and 10 APs), so a device that mostly sees one cell tower wastes most of
each slot to padding. That is a separate, larger redesign (variable-length records) and is intentionally
out of scope here — this fix only addresses file count.

If diagnosing a timeout that looks like the FW-8/CP8 session-header work or the export transport
itself, check file count and `fs ls` latency first: an A/B test that shows the timeout on both old and
new firmware builds against a full partition, but not against a cleared one, is a fast way to rule the
code out.

## Unit tests

native_sim is Linux-only — Zephyr's POSIX arch refuses to configure on macOS with "The POSIX
architecture only works on Linux". `scripts/run_unit_tests.sh` therefore runs Twister inside a small
Linux container. It needs Docker running, and builds its image on first use.

```sh
scripts/run_unit_tests.sh                        # all module tests
scripts/run_unit_tests.sh tests/module/survey    # one suite
scripts/run_unit_tests.sh --rebuild              # force image rebuild
```

On arm64 the platform must be `native_sim/native/64`; plain `native_sim` sets `CONFIG_64BIT=n` and
aborts with "this Aarch64 machine has a 64-bit userspace". The script defaults to the 64-bit
variant.

Do not reach for `twister -p native_sim` directly to run one suite in a hurry. It does not fail — it
reports **0 configurations selected** and exits 0, because the suites declare the 64-bit platform
and `native_sim` is statically filtered out. A run that tested nothing and a run in which everything
passed look identical at a glance. Always go through `scripts/run_unit_tests.sh`, and read the
configuration count in the summary, not just the exit status.

### Seeing per-test results

Twister reports a Unity suite as a single test case, so a passing run does **not** prove an
individual test was registered and executed. To see per-test output, run the container directly with
the report directory mounted out:

```sh
OUT=/tmp/twister-out && mkdir -p "$OUT"
docker run --rm \
    -v /Users/tyler/junk/ncs-3.4.0:/work/ncs \
    -v /Users/tyler/junk/Asset-Tracker-Template:/work/project \
    -v "$OUT":/work/out \
    -e ZEPHYR_BASE=/work/ncs/zephyr -e ZEPHYR_TOOLCHAIN_VARIANT=host \
    att-unit:latest \
    bash -lc "cd /work/ncs && python3 zephyr/scripts/twister \
        -T /work/project/tests/module/survey \
        --platform native_sim/native/64 -O /work/out/run"

grep -hE ':(PASS|FAIL)|Tests .* Failures' $(find "$OUT" -name handler.log)
```

Point `-O` at a **subdirectory** of the mount (`/work/out/run`, not `/work/out`). Twister deletes
its output directory on startup, and removing an active bind mount fails with
`OSError: [Errno 16] Device or resource busy`.

Tests are registered automatically. `test_runner_generate()` in the suite's `CMakeLists.txt`
generates the Unity runner by scanning for `void test_*(void)` functions, so a new test needs no
manual registration — but it is silently absent if misnamed, which is why the per-test output above
is worth checking after adding tests.

### Point `-O` outside `/tmp` on this host

`-O /tmp/twister-out/run` fails before any build starts:

```
PermissionError: [Errno 13] Permission denied: '/work/out/run'
```

`chmod 777` on the host directory does not help — macOS `/tmp` is a symlink into `/private/tmp`, and
the container cannot create inside that bind mount whatever its mode. Use a directory under
`/Users/tyler/junk` instead; `/Users/tyler/junk/twister-out` works. The error names the *container*
path, which is what makes it look like a mount-mode problem rather than a host-path one.

### Kernel features a unit suite uses have to be enabled in its own `prj.conf`

A unit suite does not inherit the application's Kconfig, so anything the unit under test calls into
has to be turned on again. The failure is a link error naming the internal symbol rather than the
feature:

```
undefined reference to `z_impl_k_event_clear'      -> CONFIG_EVENTS=y
```

`tests/module/survey_capture/prj.conf` also sets `CONFIG_NATIVE_SIM_SLOWDOWN_TO_REAL_TIME=y`, which
is the opposite of what the other suites want: its assertions compare measured step durations
against the fake's sleeps, and at simulated speed those numbers mean nothing. Set it only where a
suite measures time.

### A suite that boots and then hangs is missing `main()`

Failure signature: the suite builds, twister prints the Zephyr boot banner, then nothing, and the run
eventually fails with `FAILED: Unknown Error` and no test output at all.

`test_runner_generate()` emits `unity_main()`, **not** `main()`. If the suite does not define one,
Zephyr's weak `main` runs, returns immediately, and the main thread exits — leaving native_sim to
tick its timer against real time forever. Every suite needs:

```c
/* Provided by the test runner generator. */
extern int unity_main(void);

int main(void)
{
	(void)unity_main();

	return 0;
}
```

This looks exactly like a CMake or Kconfig problem and is not one. Before suspecting the build, run
`nm` on the ELF or diff the suite against a passing one — `tests/module/survey` is the reference. To
confirm directly, `apt-get install -y gdb` inside the container and attach to the `zephyr.exe`
twister left running: a backtrace showing only the idle thread in `hwtimer_tick_timer_reached` →
`nanosleep` means the main thread is gone, not blocked.

### Keeping twister artifacts

`scripts/run_unit_tests.sh` runs the container with `--rm`, so `handler.log` and the ELFs vanish with
it. When you need them afterwards — extracting host fixtures, running gdb, inspecting symbols — run
the container directly with an output mount as shown above.

### A test that must exist in every build variant

Unity's runner generator scans the **source text** for `void test_*(void)` and emits a call for each
one it finds. `#if`-ing a test out of the compilation does not remove it from the runner, so the
build fails at link with `undefined reference to test_...`.

When a suite is built more than once with different options — `tests/module/survey_store` builds
once per storage full-behaviour — define every test unconditionally and skip at runtime instead:

```c
if (!IS_ENABLED(CONFIG_APP_STORAGE_FULL_STOP)) {
	TEST_IGNORE_MESSAGE("FULL_OVERWRITE build; asserted in .stop");
}
```

Unity then prints `IGNORE: <reason>` for it, which is checkable: read the per-test output of *all*
variants and confirm every test PASSes somewhere. A test ignored in every variant looks like a
passing suite.

The variants themselves come from `extra_args` in `testcase.yaml`, which twister passes to CMake as
`-D`. Unit tests do not source the application's Kconfig, so an application `choice` symbol has to
arrive as a `target_compile_definitions` entry selected by that CMake variable.

`TEST_IGNORE_MESSAGE` is the fallback, not the first choice. Where the behaviour differs by a value
rather than by existing at all, branch on the expectation and assert in both variants — the BSSID
filter's suite does this:

```c
#define FILTER_ON IS_ENABLED(CONFIG_APP_SURVEY_DROP_LOCAL_MAC)
...
TEST_ASSERT_EQUAL_UINT16(FILTER_ON ? 1 : 3, obs.scan.wifi_cnt);
```

Nothing is ignored, so nothing can be ignored in *every* variant, and the off setting is asserted
rather than merely compiled.

### A new filter can silently eat an existing test fixture

`survey_obs`'s fixture BSSID was `AA:BB:CC:DD:EE:FF`, and `0xAA` has the locally-administered bit
set. Adding the BSSID filter would have dropped the fixture AP inside a dozen tests that are about
something else entirely, and they would have failed for a reason nowhere near the change.

Before adding any filter, grep the suites for literals of whatever it filters on and check them
against the new predicate by hand. Fixture values get chosen for being readable, not for being
representative — `0xAA` was picked because it is a recognisable byte, which is exactly why it is a
randomised MAC. The fix was `0xA8`, plus the two assertions that spelled the address out.

### An assertion that holds in only one of a suite's two configurations

`test_a_full_array_with_the_last_entry_randomised` asserted every surviving AP had a global first
octet. True with `APP_SURVEY_DROP_LOCAL_MAC=y`; false with `=n`, where the randomised entry survives
by design:

```
src/survey_obs_test.c:828:test_a_full_array_with_the_last_entry_randomised:FAIL: Expected 168 Was 2
```

The `FILTER_ON` ternary was already threaded through the counts in that test, so the miss was in the
one assertion that read like an invariant rather than like a filter outcome. Assert against the
fixture — `macs[i]`, not `GLOBAL_MAC_0` — and the line is correct in both configurations without a
conditional at all. When a suite has two Kconfig configurations, read every assertion twice, once
per configuration; `-T tests/module/survey` alone runs both and takes about four minutes.

### Flash then run, and the first hardware test races the boot banner

`nrfutil device program` leaves the device resetting, so a suite started in the same command line
runs its first test against a console still printing:

```
  shell_responds   FAIL  (1.5s)
      no survey command group -- wrong build or wrong port
      uart:~$ [00:00:00.524,932] <inf> littlefs: LittleFS version 2.11, disk version 2.1
      uart:~$ *** Booting Asset Tracker Template v1.5.4-dev-582d90c51546 ***
```

The failure message accuses the build and the port, which are both fine. The other 22 tests pass,
because by then the line is quiet.

`console.drain()` does not prevent it: 0.15 s quiet threshold, 1.5 s ceiling, and boot output has
gaps longer than the first and runs past the second, so it returns mid-banner. Those constants are
right for draining between commands, so `Device.wake()` now does its own settle — quiet for 1 s,
ceiling 20 s — rather than changing the shared helper. Reproduce by flashing and launching the
suite in one command; running the suite against an already-booted device will not show it.

### Some overruns are untestable, and the answer is to delete the arithmetic

Mutating the tail-zeroing `memset` in `drop_local_mac_aps()` to run one entry long left both
configurations of `tests/module/survey` green. The boundary test written specifically to catch it
did not, and its comment claimed otherwise.

The reason is layout, and it is worth reading the actual offsets rather than reasoning about field
order — `arm-zephyr-eabi-gdb -batch -ex "ptype /o struct survey_observation" build-*/app/zephyr/zephyr.elf`
prints them with the holes and padding marked, which is the fastest way to settle a question like
this. For this build:

```
/*    858      |     100 */        struct location_wifi_ap_info wifi_aps[10];
/* XXX  2-byte padding   */                     <- end of struct location_cloud_request_data
/*    960      |       2 */    uint16_t scan_local_mac_dropped;
/* XXX  6-byte padding   */                     <- total size 968
```

A one-entry overrun is 10 bytes and lands in exactly those 2 + 2 + 6. It never leaves the struct, so
ASAN cannot redzone it, and the only live bytes it touches are the counter `survey_obs_update()`
assigns on the very next line — overwritten before anything can observe it.

Note the margin: **zero bytes**. This is a coincidence of padding, not a safety property. Append one
member after `scan_local_mac_dropped`, or change `CONFIG_APP_LOCATION_WIFI_APS_MAX`, and the same
slip becomes a genuine write past the end of a `.bss` object — which ASAN *would* catch. The
conclusion below does not depend on which of those two worlds you are in, but "it's harmless" does,
so do not carry that half forward.

Do not keep writing tests for a bug the public surface cannot expose. Make the bound derive from
the object instead of from a counter:

```c
memset(&scan->wifi_aps[kept], 0, (ARRAY_SIZE(scan->wifi_aps) - kept) * sizeof(scan->wifi_aps[0]));
```

`kept` is bounded by `count`, which is already clamped to `ARRAY_SIZE`, so this cannot address past
the end whatever the counters do. Zeroing the whole remainder rather than just the compaction gap
is also better hygiene — it clears entries left by an earlier, longer scan.

The general rule: when a mutation survives, find out *why* before adding another assertion. If the
answer is "the corruption is unobservable," the test is not the thing that needs fixing.

### Regenerating host fixtures needs a twister out-dir the host can see

`scripts/run_unit_tests.sh` passes `-O /tmp/twister-out`, which is inside the container, so
`handler.log` never reaches the host and `scripts/survey_fixture.py` has nothing to read. Run the
`docker run` from that script by hand with an extra mount and `-O` pointed inside it:

```
-v /Users/tyler/junk/twister-out:/work/out ... -O /work/out/run
```

`-O /work/out` — the mount point itself — fails on a re-run, because twister *moves* an existing
out-dir aside before starting and a bind mount cannot be renamed:

```
OSError: [Errno 16] Device or resource busy: '/work/out'
```

Point `-O` at a child of the mount, never at the mount root. This is a different failure from the
`/tmp` permission one above, and it only appears on the second run.

### Break the code to see whether the test noticed

A test that stores one record into a partition `setUp()` just cleared and then asserts the unused
tail of the slot is zero passes whether or not the code zeroes anything — the erased flash was
already zero. The suite looked green and covered nothing.

The cheap check is a mutation: comment out the line the test claims to cover, re-run, confirm it
fails, put it back.

```sh
# with the memset in survey_store.c commented out
src/survey_store_test.c:318:test_the_unused_tail_of_a_slot_is_zeroed:FAIL: Expected 0 Was 159
```

Worth doing for any test whose expected value is zero, empty, or absent, because that is also what a
freshly initialised system looks like. The fix for this one was to dirty every slot with a maximal
record and drain before storing the small record under test.

The same trap catches **single-assignment propagation paths** — a field copied from one struct to
the next on its way to flash. `scan_local_mac_dropped` reached the stored record through exactly two
lines, `survey_capture.c:432` and `survey_record.c:264`, and deleting either left all 17
configurations green: the capture fake never set the source field and the `from_obs` tests never set
it either, so both sides were comparing 0 against 0. In the export the loss is invisible too —
absent means "nothing was dropped", which is precisely what a missing assignment produces.

So when a value crosses a struct boundary, set it to something **non-zero** in the fixture on the
far side of the boundary, and assert it on the near side. Then mutate to confirm:

```sh
src/survey_capture_test.c:620:...:FAIL: Expected 2 Was 0. the dropped-AP count did not reach the record
src/survey_record_test.c:543:...:FAIL: survey_record_from_obs() did not carry the dropped-AP count
```

Count the boundaries, not the functions: a field that looks covered because the encoder round-trips
it may still have no coverage on the hop *into* the encoder's input struct.

### A fake built on a message subscriber cannot model a module that discards

The capture suite's fake location module was a `ZBUS_MSG_SUBSCRIBER`. The real location module
*discards* a trigger that arrives while a search is running; a message subscriber **queues** it. So
the overlap the tests existed to detect could not occur: the second trigger sat in the queue until
the fake got round to it, `overlap_detected` was unreachable, and the start/finish ordering
assertion was tautological. Deleting the orchestrator's `wait_for_idle()` left all of it green.

Serving synchronously made it worse. When `serve()` publishes the result and the done event before
returning, no other thread can observe the fake as busy — every test agrees the requests did not
overlap because nothing could have. The fix was to serve **asynchronously**: a `k_work_delayable`
that sets a `busy` flag, publishes `LOCATION_SEARCH_STARTED`, and returns, so the subscriber thread
can dequeue an overlapping trigger and discard it exactly as the real module would. The result and
`LOCATION_SEARCH_DONE` arrive from later work items, with a gap between them — which is also where
the real library's "result published before done" hazard lives.

Then break the code (above) and count: removing `wait_for_idle()` now fails five tests.

The general rule: a fake has to reproduce the *disposition* of the real module, not just its
messages. Queueing where the real thing drops turns every sequencing assertion into a tautology.

### native_sim cannot test a data race

Do not write a two-thread test to prove a mutex is needed. `native_sim` advances its simulated clock
only when no thread is ready to run, and that makes both shapes of the test useless:

- A contender that spins on `k_yield()` keeps the CPU permanently busy, so the main thread's
  `k_sleep()` never expires. The suite hangs until twister kills it — `failed (rc=-9)`, with the last
  line of `handler.log` being whichever test ran before it.
- A contender that sleeps is never runnable during the window the main thread spends inside the
  function under test, at equal priority and with timeslicing off. It never interleaves.

The second version was written, passed, and then **still passed with the mutex deliberately removed**
— a green test asserting nothing, which is worse than no test. It was deleted rather than kept.
Serialisation gets verified by inspection and by documenting the lock ordering (see
`survey_store.h`), and on target where preemption is real.

The general rule is the one above: if a mutation of the code under test does not turn the test red,
the test does not cover it. That applies to concurrency tests too, and concurrency is where a test is
most likely to look convincing while covering nothing.

### A `SYS_INIT` timer under `--whole-archive` really runs in the test binary

`tests/module/survey_capture` links `survey_capture.c` with `target_link_options(app PRIVATE
--whole-archive)`, so every `SYS_INIT`-registered function in that file executes during the test
binary's own boot, not just in a real device build — there is no "it's just linked in, not called"
escape hatch. Adding FW-7's cadence timer (a `SYS_INIT` that schedules a `k_work_delayable` calling
`survey_capture_request()` on its own) meant that hook really started firing during the suite,
racing its own deterministic request/response assertions — and racing for real: the test's
`prj.conf` sets `CONFIG_NATIVE_SIM_SLOWDOWN_TO_REAL_TIME=y` (needed elsewhere to assert on measured
durations, see CP6), so a 20 s timer genuinely takes 20 real seconds, well inside the suite's own
~30 s worst-case cycle.

Fixed with a hidden Kconfig symbol (`bool`, no prompt, `default y`) that the real `Kconfig.survey`
sets but the test's `CMakeLists.txt` — which defines the module's Kconfig symbols via
`target_compile_definitions` rather than sourcing `Kconfig.survey` at all — simply never defines.
Wrapping the `SYS_INIT` registration (and everything only it needs, like the `k_work_delayable`
itself) in `#if defined(...)` compiles the autostart out of the test binary entirely, rather than
trying to make the timer itself race-proof. The general lesson: before adding any `SYS_INIT` to a
module a `--whole-archive` unit test links, check whether that test's Kconfig approach would leave
it silently enabled, and give it an explicit off-switch if so.

### Capacity arithmetic on the LittleFS backend

Record capacity is bounded by **blocks, not bytes**. The backend groups
`CONFIG_APP_STORAGE_LITTLEFS_TARGET_FILE_SIZE` worth of blocks into each file (default 0 = one block
per file; the survey build sets 65536 = 16 blocks/file, see "A near-full partition..." above), so
`get_file_index()` is `index / entries_per_file`, and *N* records mean *N*/`entries_per_file` files,
each consuming a whole number of erase blocks plus its directory metadata. Total block count for a
type is independent of this grouping — it is still `data_size * RECORDS_PER_TYPE / block_size` — only
the file count (and therefore directory-metadata overhead) changes.

`verify_partition_size()` does **not** check this — it models densely packed records plus a flat 3
blocks, so it will pass a configuration that later runs the filesystem out of blocks mid-run. Size
`CONFIG_APP_STORAGE_MAX_RECORDS_PER_TYPE` from the file count and leave real margin. See the FW-6
section of `REQUIREMENTS.md` for the worked example.

## Host tests

The Python under `scripts/` has its own suite, which does not involve Docker, Zephyr, or a toolchain:

```sh
python3 -m unittest discover -s tests/host
```

The decoder is stdlib-only by design so this stays true. See `tests/host/README.md` for how the CBOR
fixtures are regenerated from the firmware encoder's actual output.

`scripts/survey_smp.py` (the fs_mgmt client `scripts/survey_export.py` and the CP9 export path build
on) needs pyserial, but only to open a real port. `import serial` lives inside `SmpSerial.__init__`,
not at module scope, so the framing, the fs_mgmt CBOR encode/decode, and `FsMgmt` driven by a fake
transport all stay importable — and unit-testable via `tests/host/test_survey_smp.py` and
`tests/host/test_survey_export.py` — on a bare `python3` with no pyserial installed, same as the
decoder above. Bare `python3 -m pip install pyserial` on this machine hits a PEP 668
externally-managed-environment error; pulling `import serial` down into the one place that actually
opens a port avoided needing to fight that just to run the host test suite.

That deferred import only fixes the *test suite* — it does not make `scripts/survey_export.py`
runnable against a real port, since that script's whole job is to open one. For that,
`survey_export.py` carries PEP 723 inline script metadata (a `# /// script ... dependencies =
["pyserial"] ... ///` block right after the shebang), so `uv run scripts/survey_export.py --port
... --baud ... out.bin` provisions pyserial into an isolated environment on first run — no
system-Python fight, no toolchain-manager detour. Confirmed against a real Thingy:91 X on
`/dev/cu.usbmodem105`.

## Hardware integration tests

`tests/hardware/survey_hwtest.py` drives the console and asserts on the replies, so the on-target
step below is a command rather than a bench session retyped from memory:

```sh
nrfutil toolchain-manager launch --ncs-version v3.4.0 -- \
    python3 tests/hardware/survey_hwtest.py --radio
```

The `nrfutil toolchain-manager launch` prefix is not optional: pyserial is installed in the
toolchain environment and nowhere else, so a bare `python3 tests/hardware/survey_hwtest.py` on this
machine stops at "pyserial is required" before it opens the port.

**A run with no flags is not a verification.** It selects 9 of the 22 tests — every case that
touches the radio or flash is tagged out — and prints `9 passed, 0 skipped, 0 failed` with exit 0.
That output is indistinguishable at a glance from a full green run, and it was once taken for one
after a firmware change to the capture path, none of which those 9 tests exercise. The command that
verifies a firmware change is `--radio --destructive --yes`; read the test count on the first line
before believing the last one.

Without `--radio` it runs only what needs no network or sky view. That is not the same as harmless:
it injects synthetic data into the observation cache and then clears it, zeroing the counters, so it
will confuse a bench session in progress. What it leaves alone is flash and the reset line — the
tests that append records or cold-boot the device are behind `--destructive`, on a separate axis from
`--radio`. `--loopback` runs the harness's own self-tests against the fake shell in
`scripts/survey_console.py`, with no hardware attached — worth doing after editing the harness,
because a harness that silently fails to read the port reports PASS for everything. See
`tests/hardware/README.md`.

### Three device behaviours the harness had to be built around

The first two are documented above; what is new there is how long the waits actually are. The third
is new.

**`att_storage stats` can be silent far longer than "a second or two"** (see *A command that reports
through the log needs a settle delay*). The storage thread counts every record at mount, and until
it finishes it answers nothing: measured **silent for about 45 s after a cold boot holding 5,004
records**, then answering in 1.1 s on every subsequent ask. It is equally unavailable for the
duration of a store. One silent window is not evidence of a wedged device — ask again.

This does not contradict *Measure store latency from the storage log, not by polling* further down.
That warning is about **timing** a store, where the poll competes with the thread being timed and
distorts the number. Asking a few times, seconds apart, for a **count** does not perturb the count.
Do not reuse the harness's `_survey_count()` to measure latency.

**A dropped trigger costs a whole test window** (see *The first scan after boot is usually
dropped*). With `CONFIG_LOCATION_REQUEST_DEFAULT_GNSS_TIMEOUT=600000` the startup sample can hold
the Location library for ten minutes, and the only evidence is one log line:

```
<wrn> location_module: Location trigger received while a search is active, ignoring
```

Observed as a `survey scan` that produced nothing for 60 s. The harness's `trigger()` waits the
running search out and retries, and its reboot tests are registered last so they cannot starve the
radio tests. **Removed at CP6:** `CONFIG_APP_SURVEY_CAPTURE_OWNS_SEARCH` drops the default
`LOCATION_SEARCH_TRIGGER`, and the radio tests stopped needing retries — `scan_returns_observations`
now completes in 8.1 s. Keep the retry logic anyway; it costs nothing when nothing is competing, and
it is the only thing standing between a busy modem and a false failure.

**A trigger cannot be correlated from the result path at all.** `GNSS fix cached` and `Scan cached`
come from a zbus listener on `location_chan` that fires for *any* result, including the startup
sample and the periodic timer, so a test that waits for one passes with the trigger deleted from the
firmware. The `gnss #N, scan #M` counters in `survey show` are the same signal wearing a different
hat — `survey_obs_update()` increments them inside that same callback — so switching to them buys
nothing. Neither does the absence of the `ignoring` warning, which is also absent when no trigger
arrived.

Closing it takes a token on the *trigger* path. `location.c` counts the triggers it accepts, the ones
it discards, and the ones `OWNS_SEARCH` suppresses; `survey show` prints
`Triggers: accepted N, dropped M, suppressed K`. Require both `accepted` and the result counter to
advance.

`suppressed` is separate from `dropped` because the harness reads a rise in `dropped` as "our trigger
lost the race" and retries. Counting the application's own suppressed sampling there would make the
harness abandon triggers that were still pending. Two dispositions, two counters — and `suppressed`
is also the only outside evidence that the suppression branch exists at all, which is what
`owns_search_suppresses_the_application_sample` asserts. The general lesson: when a test has to attribute an effect to its own
stimulus, and the effect is something the system also produces on its own, the observability has to
be added at the stimulus. No amount of care on the observation side substitutes for it.

### Match a single space in shell output and the test will fail on the longest label

`capture_timing_reports_real_durations` failed on hardware with "no gnss_after in survey timing"
while the device was printing it. The report is column-aligned, so the padding after each label is
whatever the *longest* label needs:

```
gnss_before 1800 ms
scan        4846 ms
gnss_after  1154 ms
```

`gnss_after` gets two spaces. Use `\s+` between a label and its value in every harness regex — the
gap is layout, not content, and it changes whenever a longer label is added to the same block.

### A ninth task-watchdog client needs a ninth channel

`prj.conf` sets `CONFIG_TASK_WDT_CHANNELS=8`, and upstream uses all eight — main, power, fota,
location, network, environmental, storage, cloud. Registering the capture thread made a ninth
caller, `task_wdt_add()` returned `-ENOMEM`, and `SEND_FATAL_ERROR()` rebooted the device on every
boot.

What that looked like on the bench was **twelve unrelated hardware tests failing at once**, none of
them about watchdogs, with the only evidence a single `<err> survey_capture: Failed to add capture
thread to watchdog: -12` line inside one test's captured boot log. Nothing asserted on it.

`overlay-survey.conf` now sets `CONFIG_TASK_WDT_CHANNELS=9`, and the module logs the channel it got
on success so `capture_thread_is_watchdogged` can require it. Two general points:

- A test suite that goes broadly red is usually one systemic cause, not many. Read the boot log
  first, before triaging individual tests.
- When adding a resource-limited registration (watchdog channel, zbus observer, net_buf pool user),
  check the configured count against the existing users. The failure is at runtime and the resource
  is fully consumed by upstream in more places than this one.
- Better still, make it a build error. `survey_capture.c` now carries
  `BUILD_ASSERT(CONFIG_TASK_WDT_CHANNELS >= 9, ...)`. A misconfiguration that can only present as a
  boot loop should be caught by the compiler, where the message can name the file to edit.

### Swallowing a zbus message can strand another module's state machine

`CONFIG_APP_SURVEY_CAPTURE_OWNS_SEARCH` suppresses main.c's periodic `LOCATION_SEARCH_TRIGGER` so the
capture orchestrator owns the radio. The first version simply dropped the message. But
`LOCATION_SEARCH_DONE` is the **only** exit from main.c's two SAMPLING states (`main.c:1058` and
`:1164`), and entering one stops the sample timer — so main sat in SAMPLING forever, with no periodic
power or environmental samples. Silently: main feeds its task watchdog *before* `zbus_sub_wait_msg`,
not after, so a thread parked on the wait looks perfectly healthy. It unwedged only by accident, when
a later capture cycle's own DONE happened along.

The fix is to answer, not merely ignore: the suppression branch publishes a synthetic
`LOCATION_SEARCH_DONE`. That is honest — no search ran, so a search taking zero time is what
happened.

Two things to carry forward:

- **A request/response message pair is a contract.** Before dropping a request, find every state
  machine waiting on its response. `grep` for the response type, not the request you are suppressing.
- **A watchdog fed before a blocking wait proves nothing about progress.** It proves the thread
  reached the wait. Upstream's module loops are all shaped this way, so a module stuck waiting for a
  message that will never arrive is invisible to the watchdog by construction. Assert on a state
  transition instead — `suppressed_trigger_still_releases_main` requires main to enter a WAITING
  state after the SAMPLING it entered at boot, which the trigger counters cannot see.

### A message published to answer one module is delivered to all of them

The synthetic `LOCATION_SEARCH_DONE` above fixed main.c and broke two other things, because zbus has
no addressee: every observer of `location_chan` sees it.

- `survey_capture` sequences steps on that exact message. A synthetic DONE arriving mid-step ends the
  step with the radio still working, so the cycle skips its wind-down wait, publishes the next
  trigger into a busy modem — which the location module discards — and stores a record carrying one
  step's timestamps around the next step's measurements. Wrong data, no error anywhere.
- The location module observes its own channel. Its subscriber FIFO can deliver the synthetic DONE
  *after* an already-queued capture trigger, returning it to INACTIVE while the library is still
  searching. The next trigger is then accepted, `location_request()` returns `-EBUSY`, and
  `location.c:485` calls `SEND_FATAL_ERROR()` — a device reset, from a message published to be
  helpful.

Both are closed, and the shape of the fixes is the lesson:

- **Publish only when nobody else is listening for that message.** The suppression branch checks
  `survey_capture_busy()` first; a running cycle publishes a real DONE seconds later, which releases
  main just as well.
- **Consumers should qualify what they accept rather than trust the channel.** `survey_capture`'s
  listener drops any DONE that arrives before its own search has reported `LOCATION_SEARCH_STARTED` —
  the library reports started before done for every request it accepts, so an earlier done cannot be
  ours. Gate in the *listener*, not in the waiting thread: a clear at the end of a bounded handshake
  wait misses a stray message that arrives while a slower started is still pending, and inside the
  listener the ordering is the channel lock's rather than the scheduler's.
- **Test the gate by removing it.** `test_a_stray_done_before_started_does_not_end_the_step` makes
  the fake publish a stray DONE and then report started *later than the orchestrator waits for it*.
  Without the gate it fails; with the earlier, weaker placement it also fails. A test that only
  covers the fast path would have passed against both.

### "Busy" is two questions, and answering both with one predicate strands a state machine

Once `location.c` was gating its synthetic DONE on `survey_capture_busy()`, the guard read correctly
and was still wrong. A capture cycle holds `cycle_slot` from admission until *after*
`survey_store_publish_record()` returns — tens of seconds once the partition holds thousands of
records — but its last `LOCATION_SEARCH_DONE` is published well before that. So a suppressed
application trigger arriving during the store got no DONE and none was ever coming. `DONE` is the
only exit from main.c's two SAMPLING states and the sample timer is stopped on entry, so main sat
there with no periodic power or environmental samples, no log line, and a watchdog that is fed
before the zbus wait rather than after. At CP6, where cycles are started by hand, it would never
have recovered.

The two callers were asking different questions:

| Caller | Question | Predicate |
| --- | --- | --- |
| `survey store` shell command | may I start work that would interleave with the cycle? | `survey_capture_busy()` — the slot |
| `location.c`'s trigger suppression | is the *radio* spoken for? | `survey_capture_radio_busy()` — cleared after step 3 |

Two lessons worth carrying:

- **A predicate named after a resource should be scoped to that resource**, not to the operation that
  happens to hold it. `busy` covered the cycle; the guard needed the radio.
- **Raise the flag in the requester, not in the worker.** `radio_busy` is set in
  `survey_capture_request()`, before `k_sem_give(&cycle_go)`. The capture thread runs at
  `K_LOWEST_APPLICATION_THREAD_PRIO` and may not be scheduled for a while, and a flag that reads
  false in that gap reintroduces the reordering hazard it exists to prevent. This is the same reason
  `survey_capture_busy()` reads the semaphore rather than a flag the thread sets.
- **Fake the slow part in the unit test.** `survey_store_publish_record()`'s fake samples both
  predicates on the capture thread the moment the store begins. That is the only point where the two
  can be caught disagreeing through the module's public API, and it is what stops
  `survey_capture_radio_busy()` from being quietly reimplemented as an alias. Verified by deleting
  the clear: `test_the_radio_is_reported_free_before_the_store_begins` fails.

### Bound every poll loop in a Twister test

`test_busy_is_asserted_for_the_whole_cycle_including_between_steps` polled `while (steps_seen < 3)`
with no deadline. The regression that loop exists to catch is a cycle that stops short of three
steps — which would have hung, and Twister kills the binary on a hang, taking the other fourteen
named results in the file with it. A bounded loop fails one test by name. Use a deadline generous
against the fake's timing (10 s against a ~300 ms fake cycle); the bound is there to convert a hang
into a failure, not to measure anything.

### Reboot with `await_line(command=...)`, never `cmd()` then await

`cmd()` reads for its entire timeout before returning, so everything the device logs in those
seconds lands in *its* reply and is gone by the time the caller starts listening. Issuing a reboot
that way loses the first ~2 s of boot — which is where the interesting lines are. This cost two
separate debugging rounds: once for `Capture thread watchdog channel 8`, once for main.c's
`disconnected_sampling_entry`. Both times the failure message read "the device never logged it",
which is indistinguishable from a real firmware defect.

```python
# Wrong -- the reply to cmd() eats the boot log.
dev.cmd("kernel reboot cold", timeout=3.0)
boot = dev.await_line(pattern, 30.0)

# Right -- one continuous read that starts before the reset takes effect.
boot = dev.await_line(pattern, 30.0, command="kernel reboot cold")
```

`reboot()` in the harness does this. Any new test that needs boot output should use it rather than
rolling its own.

### Host Python is typed

Every function in the survey host tooling and test harnesses carries parameter and return
annotations — `scripts/survey_console.py`, `scripts/survey_decode.py`, `scripts/survey_fixture.py`,
`tests/host/`, `tests/hardware/`. Annotate as you write, not in a cleanup pass. Structured returns
use `NamedTuple` rather than positional tuples, so a caller reading `counters(dev).suppressed` cannot
silently pick the wrong field when one is added.

The upstream scripts (`inspect_state.py`, `smf_to_plantuml.py`, `nrf91_flasher.py`, and the rest) are
deliberately left alone — retyping them is divergence for no benefit.

### Ask the device how long to wait, do not hardcode it

`run_capture()` waited 420 s, which was the worst case for the step timeouts at the time it was
written. Raising `APP_SURVEY_CAPTURE_GNSS_TIMEOUT_SECONDS` silently made the harness abandon cycles
that were still inside their own budget, and the failure reads as "the device stopped responding" —
the most misleading message it could produce, because the device was working.

`survey capture` prints `Expect up to N s` from `SURVEY_CAPTURE_WORST_CASE_SECONDS`, which is derived
from the same Kconfig values the orchestrator obeys. The harness now parses that line and waits for
it plus a fixed slack for the store, so a timeout change cannot desynchronise the two. Prefer this
shape wherever the firmware already knows a bound: a constant duplicated into a test is a constant
that will be wrong.

## Before committing

1. All three builds pass (survey overlay, default, and — if the change touches anything `survey fill`
   or another default-off option compiles — the fill build), plus one build of the *off* arm of any
   Kconfig the change adds or branches on (see below).
2. Unit tests pass, and any new test appears in the per-test output.
3. Behaviour verified on target — every commit must be flashable and working, not merely compiling.
   `tests/hardware/survey_hwtest.py --radio` is the floor; add a case there for whatever the commit
   changed.
4. A senior-firmware-engineer review of the changeset in a separate context.

### `#if` around a Kconfig hides the branch nobody builds

`survey.c` had an `#if defined(CONFIG_APP_SURVEY_DROP_LOCAL_MAC)` with an `#else` that **no build
in the verification set ever compiled**: `survey.c` is not in any native_sim suite, the option
defaults `y`, and all three application builds take it. The `#else` could have referenced a deleted
variable or the wrong format specifier and every gate above would still have been green. The
native_sim suite's `keep_local_mac` configuration does not help — it builds `survey_obs.c`, not the
zbus glue.

Two fixes, and take both:

- Prefer `if (IS_ENABLED(CONFIG_...))` to `#if`. Both arms then have to type-check in every
  configuration and the dead one is optimised out, so the compiler covers the branch no test suite
  reaches. It also removes the `ARG_UNUSED` that the `#else` arm needed.
- Build the off configuration once, as an *application* image, before committing a change that adds
  one:

  ```
  west build -b thingy91x/nrf9151/ns --sysbuild -d build-att-nodroplocal ../Asset-Tracker-Template/app \
      -- -DEXTRA_CONF_FILE=overlay-survey.conf -DEXTRA_DTC_OVERLAY_FILE=overlay-survey.overlay \
         -DCONFIG_APP_SURVEY_DROP_LOCAL_MAC=n
  ```

  A `-DCONFIG_*` on a sysbuild command line is silently ignored in more places than it is honoured,
  so confirm it landed rather than trusting it — per the section below, read
  `build-att-nodroplocal/app/zephyr/.config` and expect `# CONFIG_APP_SURVEY_DROP_LOCAL_MAC is not
  set`. A build that quietly kept `=y` proves nothing and looks identical.

The general form: any Kconfig with a default-off (or default-on) sibling arm needs one build of the
non-default arm in the checklist, or the arm is unverified code.

### Check Kconfig claims against the generated `.config`, not against upstream defaults

A comment that names a Kconfig value has to be read out of `build-*/app/zephyr/.config`, because
this application overrides plenty of upstream defaults and the override is not visible from the
Zephyr source. The CP5 write-up asserted a 16-entry zbus `net_buf` pool — Zephyr's default — in four
files, while `app/prj.conf` had been setting `CONFIG_ZBUS_MSG_SUBSCRIBER_NET_BUF_POOL_SIZE=64` all
along. That is not a harmless slip: the bench procedure above sized `survey fill` bursts off the
pool figure, and the real ceiling is the 12,288-byte system heap (roughly a dozen 816-byte messages
in flight), which binds long before either descriptor count. Sizing a burst off "64" panics the
device.

The general form: when two limits could bind, measure or derive which one does rather than quoting
whichever is easier to find. Same applies to RAM figures — take them from the two link maps
(`grep` the symbol in `build-*/app/zephyr/zephyr.map`) rather than reasoning about the Kconfig
arithmetic, which is how a 1,536-byte estimate stood in for an actual 1,152 bytes, 768 of it in
`.noinit` rather than `.bss`.

### A Kconfig whose `depends on` is unmet vanishes without a word

Setting `CONFIG_MCUMGR_TRANSPORT_UART=y` in an overlay does nothing on its own: the symbol
`depends on UART_MCUMGR`, a console-driver option nothing else in this build enables. Kconfig does
not warn, does not error, and does not mention the assignment it discarded. The build succeeded, the
devicetree `chosen` override to `&uart1` applied correctly and was visible in `zephyr.dts`, the
image flashed — and the port simply never answered. Every visible artefact said the transport was
there.

`select` propagates upward and `depends on` does not, so an overlay that only turns on the leaf
option gets silence. The rule that follows is the one the section above already states, applied
before debugging rather than after: **grep the generated `.config` for the symbol you set.** Absent
means the dependency chain rejected it. Two minutes of `grep -E "^CONFIG_MCUMGR_TRANSPORT"` beat an
hour of probing a serial port at four baud rates, which is what it cost here.

Worth pairing with: when a config is load-bearing for a transport that only a host can exercise, add
a `BUILD_ASSERT` on it. `IS_ENABLED(CONFIG_MCUMGR_TRANSPORT_UART)` failing at compile time names the
problem; a silent port does not.

## Editor diagnostics

clangd in this tree reports `'zephyr/kernel.h' file not found` and unknown Zephyr types. Those are
missing include paths in the LSP configuration, not real errors. **The build is the authority** — do
not "fix" code in response to them.
