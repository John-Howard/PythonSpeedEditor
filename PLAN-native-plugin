# SDR++ Native Speed Editor Plugin — Detailed Plan

## 1. Overview

This document describes the design, functionality, and configuration management for `speed_editor_ctrl`, a native C++ misc_module plugin for SDR++ that turns the Blackmagic DaVinci Speed Editor into a hardware control surface for software-defined radio operation.

The plugin loads inside the SDR++ process, communicates with the Speed Editor over USB HID on a background thread, and drives SDR++ directly through its internal C++ APIs — bypassing the limitations of the rigctl TCP protocol used by the Phase 1 Python bridge.

---

## 2. What the Rigctl Bridge Cannot Do

The Phase 1 Python bridge proved the concept, but rigctl only exposes a narrow slice of SDR++ functionality. The native plugin unlocks everything rigctl cannot reach:

| Capability | Rigctl Bridge | Native Plugin |
|---|---|---|
| Tune VFO frequency | ✓ | ✓ |
| Set demod mode + passband | ✓ | ✓ |
| Waterfall zoom / pan | ✗ | ✓ |
| VFO bandwidth (drag edges) | ✗ | ✓ |
| Source start / stop | ✗ | ✓ |
| Audio volume control | ✗ | ✓ |
| Audio mute toggle | ✗ | ✓ |
| Squelch level adjust | ✗ | ✓ |
| Recording start / stop | ✗ | ✓ |
| FFT size / framerate | ✗ | ✓ |
| Create / delete / select VFOs | ✗ | ✓ |
| Waterfall colour map cycle | ✗ | ✓ |
| Source gain / attenuation | ✗ | ✓ |
| LED feedback from live SDR++ state | ✗ | ✓ |
| ImGui settings panel | ✗ | ✓ |
| Saved per-instance configuration | ✗ | ✓ |

---

## 3. SDR++ Internal APIs Used

The plugin interacts with the following SDR++ core subsystems. All are accessed through headers in `core/src/` and the `sdrpp_core` shared library.

### 3.1 Tuner System (`core/src/gui/tuner.h`)

The primary frequency control interface.

- `tuner::tune(int mode, std::string vfoName, double freq)` — Tunes a VFO to the given frequency. Mode selects behaviour:
  - `TUNER_MODE_NORMAL` — VFO offset moves, SDR retunes only when needed
  - `TUNER_MODE_CENTER` — VFO stays centred, SDR retunes every time
  - `TUNER_MODE_IQ_ONLY` — Direct IQ centre frequency control (no VFO)
- `tuner::normalTuning(std::string vfoName, double freq)` — Convenience wrapper for normal mode

This replaces the rigctl `F` command and adds tuning mode awareness.

### 3.2 VFO Manager (`core/src/signal_path/vfo_manager.h`)

Manages virtual receiver channels on the waterfall.

- `sigpath::vfoManager.createVFO(name, type, ...)` — Create a new VFO
- `sigpath::vfoManager.deleteVFO(name)` — Remove a VFO
- `sigpath::vfoManager.setOffset(name, offset)` — Move VFO within the sampled bandwidth
- `sigpath::vfoManager.setBandwidth(name, bw)` — Set VFO demodulation bandwidth
- `sigpath::vfoManager.getOutputBlockSize(name)` — Query output stream parameters

This enables direct bandwidth control (TRIM IN / TRIM OUT) and multi-VFO management that rigctl cannot express.

### 3.3 Waterfall / FFT GUI (`core/src/gui/widgets/waterfall.h`)

The spectrum display widget.

- `gui::waterfall.setViewOffset(offset)` — Pan the waterfall view left/right
- `gui::waterfall.setViewBandwidth(bw)` — Zoom in/out on the waterfall
- `gui::waterfall.setFFTMin(min)` / `setFFTMax(max)` — Adjust amplitude range
- `gui::waterfall.getViewOffset()` / `getViewBandwidth()` — Read current view

This gives the jog wheel a second purpose: waterfall navigation in a dedicated mode.

### 3.4 Signal Path (`core/src/signal_path/signal_path.h`)

The top-level DSP chain.

