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
        ../Asset-Tracker-Template/app -- -DEXTRA_CONF_FILE=overlay-survey.conf
'
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

## Before committing

1. Both builds pass (survey overlay **and** default).
2. Unit tests pass, and any new test appears in the per-test output.
3. Behaviour verified on target — every commit must be flashable and working, not merely compiling.
4. A senior-firmware-engineer review of the changeset in a separate context.

## Editor diagnostics

clangd in this tree reports `'zephyr/kernel.h' file not found` and unknown Zephyr types. Those are
missing include paths in the LSP configuration, not real errors. **The build is the authority** — do
not "fix" code in response to them.
