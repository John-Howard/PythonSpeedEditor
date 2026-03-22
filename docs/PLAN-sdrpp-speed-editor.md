# SDR++ × DaVinci Speed Editor — Integration Plan

## Problem

The DaVinci Speed Editor is a beautifully built USB HID controller with a weighted jog wheel, shuttle ring, and 20+ backlit keys — but it’s locked to DaVinci Resolve. We already have a working Python driver that authenticates, reads inputs, and controls LEDs. The goal is to use it as a physical tuning surface for SDR++.

-----

## Architecture Decision: Two Viable Approaches

### Option A — External Python bridge via rigctl (recommended to start)

```
┌─────────────────┐       TCP :4532       ┌────────────────┐
│  Speed Editor    │  ←── HID USB ──→      │                │
│  Python daemon   │  ──── rigctl ───→     │    SDR++       │
│  (our code)      │  ←── rigctl ────      │  (unmodified)  │
└─────────────────┘                        └────────────────┘
```

SDR++ ships with a **rigctl_server** module that accepts hamlib-compatible TCP commands on a configurable port (default 4532). Our existing Python driver reads the Speed Editor and sends `F` (set freq), `f` (get freq), `M` (set mode), `m` (get mode) commands over a TCP socket. No C++ compilation, no SDR++ source tree, works with any SDR++ release.

**Pros:** Fast to build, no C++ toolchain, portable, works with stock SDR++, easy to iterate.
**Cons:** Limited to what rigctl exposes (frequency, mode, VFO). No waterfall zoom, no bandwidth control, no FFT, no recording control, no direct LED feedback from SDR++ state.

### Option B — Native C++ misc_module plugin

```
┌─────────────────────────────────────────────────────┐
│                      SDR++                          │
│  ┌───────────────────────────────────────────┐      │
│  │  speed_editor_ctrl  (misc_module, .dylib) │      │
│  │  - links hidapi                           │      │
│  │  - calls tuner::tune(), gui::waterfall    │      │
│  │  - reads HID on background thread         │      │
│  └───────────────────────────────────────────┘      │
└─────────────────────────────────────────────────────┘
            ↕ USB HID
     ┌──────────────┐
     │ Speed Editor  │
     └──────────────┘
```

Build a shared library (`speed_editor_ctrl.dylib`/`.so`/`.dll`) that loads inside SDR++ as a `misc_module`. It links `hidapi`, runs our authentication + read loop on a background thread, and calls SDR++ internal APIs directly: `tuner::tune()`, `gui::waterfall`, `sigpath::iqFrontEnd`, recorder interface, etc. Full access to everything.

**Pros:** Complete control — waterfall zoom, bandwidth, gain, recording, VFO switching, LED feedback from live SDR++ state.
**Cons:** Requires building against the SDR++ source tree, C++ toolchain, version-coupled, platform-specific binaries.

### Option C — Hybrid (best of both worlds, phase 2)

Thin C++ plugin exposes a richer local TCP/WebSocket API beyond rigctl. Python daemon handles HID. Lets us evolve the protocol without recompiling SDR++ constantly.

### Recommendation

**Start with Option A** — it can be built in a day with our existing Python code and validates the physical ergonomics. Then graduate to **Option B** for features that rigctl can’t reach.

-----

## Phase 1 — Python rigctl Bridge (1–2 weeks)

### 1.1 Control Mapping

|Speed Editor Input          |SDR++ Action                                                           |rigctl Command                  |
|----------------------------|-----------------------------------------------------------------------|--------------------------------|
|**Jog wheel** (JOG mode)    |Fine-tune VFO frequency (±1 kHz steps)                                 |`F <freq>`                      |
|**Jog wheel** (SHUTTLE mode)|Coarse-tune VFO frequency (±10/100 kHz)                                |`F <freq>`                      |
|**Jog wheel** (SCROLL mode) |Waterfall zoom *(rigctl limitation — not available, skip or repurpose)*|—                               |
|**CAM 1–9**                 |Frequency presets / band bookmarks                                     |`F <preset_freq>` + `M <mode>`  |
|**SHTL / JOG / SCRL**       |Switch jog wheel mode (changes step size)                              |Local state only                |
|**CUT**                     |Toggle mute / squelch                                                  |— *(no rigctl equivalent; skip)*|
|**PLAY FWD / REV**          |Scan up / down from current freq                                       |`F <freq±step>` in loop         |
|**STOP**                    |Stop scanning                                                          |Cancel scan loop                |
|**TRANS**                   |Cycle demod mode (AM→FM→USB→LSB→…)                                     |`M <mode> <passband>`           |
|**ESC**                     |Return to last frequency                                               |`F <prev_freq>`                 |
|**SNAP**                    |Round frequency to nearest channel                                     |`F <snapped>`                   |
|**TRIM IN / TRIM OUT**      |Adjust VFO bandwidth ±500 Hz                                           |`M <mode> <bw±500>`             |
|**SOURCE / TIMELINE**       |Switch VFO target (if multi-VFO)                                       |`V <vfo_id>`                    |