- `sigpath::signalPath.start()` / `stop()` — Start/stop the SDR source
- `sigpath::signalPath.setInput(...)` — Select SDR source
- `sigpath::signalPath.getSampleRate()` — Query current sample rate

### 3.5 Audio Volume (`core/src/gui/main_window.h`)

- `audio::setVolume(float vol)` — Set audio output volume (0.0–1.0)
- `audio::getVolume()` — Query current volume

Mapped to the jog wheel in AUDIO LEVEL mode.

### 3.6 Recorder Interface (`misc_modules/recorder/src/main.cpp`)

The recorder exposes a programmatic interface that other modules can use.

- `recorder::setRecordingPath(path)`
- `recorder::startRecording()` / `stopRecording()`
- `recorder::isRecording()` — Query state

### 3.7 Configuration System (`core/src/config.h`)

SDR++ stores all configuration in a single `config.json` file. Each module instance gets a named JSON block.

- `core::configManager.acquire()` — Lock and get reference to JSON root
- `core::configManager.release(changed)` — Release, optionally mark dirty
- Modules read/write their own subtree: `config["speed_editor_ctrl"]["presets"]`

### 3.8 Menu System (`core/src/gui/gui.h`)

- `gui::menu.registerEntry(name, drawCallback, userData, NULL)` — Add a menu panel to the left sidebar
- `gui::menu.removeEntry(name)` — Remove on unload

The draw callback receives an ImGui context and renders the settings panel.

---

## 4. Control Mapping — Full Layout

Every physical control on the Speed Editor is assigned a function. The secondary function (shown in brackets below) is accessed by holding the modifier (CLOSE UP button).

### 4.1 Jog Wheel — Context-Dependent

The jog wheel's function changes based on the active wheel mode:

| Mode | Button | Primary Function | Step Size (default) |
|---|---|---|---|
| JOG | JOG (0x1D) | Fine frequency tune | 100 Hz / tick |
| SHUTTLE | SHTL (0x1C) | Coarse frequency tune | 25 kHz / tick |
| SCROLL | SCRL (0x1E) | Waterfall zoom in/out | 10% / tick |
| AUDIO LEVEL | AUDIO LEVEL (0x2C) | Volume up/down | 2% / tick |

When in SCROLL mode, holding CLOSE UP while turning switches to waterfall pan (left/right) instead of zoom.

### 4.2 Camera Buttons — Frequency Presets

| Button | Keycode | Function |
|---|---|---|
| CAM 1 | 0x33 | Recall preset 1 (freq + mode + bandwidth) |
| CAM 2 | 0x34 | Recall preset 2 |
| ... | ... | ... |
| CAM 9 | 0x3B | Recall preset 9 |
| CLOSE UP + CAM n | — | Store current freq/mode to preset n |

Each preset stores frequency, demod mode, passband bandwidth, and an optional label. Presets persist in SDR++ config.json.

### 4.3 Transport Controls

| Button | Keycode | Function |
|---|---|---|
| STOP/PLAY | 0x3C | Toggle source start/stop |
| TIMELINE | 0x1B | Start scan forward |
| SOURCE | 0x1A | Start scan reverse |

When scanning is active, STOP/PLAY halts the scan (does not stop the source). Scanning advances frequency by the configurable scan step, dwelling for a configurable time at each step.

### 4.4 Mode & Editing Controls

| Button | Keycode | Function |
|---|---|---|
| TRANS | 0x22 | Cycle demod mode (WFM→FM→AM→USB→LSB→CW→DSB→RAW) |
| SNAP | 0x2E | Round frequency to nearest grid step |
| ESC | 0x31 | Recall previous frequency (undo last tune) |
| TRIM IN | 0x09 | Decrease VFO bandwidth |
| TRIM OUT | 0x0A | Increase VFO bandwidth |
| CUT | 0x0F | Toggle recording start/stop |
| DIS | 0x10 | Cycle waterfall colour map |
| SMTH CUT | 0x11 | Toggle squelch on/off |
| SPLIT | 0x2F | Create a new VFO at current frequency |
| RIPL DEL | 0x2B | Delete the currently selected VFO |

