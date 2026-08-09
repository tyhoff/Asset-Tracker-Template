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
        -DEXTRA_CONF_FILE=overlay-survey.conf \
        -DEXTRA_DTC_OVERLAY_FILE=overlay-survey.overlay
'
```

Both overlays are required. `overlay-survey.overlay` grows `littlefs_storage` from 1 MiB to 24 MiB,
which is what `CONFIG_APP_STORAGE_MAX_RECORDS_PER_TYPE=25000` in the `.conf` is sized against.
Building with the `.conf` alone links and flashes, then fails an `__ASSERT` inside the LittleFS
backend at boot — the assert message names the block counts, so it is diagnosable, but the device is
dead until it is reflashed. Confirm the partition took by checking the generated devicetree rather
than trusting the command line:

```sh
grep -A3 'littlefs_storage:' build-att-survey/app/zephyr/zephyr.dts
# reg = < 0x4d2000 0x1800000 >;
```

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

### Do not diagnose from the log output

No log lines appeared on either CDC port during normal bench operation, including across a scan, so
absence of logging says nothing about whether the device is healthy. Diagnose from the shell instead:

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

### Capacity arithmetic on the LittleFS backend

Record capacity is bounded by **blocks, not bytes**. The backend stores one file per block
(`get_file_index()` is `index / entries_per_block`), so *N* records mean *N*/`entries_per_block`
files, each consuming a whole erase block plus its directory metadata.

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

## Before committing

1. Both builds pass (survey overlay **and** default).
2. Unit tests pass, and any new test appears in the per-test output.
3. Behaviour verified on target — every commit must be flashable and working, not merely compiling.
4. A senior-firmware-engineer review of the changeset in a separate context.

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

## Editor diagnostics

clangd in this tree reports `'zephyr/kernel.h' file not found` and unknown Zephyr types. Those are
missing include paths in the LSP configuration, not real errors. **The build is the authority** — do
not "fix" code in response to them.