### 1.2 LED Feedback

Map LED state to current mode/status — all local logic in the Python daemon:

|LED Group            |Indicates                             |
|---------------------|--------------------------------------|
|**CAM 1–9**          |Active preset (light the selected one)|
|**SHTL / JOG / SCRL**|Current wheel mode                    |
|**CUT**              |Muted indicator                       |
|**PLAY FWD**         |Scanning up                           |
|**PLAY REV**         |Scanning down                         |

### 1.3 Architecture

```
davinci_speed_editor.py    ← existing HID driver (our library)
sdrpp_bridge.py            ← NEW: rigctl TCP client + mapping logic
config.yaml                ← NEW: preset frequencies, step sizes, key mappings
```

**`sdrpp_bridge.py`** structure:

- `RigctlClient` class — TCP socket wrapper, sends/receives rigctl commands
- `SpeedEditorMapper` class — maps HID events to SDR++ actions, manages state (current freq, mode, wheel mode, presets)
- `LedController` class — updates Speed Editor LEDs based on current state
- Main loop: read HID → map → send rigctl → update LEDs

### 1.4 Configuration

```yaml
sdrpp:
  host: 127.0.0.1
  port: 4532

tuning:
  jog_step_hz: 1000           # fine step per detent
  shuttle_step_hz: 25000      # coarse step per detent
  scroll_step_hz: 100000      # scroll step per detent
  snap_grid_hz: 5000          # SNAP rounds to nearest multiple

presets:
  cam1: { freq: 88100000, mode: WFM, label: "FM Broadcast" }
  cam2: { freq: 121500000, mode: AM, label: "Air Guard" }
  cam3: { freq: 144390000, mode: NFM, label: "APRS" }
  cam4: { freq: 145800000, mode: NFM, label: "ISS Downlink" }
  cam5: { freq: 162550000, mode: NFM, label: "NOAA Weather" }
  cam6: { freq: 433920000, mode: NFM, label: "ISM 433" }
  cam7: { freq: 462562500, mode: NFM, label: "FRS Ch 1" }
  cam8: { freq: 1090000000, mode: RAW, label: "ADS-B" }
  cam9: { freq: 137100000, mode: NFM, label: "NOAA APT" }

scan:
  step_hz: 25000
  dwell_ms: 500
```

### 1.5 Deliverables

1. `sdrpp_bridge.py` — the bridge daemon (single file, PEP 723 inline deps)
1. `config.yaml` — default config with sensible amateur/monitoring presets
1. Updated `davinci_speed_editor.py` — refactored as importable library (class-only, `if __name__` guard already exists)
1. `README.md` — setup instructions

### 1.6 Acceptance Criteria

- [ ] Jog wheel tunes SDR++ frequency in real-time with no perceptible lag
- [ ] CAM buttons recall preset frequencies and modes
- [ ] LED on the active CAM button lights up
- [ ] JOG/SHTL/SCRL buttons switch step size, corresponding LED lit
- [ ] TRANS cycles through demod modes
- [ ] Scanning works (PLAY FWD/REV to start, STOP to halt)

-----

## Phase 2 — Native C++ Plugin (2–4 weeks)

### 2.1 Why Upgrade

Phase 1 will expose limitations — the things you *want* to do with a jog wheel on an SDR that rigctl can’t express:

- Smooth waterfall scrolling (zoom in/out, pan left/right)
- Direct gain/attenuation control
- VFO bandwidth drag with the wheel
- Recording start/stop with LED confirmation
- FFT size adjustment
- Multi-VFO management (create/delete/select)
- True squelch control

### 2.2 Module Skeleton

SDR++ misc_modules follow a fixed pattern. Ours would be `misc_modules/speed_editor_ctrl/`:

```
speed_editor_ctrl/
├── CMakeLists.txt
└── src/
    └── main.cpp
```

**CMakeLists.txt:**

```cmake
project(speed_editor_ctrl)
find_package(PkgConfig REQUIRED)
pkg_check_modules(HIDAPI REQUIRED hidapi-libusb)
file(GLOB SRC "src/*.cpp")
include_directories(${HIDAPI_INCLUDE_DIRS})
add_library(speed_editor_ctrl SHARED ${SRC})
target_link_libraries(speed_editor_ctrl PRIVATE sdrpp_core ${HIDAPI_LIBRARIES})
set_target_properties(speed_editor_ctrl PROPERTIES PREFIX "")
```

**main.cpp exports** (required by SDR++ module loader):