### 4.5 Source & View Controls

| Button | Keycode | Function |
|---|---|---|
| FULL VIEW | 0x2D | Reset waterfall zoom to full bandwidth |
| AUDIO LEVEL | 0x2C | Switch jog wheel to volume mode |
| VIDEO ONLY | 0x25 | (Reserved — future: toggle waterfall display) |
| AUDIO ONLY | 0x26 | Toggle audio mute |
| LIVE O/WR | 0x30 | (Reserved — future: toggle tuning mode normal/center) |
| IN | 0x07 | Mark current freq as scan lower bound |
| OUT | 0x08 | Mark current freq as scan upper bound |
| SYNC BIN | 0x1F | (Reserved — future: snap VFO to nearest signal peak) |

### 4.6 Modifier Key

CLOSE UP (0x04) acts as a shift/modifier when held simultaneously with another key:

- CLOSE UP + CAM n = store preset
- CLOSE UP + jog in SCROLL mode = waterfall pan instead of zoom
- CLOSE UP + TRIM IN/OUT = fine bandwidth adjust (100 Hz steps instead of 500 Hz)
- CLOSE UP + SNAP = open frequency direct entry (future: ImGui popup)

---

## 5. LED Feedback

The Speed Editor's LEDs provide immediate visual feedback of SDR++ state without looking at the screen.

### 5.1 Main Panel LEDs (Report 0x02 — 18 bits)

| LED | Indicates |
|---|---|
| CAM 1–9 | Active preset (only the selected one lit) |
| CUT | Recording in progress (blink while recording) |
| TRANS | Demod mode available (always on when source running) |
| SNAP | Frequency is grid-aligned |
| LIVE O/WR | Source is running |
| DIS | Squelch is open (signal present) |
| SMTH CUT | Squelch is enabled |
| CLOSE UP | Modifier held |
| AUDIO ONLY | Audio muted |
| VIDEO ONLY | (Reserved) |

### 5.2 Jog Mode LEDs (Report 0x04 — 3 bits)

| LED | Indicates |
|---|---|
| JOG | Jog mode active (fine tune) |
| SHTL | Shuttle mode active (coarse tune) |
| SCRL | Scroll mode active (waterfall zoom) |

All three off = volume control mode (entered via AUDIO LEVEL button).

### 5.3 LED Update Strategy

LEDs are updated from the SDR++ GUI thread (inside `menuHandler`) at the ImGui frame rate (~60 Hz), not from the HID reader thread. This avoids threading issues and ensures LED state always reflects the true GUI state.

---

## 6. Configuration Management

### 6.1 Storage Location

All configuration lives inside SDR++'s existing `config.json` file, under the module instance key. No separate config files.

```json
{
  "modules": {
    "Speed Editor Controller": {
      "module": "speed_editor_ctrl",
      "serialNumber": "",
      "tuning": {
        "jogStepHz": 100,
        "shuttleStepHz": 25000,
        "scrollZoomPct": 10,
        "volumeStepPct": 2,
        "snapGridHz": 5000,
        "trimStepHz": 500,
        "trimFineStepHz": 100
      },
      "scan": {
        "stepHz": 25000,
        "dwellMs": 300,
        "lowerBoundHz": 0,
        "upperBoundHz": 0
      },
      "modes": ["WFM", "FM", "AM", "USB", "LSB", "CW", "DSB", "RAW"],
      "presets": {
        "1": { "freq": 88100000, "mode": "WFM", "bw": 200000, "label": "FM Broadcast" },
        "2": { "freq": 121500000, "mode": "AM", "bw": 10000, "label": "Air Guard" },
        "3": { "freq": 144390000, "mode": "FM", "bw": 12500, "label": "APRS 2m" },
        "4": { "freq": 145800000, "mode": "FM", "bw": 12500, "label": "ISS Downlink" },
        "5": { "freq": 162550000, "mode": "FM", "bw": 12500, "label": "NOAA Weather" },
        "6": { "freq": 433920000, "mode": "FM", "bw": 12500, "label": "ISM 433" },
        "7": { "freq": 462562500, "mode": "FM", "bw": 12500, "label": "FRS Ch 1" },
        "8": { "freq": 1090000000, "mode": "RAW", "bw": 0, "label": "ADS-B" },
        "9": { "freq": 137100000, "mode": "FM", "bw": 12500, "label": "NOAA APT" }
      }
    }
  }
}
```

