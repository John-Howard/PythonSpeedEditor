# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Running the Code

This project uses [PEP 723 inline script metadata](https://peps.python.org/pep-0723/) — no build step or separate requirements file. Use `uv run` to execute with auto-resolved dependencies (requires Python >=3.14).

```bash
# HID driver — device scan, LED demo, or interactive mode
uv run davinci_speed_editor.py --scan
uv run davinci_speed_editor.py --demo
uv run davinci_speed_editor.py [--quiet]

# SDR++ bridge
uv run sdrpp_bridge.py [-c config.yaml] [--dry-run] [--quiet]
```

`--dry-run` runs the bridge without a live SDR++ connection (uses `DryRunRigctl`). `--quiet` suppresses raw HID TX/RX logging.

On Linux, udev rules are needed for non-root HID access — see README.md.

## Architecture

Two Python scripts with a clean layered design:

**`davinci_speed_editor.py`** — HID driver library
- Implements a proprietary challenge-response authentication handshake (reverse-engineered; lookup tables `AUTH_EVEN`/`AUTH_ODD`)
- Parses two HID report types: report 0x03 (jog wheel), report 0x04 (buttons, up to 6 simultaneous keycodes)
- Controls 21 LEDs via two output report subsystems: report 0x02 (18 panel LEDs, 3-byte bitmask), report 0x04 (3 jog mode LEDs)
- Exports `DaVinciSpeedEditor`, `WheelEvent`, `ButtonEvent`, `BUTTON_NAMES`, `scan_for_device()`
- Usable as a standalone script (`--scan`, `--demo`) or importable library

**`sdrpp_bridge.py`** — Application logic layer
- Imports `davinci_speed_editor` as a library
- `RigctlClient`: minimal TCP socket wrapper around hamlib rigctl protocol (commands: `f`, `F`, `m`, `M`)
- `Bridge`: maps Speed Editor events to rigctl commands; maintains `BridgeState` (freq, mode, passband, jog mode, active preset, scan state)
- Main loop: read HID event → `bridge.handle_event()` → `bridge.tick_scan()` → `bridge.update_leds()` (50ms poll cycle)
- All control mappings are in `_on_key_down()`; LED state logic is in `update_leds()`

**Configuration** (`config.yaml`): YAML-driven presets, tuning step sizes, mode list, and scan parameters. Parsed into `Config` dataclass with sensible defaults.

## Key Implementation Details

- Authentication tables (`AUTH_EVEN`, `AUTH_ODD`) and LED bit positions are carefully documented in the source — treat these as fixed hardware constants
- `BUTTON_NAMES` maps HID keycodes to semantic names; only keys present in this dict are reported as `ButtonEvent`
- LED updates are throttled (~200ms) to avoid flooding the USB bus
- `RigctlClient` reconnects automatically on TCP errors; bridge survives SDR++ restarts
- Both scripts guard their `main()` with `if __name__ == "__main__"` for clean library import

## Project Roadmap

- **Phase 1** (current): Python rigctl bridge — complete and functional
- **Phase 2** (planned, `PLAN-native-plugin.md`): Native C++ SDR++ plugin for direct API access (waterfall, bandwidth, volume, recording)
