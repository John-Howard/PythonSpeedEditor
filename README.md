# PythonSpeedEditor
Blackmagic DaVinci Speed Editor Python Wrapper

Python driver and SDR++ bridge for the Blackmagic DaVinci Speed Editor.

The Speed Editor is a USB HID controller with a weighted jog wheel, shuttle ring, and 20+ backlit keys. This project unlocks it from DaVinci Resolve and repurposes it as a tuning surface for [SDR++](https://github.com/AlexandreRouma/SDRPlusPlus) — or anything else you want to control.

## What works today

- **Full HID authentication** — the proprietary challenge-response handshake, translated from [smunaut](https://github.com/smunaut/blackmagic-misc), [davidgiven](https://github.com/davidgiven/bmdkey), and [Haavard15](https://github.com/Haavard15/SpeedEditorHID/tree/test)'s reverse-engineering work
- **Jog wheel** — signed delta with mode detection (jog / shuttle / scroll)
- **All buttons** — decoded keycodes with named labels, supports 6-key rollover
- **LED control** — 18 individually addressable LEDs via 4-byte bitmask
- **Device scanning** — enumerate without opening

## Requirements

- Python ≥ 3.14
- [uv](https://docs.astral.sh/uv/)
- macOS, Linux, or Windows
- On Linux: a udev rule or `sudo` for HID access

No manual dependency install needed — `uv run` handles it via [PEP 723](https://peps.python.org/pep-0723/) inline metadata.

## Quick start

```bash
# Scan for the device
uv run davinci_speed_editor.py --scan

# Authenticate, chase LEDs, then read inputs
uv run davinci_speed_editor.py --demo

# Normal mode — authenticate and listen
uv run davinci_speed_editor.py

# Suppress raw HID logging
uv run davinci_speed_editor.py --quiet
```

## Roadmap

| Phase | Status | Description |
|-------|--------|-------------|
| HID driver | ✅ Done | Authenticate, read inputs, control LEDs |
| SDR++ rigctl bridge | 🔜 Next | Python daemon maps Speed Editor → SDR++ via rigctl TCP |
| Native SDR++ plugin | 📋 Planned | C++ misc_module for waterfall, gain, bandwidth, recording |

See [PLAN-sdrpp-speed-editor.md](PLAN-sdrpp-speed-editor.md) for the full integration plan.

## License

BSD 3-Clause. See [LICENSE.txt](LICENSE.txt).

## Acknowledgements

The authentication logic was reverse-engineered by others — this project only translates their work into Python.

- [@smunaut](https://github.com/smunaut/blackmagic-misc) — original protocol analysis
- [@davidgiven](https://github.com/davidgiven/bmdkey) — documentation and tooling
- [@Haavard15](https://github.com/Haavard15/SpeedEditorHID/tree/test) — JavaScript implementation
- [Tractus.Hid.DaVinciSpeedEditor](https://github.com/nicktractus/Tractus.Hid.DaVinciSpeedEditor) — C# implementation this project was derived from