### 6.2 ImGui Settings Panel

The plugin registers a menu entry in SDR++'s left sidebar. The panel provides:

**Device section:**
- Device status indicator (connected / disconnected / authenticating)
- Serial number filter (for multi-device setups — leave blank for first found)
- Reconnect button

**Tuning section:**
- Jog step size (Hz) — spinner
- Shuttle step size (Hz) — spinner
- Snap grid size (Hz) — spinner
- Trim step size (Hz) — spinner

**Scan section:**
- Scan step size (Hz) — spinner
- Dwell time (ms) — slider
- Lower/upper bound display (set via IN/OUT buttons on hardware)

**Presets section:**
- 9-row table: preset number, frequency, mode, bandwidth, label
- Each row has Edit / Clear buttons
- "Store current" checkbox (when checked, next CAM press stores instead of recalls)

**Mode cycle:**
- Reorderable list of demod modes to cycle through

All changes are saved immediately to config.json via the SDR++ ConfigManager API.

### 6.3 Multi-Instance Support

SDR++'s Module Manager allows creating multiple instances of any misc_module. Each instance gets its own config block and can target a different Speed Editor by serial number. This supports the dual-device scenario: one editor for VFO A, another for VFO B.

---

## 7. Threading Model

```
┌──────────────────────────────────────────────────────────┐
│  SDR++ Process                                           │
│                                                          │
│  GUI Thread (main)              HID Reader Thread        │
│  ┌─────────────────────┐       ┌──────────────────────┐  │
│  │ menuHandler()       │       │ hidReaderLoop()      │  │
│  │  - draw ImGui panel │       │  - authenticate()    │  │
│  │  - update LEDs      │◄──────│  - read_event()      │  │
│  │  - apply queued     │ event │  - push to queue     │  │
│  │    actions to       │ queue │                      │  │
│  │    SDR++ APIs       │       │ (runs at ~100Hz      │  │
│  │                     │       │  read timeout)       │  │
│  └─────────────────────┘       └──────────────────────┘  │
│            │                                             │
│            ▼                                             │
│  SDR++ Core APIs (tuner, waterfall, sigpath, etc.)       │
└──────────────────────────────────────────────────────────┘
```

**HID reader thread** — Dedicated thread runs the HID open/auth/read loop. Parsed events (WheelEvent, ButtonEvent) are pushed into a lock-free single-producer single-consumer queue. This thread never calls SDR++ APIs directly.

**GUI thread** — The `menuHandler` callback (called every ImGui frame) drains the event queue, maps events to SDR++ API calls, updates LED state, and draws the settings panel. All SDR++ API calls happen on this thread, which is the only safe context for them.

**Queue** — A simple `std::mutex`-guarded `std::deque` is sufficient given the low event rate (~100 events/sec maximum). No need for a lock-free queue at this throughput.

---

## 8. Module Lifecycle

### 8.1 Initialisation

```
_INIT_()
  └─ Register config defaults if absent

_CREATE_INSTANCE_(name)
  └─ new SpeedEditorModule(name)
       ├─ Load config from config.json
       ├─ Register menu entry
       └─ Start HID reader thread
            ├─ Enumerate USB devices
            ├─ Open matching device (by serial or first found)
            ├─ Run authentication handshake
            ├─ Send init reports
            └─ Enter read loop → push events to queue
```

### 8.2 Steady State

```
Every GUI frame (~16ms):
  menuHandler()
    ├─ Drain event queue
    │    ├─ WheelEvent → tune / zoom / volume
    │    └─ ButtonEvent → preset / mode / scan / record / etc
    ├─ Tick scan timer (advance if due)
    ├─ Update LED state from SDR++ state
    │    ├─ set_leds() for main panel
    │    └─ set_jog_leds() for mode indicators
    └─ Draw ImGui panel (if sidebar visible)
```