```cpp
SDRPP_MOD_INFO {
    "speed_editor_ctrl",
    "Speed Editor Controller",
    "Your Name",
    {0, 1, 0}   // version
};

_INIT_()      { /* register config */ }
_CREATE_INSTANCE_(name) { return new SpeedEditorModule(name); }
_DELETE_INSTANCE_(inst) { delete (SpeedEditorModule*)inst; }
_END_()       { /* cleanup */ }
```

**SpeedEditorModule** class:

- Extends `ModuleManager::Instance`
- `postInit()` — open HID device, authenticate, start reader thread
- `enable()` / `disable()` — pause/resume
- `menuHandler()` — ImGui panel for config (step sizes, preset editor, LED brightness)
- Background thread — reads HID reports, dispatches via SDR++ core API:
  - `tuner::tune(tuner::TUNER_MODE_NORMAL, vfoName, freq)`
  - `gui::waterfall.setViewOffset()` / `setViewBandwidth()`
  - `sigpath::iqFrontEnd.setGain()`
  - Recorder interface for start/stop

### 2.3 Build Integration

Two options:

**A) In-tree** — clone SDR++ repo, add `speed_editor_ctrl` to `misc_modules/`, add CMake option:

```cmake
option(OPT_BUILD_SPEED_EDITOR_CTRL "Build Speed Editor controller" OFF)
if(OPT_BUILD_SPEED_EDITOR_CTRL)
    add_subdirectory(misc_modules/speed_editor_ctrl)
endif()
```

Build with: `cmake .. -DOPT_BUILD_SPEED_EDITOR_CTRL=ON`

**B) Out-of-tree** — standalone CMake project that finds SDR++ headers. More portable but requires matching header versions.

### 2.4 Platform Notes

|Platform|HID Library        |Notes                                               |
|--------|-------------------|----------------------------------------------------|
|macOS   |`hidapi` (Homebrew)|`.dylib` → SDR++ plugins dir. May need code signing.|
|Linux   |`hidapi-libusb`    |`.so` → `/usr/lib/sdrpp/plugins/`. Needs udev rule. |
|Windows |`hidapi` (vcpkg)   |`.dll` → SDR++ modules dir.                         |

### 2.5 Deliverables

1. `speed_editor_ctrl` module source (C++, ~500–800 LOC)
1. `CMakeLists.txt` with platform detection
1. ImGui settings panel
1. udev rule for Linux (`99-speed-editor.rules`)
1. Build instructions per platform

-----

## Phase 3 — Polish & Extras (ongoing)

- **Profile system** — save/load entire key→action mappings per radio activity (HF contesting, satellite tracking, FM DXing, scanner)
- **Frequency memory bank** — CAM keys become paged (SCROLL mode + CAM = 9 pages × 9 presets = 81 frequencies)
- **Squelch-triggered scanning** — scan halts when signal detected, resumes when squelch closes, with LED feedback
- **Waterfall markers** — highlight preset frequencies on the waterfall (via SDR++ bookmark API)
- **Multi-device** — support two Speed Editors (one for VFO A, one for VFO B) by serial number filtering
- **OSD overlay** — display frequency/mode on a small ImGui overlay that appears on jog wheel movement and fades after 2 seconds

-----

## Immediate Next Steps

1. **Refactor** `davinci_speed_editor.py` — ensure `DaVinciSpeedEditor` is cleanly importable as a library (it already mostly is)
1. **Build `sdrpp_bridge.py`** — rigctl TCP client + mapper + main loop
1. **Create `config.yaml`** — ship sensible defaults
1. **Test with live hardware** — Speed Editor + SDR++ + any supported SDR dongle
1. **Iterate on ergonomics** — tune step sizes, decide which buttons feel right for which actions
1. **Document** — README with wiring diagram, setup guide, demo video

-----

## Reference: rigctl Protocol Quick Reference

SDR++ implements a subset of hamlib rigctl. Commands are newline-terminated ASCII over TCP.

|Command        |Description        |Example                       |
|---------------|-------------------|------------------------------|
|`f`            |Get frequency      |→ `f\n` ← `145800000\n`       |
|`F <hz>`       |Set frequency      |→ `F 145800000\n` ← `RPRT 0\n`|
|`m`            |Get mode + passband|→ `m\n` ← `NFM\n12500\n`      |
|`M <mode> <bw>`|Set mode + passband|→ `M USB 3000\n` ← `RPRT 0\n` |
|`v`            |Get VFO            |→ `v\n` ← `VFOA\n`            |
|`V <vfo>`      |Set VFO            |→ `V VFOA\n` ← `RPRT 0\n`     |
|`s`            |Get split          |→ `s\n`                       |
|`q`            |Quit               |Closes connection             |

Modes: `FM`, `WFM`, `AM`, `DSB`, `USB`, `CW`, `LSB`, `RAW`