# Survey module

The survey module turns the device into a **radio-survey data exporter**: it captures GNSS fixes
alongside the cell and Wi-Fi observations visible at the same moment, so that the accuracy of
resolving position from cell/Wi-Fi can be measured off-device against GNSS ground truth.

The module is **capture and export only**. It never resolves position on the device and never calls
the nRF Cloud ground-fix service. All analysis happens on a host, from the exported data.

Enabled with `CONFIG_APP_SURVEY`, which is **disabled by default** — the asset-tracker application
behaves identically when it is off. Build it with the provided overlay:

```
west build -b thingy91x/nrf9151/ns --sysbuild app -- -DEXTRA_CONF_FILE=overlay-survey.conf
```

See `REQUIREMENTS.md` in the repository root for the full specification.

## Operation

The module observes the `location_chan` channel as a zbus **listener** (like the LED module, and
unlike the modules that own a state machine) and caches the most recent observation of each kind:

- `LOCATION_GNSS_DATA` — the GNSS fix, with its timestamp.
- `LOCATION_CLOUD_REQUEST` — the cell and Wi-Fi scan. Available because
  `CONFIG_LOCATION_SERVICE_EXTERNAL=y` makes the Location library hand raw scan data back to the
  application instead of resolving it.

The listener callback runs in the publisher's context with the channel mutex held, so it only copies
the message and returns. Nothing in it can block.

At this stage the latest fix and the latest scan are cached **independently**; they are not yet
assembled into a matched, timestamped record.

### Time base

Timestamps are Unix time when the system clock has been synchronised and uptime when it has not —
the Location library falls back to uptime silently. Which clock was used is recorded alongside every
cached observation and shown by the console, because an uptime-stamped observation cannot be
correlated with anything off-device.

## Shell commands

Registered under `survey` when `CONFIG_APP_SURVEY_SHELL` is enabled (default `y`).

| Command | Purpose |
|---|---|
| `survey scan` | Request a Wi-Fi + cellular scan. Takes about 5–10 s. |
| `survey gnss` | Request a GNSS-only fix. Seconds once warm; minutes from cold without assistance data. |
| `survey show` | Print the cached GNSS fix and radio scan. |
| `survey stats` | Print observation counters and how old the cached data is. |
| `survey clear` | Discard the cached observation. |
| `survey selftest` | Render a synthetic observation. Needs no SIM, antenna or sky view. |

**Only one search runs at a time.** A trigger that arrives while a search is already in
progress is discarded by the location module, which logs a warning. This matters most after
`survey gnss`, because a cold fix can occupy the module for minutes — a `survey scan` issued
during that window does nothing, and `survey show` would then return the *previous* scan.
Check `survey stats` for the cache age if in doubt.

`survey scan` and `survey gnss` are separate on purpose. The Location library treats its method list
as a **fallback chain and stops at the first method that succeeds**, so a request that includes GNSS
returns a fix and never performs a scan. Obtaining both means issuing two separate requests.

`survey selftest` is the quickest way to confirm a device is working: if it prints sensible values,
everything from the observation cache to the console is functioning, so an empty `survey show`
afterwards points at the radio rather than the firmware. Injected data is marked with a
`SYNTHETIC SELFTEST DATA` banner that persists until a real observation replaces it or
`survey clear` is run, so it can never be mistaken for a measurement.

### Driving the shell from a host

`scripts/survey_console.py` sends these commands over the USB CDC port and prints the reply, so
sessions can be scripted instead of typed into a terminal emulator. It needs `pyserial`, which the
nRF Connect SDK toolchain already provides:

```
nrfutil toolchain-manager launch --ncs-version v3.4.0 -- \
    python3 scripts/survey_console.py --identify
```

The Thingy:91 X presents two CDC ports through the nRF5340 connectivity bridge — the nRF9151
application console and the nRF5340 itself. `--identify` probes the Nordic-VID ports and reports which one answers
the `survey` command; that is the nRF9151.

| Invocation | Purpose |
|---|---|
| `--list` | List candidate serial ports. |
| `--identify` | Probe Nordic candidate ports and show what each answers. |
| `--probe-all` | With `--identify`, also write to non-Nordic ports. |
| `survey selftest` | Run one command and print its reply. |
| `-n 5 -d 12 survey scan` | Repeat a command, five times at 12 s spacing. |
| `--monitor -t 30` | Print incoming output without sending anything. |
| `--loopback survey selftest` | Exercise the script against a built-in fake shell, no hardware. |

Repeating `survey scan` is the check that matters most after a firmware change: the Location library
cannot truly cancel a Wi-Fi scan, so a mishandled request leaves the next one returning `-EBUSY`.
Several scans in a row passing is what demonstrates that path is healthy.

## Reading the output

Field names follow the [nRF Cloud ground-fix API](https://api-docs.nrfcloud.com/): `lte[]` with
`mcc`, `mnc`, `eci`, `tac`, `earfcn`, `pci`, `rsrp`, `rsrq`, `adv` and `nmr[]`, and
`wifi.accessPoints[]` with `macAddress`, `signalStrength`, `channel` and `frequency`. The console
uses the same vocabulary as the API being validated, so nothing has to be translated by eye.

Two conventions matter when reading a capture:

- **`absent` never means zero.** The modem reports several fields as unavailable using sentinel
  values, and the scan struct is zero-initialised, so a Wi-Fi-only result leaves the whole cell block
  at zero. Anything the modem did not measure is printed as `absent`, and a scan with no cellular
  data at all says `no cellular data in this scan`. A printed number is always a real measurement.
- **`rsrp` and `rsrq` are 3GPP index values, not dBm.** The modem reports indices and `lte_lc` passes
  them through unconverted; the ground-fix API expects dBm/dB. Both forms are printed — for example
  `rsrp 55 (idx) = -86 dBm` — so the conversion can be checked against a real network.

Fields marked `not captured yet (firmware limitation)` are reported by the modem but dropped by the
location module's data structures: the physical cell ID of the serving and GCI cells, and the
channel, frequency and band of each access point. They are called out explicitly so that a missing
value is not misread as a radio failure.

SSID is deliberately **not** captured.

## Limitations

- At most `CONFIG_APP_LOCATION_WIFI_APS_MAX` (10) access points and
  `CONFIG_APP_LOCATION_NEIGHBOR_CELLS_MAX` (10) neighbor cells are recorded. Dense urban
  environments will exceed this and be truncated — an accepted tradeoff. Analysis of dense-area
  captures should account for it.
- Timing advance (`adv`) is only measured while the modem is RRC-connected, which this application
  does not stay in. It is passed through when the modem happens to report a valid value and reported
  as `absent` otherwise. It should be treated as usually unavailable.

## Configuration

| Option | Purpose |
|---|---|
| `CONFIG_APP_SURVEY` | Enable the module. Default `n`. |
| `CONFIG_APP_SURVEY_SHELL` | Register the `survey` command group. Default `y`. |
| `CONFIG_APP_SURVEY_LOG_LEVEL` | Module log level. |

Enabling `CONFIG_APP_SURVEY` selects `CONFIG_APP_LOCATION_SCAN_TRIGGER` in the location module,
which is what handles the scan-only request.
