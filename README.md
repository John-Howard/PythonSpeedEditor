# speededitor

Python driver and SDR++ bridge for the Blackmagic DaVinci Speed Editor.

The Speed Editor is a USB HID controller with a weighted jog wheel, shuttle ring, and 30+ backlit keys. This project unlocks it from DaVinci Resolve and repurposes it as a physical tuning surface for [SDR++](https://github.com/AlexandreRouma/SDRPlusPlus) — or anything else you want to control.

## What works today

### HID Driver (`davinci_speed_editor.py`)

- **Full HID authentication** — the proprietary challenge-response handshake
- **Jog wheel** — signed delta with mode detection (jog / shuttle / scroll)
- **All 34 buttons** — verified keycodes with named labels, 6-key rollover
- **Two LED subsystems** — 18 main panel LEDs (report 0x02, 3-byte bitmask) and 3 jog mode LEDs (report 0x04, separate byte) with documented bit positions
- **Structured events** — `WheelEvent`, `ButtonEvent`, `UnknownEvent` dataclasses for clean integration
- **Device scanning** — enumerate without opening
- **LED chase demo** — slow walk through every LED by name for mapping verification
- **Clean shutdown** — Ctrl-C handled properly, LEDs turned off on exit

### SDR++ Bridge (`sdrpp_bridge.py`)

- **Jog wheel tuning** — three step sizes (JOG: fine, SHTL: coarse, SCRL: band) switchable via dedicated buttons
- **9 frequency presets** — CAM 1–9 buttons recall frequency + mode + passband
- **Mode cycling** — TRANS button cycles through WFM → FM → AM → USB → LSB → CW → DSB → RAW
- **Passband adjust** — TRIM IN / TRIM OUT buttons widen or narrow the demod bandwidth
- **Frequency snap** — SNAP rounds to nearest configurable grid step
- **Frequency recall** — ESC returns to previous frequency
- **Scanning** — TIMELINE scans forward, SOURCE scans reverse, STOP/PLAY halts
- **LED feedback** — active preset, jog mode, and scan state reflected on hardware LEDs
- **Dry-run mode** — test the full mapping logic without SDR++ connected
- **YAML configuration** — presets, step sizes, mode list, scan parameters all configurable

### Native C++ SDR++ Plugin (`plugin/`)

- **Out-of-tree `misc_module`** — builds against an existing SDR++ source tree (Linux)
- **Direct API access** — waterfall zoom/pan, gain, bandwidth, recording, VFO management
- **Full HID driver in C++** — authentication, jog wheel, buttons, and LED control without Python
- **See [Button layout guide](docs/speed_editor_guides.html)** for a visual reference of all control mappings

## Requirements

- Python ≥ 3.14
- [uv](https://docs.astral.sh/uv/)
- macOS (Apple Silicon or Intel) or Linux (x86_64)
- On Linux: a udev rule for HID access (see below)

No manual dependency install — `uv run` resolves everything via [PEP 723](https://peps.python.org/pep-0723/) inline metadata.

## Quick start

### HID driver (standalone test)

```bash
# Scan for the device
uv run davinci_speed_editor.py --scan

# LED chase demo — walks every LED by name (1.5s per step)
uv run davinci_speed_editor.py --demo

# Normal mode — authenticate and print inputs
uv run davinci_speed_editor.py

# Suppress raw HID TX/RX logging
uv run davinci_speed_editor.py --quiet
```

### SDR++ bridge

1. In SDR++, open **Module Manager**, add `rigctl_server`, and start it on port 4532.
2. Run the bridge:

```bash
# Connect to SDR++ and start controlling
uv run sdrpp_bridge.py --quiet

# Custom config file
uv run sdrpp_bridge.py -c my_config.yaml --quiet

# Dry run — no SDR++ needed, prints what would happen
uv run sdrpp_bridge.py --dry-run
```

### Controls reference

| Control | Function |
|---|---|
| **Jog wheel** | Tune frequency (step depends on mode) |
| **JOG / SHTL / SCRL** | Switch jog step size |
| **CAM 1–9** | Recall frequency preset |
| **TRANS** | Cycle demod mode |
| **SNAP** | Round frequency to grid |
| **TRIM IN / OUT** | Adjust passband ±500 Hz |
| **TIMELINE** | Scan forward |
| **SOURCE** | Scan reverse |
| **STOP/PLAY** | Stop scanning |
| **ESC** | Recall previous frequency |
| **IN / OUT** | Mark frequency (logged) |

## Configuration

Edit `config.yaml` to customise presets, step sizes, and the mode cycle list. See the file for the full schema. Example preset:

```yaml
presets:
  1: { freq: 145800000, mode: FM, passband: 12500, label: "ISS Downlink" }
```

## LED mapping

The Speed Editor has two independent LED subsystems, both fully documented with verified bit positions:

**Main panel** — 18 LEDs controlled via HID report 0x02 (3-byte bitmask):

| Byte 1 | Byte 2 | Byte 3 |
|---|---|---|
| CLOSE_UP, CUT, DIS, SMTH_CUT, TRANS, SNAP, CAM7, CAM8 | CAM9, LIVE_OWR, CAM4, CAM5, CAM6, VIDEO_ONLY, CAM1, CAM2 | CAM3, AUDIO_ONLY |

**Jog mode** — 3 LEDs controlled via HID report 0x04 (1-byte bitmask): JOG, SHTL, SCRL.

Run `--demo` to visually verify each LED on your unit.

## Linux udev rule

Create `/etc/udev/rules.d/99-speed-editor.rules`:

```
SUBSYSTEM=="usb", ATTR{idVendor}=="1edb", ATTR{idProduct}=="da0e", MODE="0666"
SUBSYSTEM=="hidraw", ATTRS{idVendor}=="1edb", ATTRS{idProduct}=="da0e", MODE="0666"
```

Then reload: `sudo udevadm control --reload-rules && sudo udevadm trigger`

## Project structure

```
davinci_speed_editor.py     HID driver — auth, read, LEDs, event types
sdrpp_bridge.py             SDR++ bridge — rigctl TCP client + mapping
config.yaml                 Default presets, step sizes, mode list
plugin/                     Native C++ SDR++ plugin (out-of-tree build)
  CMakeLists.txt            Out-of-tree cmake config
  src/                      Plugin source (HID driver, key map, main module)
docs/
  PLAN-sdrpp-speed-editor.md  Phase 1 bridge design + overall architecture
  PLAN-native-plugin.md       Phase 2 native C++ plugin detailed plan
  speed_editor_guides.html    Visual button layout reference
README.md                   This file
LICENSE.txt                 BSD 3-Clause
```

## Roadmap

| Phase | Status | Description |
|---|---|---|
| HID driver | ✅ Done | Auth, read, LEDs, structured events, verified keycodes |
| SDR++ rigctl bridge | ✅ Done | Python daemon maps Speed Editor → SDR++ via rigctl TCP |
| Native C++ plugin (Linux) | ✅ Done | `misc_module` for waterfall zoom/pan, gain, bandwidth, recording, VFO management |

The native plugin unlocks everything rigctl can't reach — waterfall control, direct VFO bandwidth, source start/stop, recording, squelch, volume, and full LED feedback from live SDR++ state. See [PLAN-native-plugin.md](docs/PLAN-native-plugin.md) for the detailed design.

## Building the native plugin

The C++ plugin builds **out-of-tree** against an existing SDR++ source tree and build directory.

### Prerequisites

1. **SDR++ source and build** — clone and build [SDR++](https://github.com/AlexandreRouma/SDRPlusPlus) following its own instructions:

   ```bash
   git clone https://github.com/AlexandreRouma/SDRPlusPlus.git ~/SDRPlusPlus
   cd ~/SDRPlusPlus
   mkdir build && cd build
   cmake .. -DCMAKE_BUILD_TYPE=Release
   make -j$(nproc)
   ```

2. **hidapi** — the plugin uses `hidapi-libusb` for HID communication:

   **Linux:** `sudo apt install libhidapi-dev`
   **macOS:** `brew install hidapi`

### Build

```bash
cd plugin
cmake -B build \
      -DSDRPP_SOURCE=$HOME/SDRPlusPlus \
      -DSDRPP_BUILD=$HOME/SDRPlusPlus/build
cmake --build build
```

This produces `plugin/build/speed_editor_ctrl.so`. Adjust `SDRPP_SOURCE` and `SDRPP_BUILD` if your SDR++ lives elsewhere.

### Install

Copy the built plugin into your SDR++ modules directory:

```bash
cp plugin/build/speed_editor_ctrl.so ~/.config/sdrpp/modules/
```

Then restart SDR++ and load `speed_editor_ctrl` from **Module Manager**.

See [PLAN-native-plugin.md](docs/PLAN-native-plugin.md) for the detailed plugin design and development milestones.

## License

BSD 3-Clause. See [LICENSE.txt](LICENSE.txt).

## Acknowledgements

The authentication and LED mapping were reverse-engineered by others — this project translates their work into Python and builds the SDR++ integration on top.

- [@smunaut](https://github.com/smunaut/blackmagic-misc) — original protocol analysis, LED and jog mode documentation
- [@davidgiven](https://github.com/davidgiven/bmdkey) — documentation and tooling
- [@Haavard15](https://github.com/Haavard15/SpeedEditorHID/tree/test) — JavaScript implementation
- [Tractus.Hid.DaVinciSpeedEditor](https://github.com/nicktractus/Tractus.Hid.DaVinciSpeedEditor) — C# implementation this project was derived from