### 8.3 Shutdown

```
~SpeedEditorModule()
  ├─ Signal HID thread to stop
  ├─ Join HID thread
  ├─ Turn off all LEDs
  ├─ Close HID device
  └─ Remove menu entry

_DELETE_INSTANCE_(inst)
  └─ delete inst

_END_()
  └─ (nothing — stateless)
```

### 8.4 Hot Plug / Reconnect

If the HID device disconnects (USB cable pulled), the reader thread detects the read failure, closes the device handle, and enters a reconnect polling loop (attempt every 2 seconds). When the device reappears, it re-authenticates and resumes. The ImGui panel shows connection status throughout.

---

## 9. File Structure

```
SDRPlusPlus/
└── misc_modules/
    └── speed_editor_ctrl/
        ├── CMakeLists.txt
        └── src/
            ├── main.cpp              Module entry points + SpeedEditorModule class
            ├── hid_device.h/.cpp     HID open / auth / read / LED control
            ├── event_types.h         WheelEvent, ButtonEvent structs
            ├── key_map.h             Keycode constants + LED bit positions
            ├── bridge.h/.cpp         Event → SDR++ action mapping + state machine
            └── config.h/.cpp         JSON config read/write helpers
```

Estimated total: 800–1200 lines of C++.

---

## 10. Build Integration

### 10.1 CMakeLists.txt

```cmake
cmake_minimum_required(VERSION 3.13)
project(speed_editor_ctrl)

if(MSVC)
    # Windows: use vcpkg hidapi
    find_package(hidapi CONFIG REQUIRED)
    set(HIDAPI_LIBRARIES hidapi::hidapi)
else()
    find_package(PkgConfig REQUIRED)
    if(APPLE)
        pkg_check_modules(HIDAPI REQUIRED hidapi)
    else()
        pkg_check_modules(HIDAPI REQUIRED hidapi-libusb)
    endif()
endif()

file(GLOB SRC "src/*.cpp")

add_library(speed_editor_ctrl SHARED ${SRC})
target_include_directories(speed_editor_ctrl PRIVATE
    ${HIDAPI_INCLUDE_DIRS}
    "src/"
)
target_link_libraries(speed_editor_ctrl PRIVATE
    sdrpp_core
    ${HIDAPI_LIBRARIES}
)
set_target_properties(speed_editor_ctrl PROPERTIES PREFIX "")

# Install to SDR++ plugins directory
install(TARGETS speed_editor_ctrl DESTINATION lib/sdrpp/plugins)
```

### 10.2 Top-Level Integration

Add to the SDR++ root `CMakeLists.txt`:

```cmake
option(OPT_BUILD_SPEED_EDITOR_CTRL "Build DaVinci Speed Editor controller" OFF)
if(OPT_BUILD_SPEED_EDITOR_CTRL)
    add_subdirectory("misc_modules/speed_editor_ctrl")
endif()
```

### 10.3 Build Commands

**macOS M1:**
```bash
cd SDRPlusPlus/build
cmake .. -DOPT_BUILD_SPEED_EDITOR_CTRL=ON \
         -DOPT_BUILD_PORTAUDIO_SINK=ON \
         -DOPT_BUILD_NEW_PORTAUDIO_SINK=ON \
         -DOPT_BUILD_AUDIO_SINK=OFF \
         -DUSE_BUNDLE_DEFAULTS=ON \
         -DCMAKE_BUILD_TYPE=Release
make -j$(sysctl -n hw.logicalcpu) speed_editor_ctrl
```

**Linux x86_64:**
```bash
cd SDRPlusPlus/build
cmake .. -DOPT_BUILD_SPEED_EDITOR_CTRL=ON \
         -DCMAKE_BUILD_TYPE=Release
make -j$(nproc) speed_editor_ctrl
```

Note: you can build just the plugin target without rebuilding all of SDR++ by specifying `speed_editor_ctrl` as the make target.

---

## 11. Platform-Specific Notes

### 11.1 macOS

- hidapi uses the IOKit backend on macOS (not libusb) — `pkg-config hidapi` resolves correctly via Homebrew
- The plugin binary is a `.dylib` that goes inside the SDR++.app bundle at `Contents/MacOS/plugins/`
- Code signing may be required on newer macOS versions — `codesign --force --sign - speed_editor_ctrl.dylib`
- The Speed Editor appears as multiple HID interfaces; we open by VID/PID which selects the correct one

### 11.2 Linux

- hidapi uses libusb by default — `pkg-config hidapi-libusb`
- Plugin binary is a `.so` placed in `/usr/lib/sdrpp/plugins/` or the development `root_dev/modules/` directory
- USB HID access requires either root or a udev rule:

```udev
# /etc/udev/rules.d/99-speed-editor.rules
SUBSYSTEM=="usb", ATTR{idVendor}=="1edb", ATTR{idProduct}=="da0e", MODE="0666"
SUBSYSTEM=="hidraw", ATTRS{idVendor}=="1edb", ATTRS{idProduct}=="da0e", MODE="0666"
```

Reload with: `sudo udevadm control --reload-rules && sudo udevadm trigger`

---

## 12. Development Milestones

### Milestone 1 — Skeleton (1 week)
- Module compiles and loads in SDR++
- Appears in Module Manager
- ImGui panel draws (placeholder content)
- HID thread opens device and authenticates
- LED chase on connect (visual confirmation)

### Milestone 2 — Core Controls (1 week)
- Jog wheel tunes frequency in JOG and SHUTTLE modes
- CAM 1–9 recall presets
- TRANS cycles demod mode
- SNAP rounds to grid
- ESC recalls previous frequency
- JOG/SHTL/SCRL LEDs track active mode

### Milestone 3 — Extended Controls (1 week)
- TRIM IN/OUT adjusts VFO bandwidth via VFO Manager
- SCROLL mode controls waterfall zoom
- AUDIO LEVEL mode controls volume
- CUT toggles recording
- Source start/stop via STOP/PLAY
- FULL VIEW resets waterfall zoom
- AUDIO ONLY toggles mute

### Milestone 4 — Scanning & LED Feedback (1 week)
- TIMELINE/SOURCE start forward/reverse scan
- IN/OUT set scan bounds
- Scan respects upper/lower bounds
- All LEDs reflect live SDR++ state
- CUT LED blinks during recording
- Hot-plug reconnect works

### Milestone 5 — Settings Panel & Polish (1 week)
- Full ImGui settings panel
- All tuning parameters editable in UI
- Preset editor (store/edit/clear)
- Config persists across restarts
- Tested on both macOS M1 and Linux x86_64
- udev rule and install documentation

---

## 13. Risk Assessment

| Risk | Impact | Mitigation |
|---|---|---|
| SDR++ API changes between versions | Plugin won't compile | Pin to a specific SDR++ commit; re-test on nightly builds monthly |
| ImGui context not available in menuHandler | Crash | Guard all ImGui calls with null checks; test with panel hidden |
| HID thread races with GUI thread | Corruption | All SDR++ calls on GUI thread; queue is only shared state |
| macOS code signing blocks .dylib | Plugin won't load | Document `codesign` step; investigate notarisation |
| hidapi not finding device on macOS | Can't open | Fall back to path-based open; add serial number filter |
| volk build issues on M1 | Can't build SDR++ | Use pre-built volk from CI artifacts or Homebrew tap |

---

## 14. Future Extensions

These are explicitly out of scope for the initial implementation but documented for future consideration:

- **Profile system** — Save/load entire key mappings per activity (HF contest, satellite, scanner)
- **Paged presets** — SCROLL mode + CAM = 9 pages × 9 presets = 81 frequencies
- **Squelch-triggered scan halt** — Pause scan when signal exceeds squelch threshold, resume after timeout
- **Signal peak snap** — SYNC BIN button snaps VFO to nearest signal peak in FFT data
- **OSD overlay** — Frequency/mode popup that appears on jog wheel movement, fades after 2 seconds
- **Dual device** — Two Speed Editors controlling two VFOs independently
- **Frequency bank import/export** — CSV/JSON import of frequency lists to preset pages
