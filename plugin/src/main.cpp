#include <imgui.h>
#include <module.h>
#include <gui/gui.h>
#include <gui/style.h>
#include <gui/tuner.h>
#include <gui/main_window.h>
#include <signal_path/signal_path.h>
#include <core.h>
#include <config.h>
#include <radio_interface.h>
#include <recorder_interface.h>
#include <utils/flog.h>

#include <thread>
#include <mutex>
#include <deque>
#include <atomic>
#include <chrono>
#include <algorithm>
#include <cmath>

#include "hid_device.h"
#include "event_types.h"
#include "key_map.h"

SDRPP_MOD_INFO{
    /* Name:            */ "speed_editor_ctrl",
    /* Description:     */ "DaVinci Speed Editor hardware controller for SDR++",
    /* Author:          */ "jmh",
    /* Version:         */ 0, 5, 0,
    /* Max instances    */ 1
};

ConfigManager config;

// ── Preset data ────────────────────────────────────────────────────

struct Preset {
    double freq = 0;
    int mode = -1;          // RADIO_IFACE_MODE_* or -1 for unset
    float bandwidth = 0;    // 0 = use mode default
    std::string label;
};

// ── Wheel mode (plugin-level) ──────────────────────────────────────

enum class ActiveWheelMode {
    JOG,        // fine tune
    SHUTTLE,    // coarse tune
    SCROLL,     // waterfall zoom
};

// ── Scan direction ─────────────────────────────────────────────────

enum class ScanDir {
    OFF,
    FORWARD,
    REVERSE,
};

// ── Mode cycle table ───────────────────────────────────────────────

struct ModeEntry {
    int radioMode;          // RADIO_IFACE_MODE_*
    const char* name;
    float defaultBw;        // default bandwidth in Hz
};

static const ModeEntry MODE_CYCLE[] = {
    { RADIO_IFACE_MODE_WFM, "WFM", 200000 },
    { RADIO_IFACE_MODE_NFM, "FM",  12500  },
    { RADIO_IFACE_MODE_AM,  "AM",  10000  },
    { RADIO_IFACE_MODE_USB, "USB", 2700   },
    { RADIO_IFACE_MODE_LSB, "LSB", 2700   },
    { RADIO_IFACE_MODE_CW,  "CW",  500    },
    { RADIO_IFACE_MODE_DSB, "DSB", 6000   },
    { RADIO_IFACE_MODE_RAW, "RAW", 0      },
};
static const int MODE_CYCLE_COUNT = sizeof(MODE_CYCLE) / sizeof(MODE_CYCLE[0]);

static const char* modeToName(int radioMode) {
    for (int i = 0; i < MODE_CYCLE_COUNT; i++) {
        if (MODE_CYCLE[i].radioMode == radioMode) { return MODE_CYCLE[i].name; }
    }
    return "?";
}

static int modeCycleIndex(int radioMode) {
    for (int i = 0; i < MODE_CYCLE_COUNT; i++) {
        if (MODE_CYCLE[i].radioMode == radioMode) { return i; }
    }
    return 0;
}

class SpeedEditorModule : public ModuleManager::Instance {
public:
    SpeedEditorModule(std::string name) {
        this->name = name;

        // Load config
        config.acquire();
        if (!config.conf.contains(name)) {
            config.conf[name]["serialFilter"] = "";
            config.conf[name]["jogStepHz"] = 100;
            config.conf[name]["shuttleStepHz"] = 25000;
            config.conf[name]["snapGridHz"] = 5000;
            config.conf[name]["trimStepHz"] = 500;
            config.conf[name]["scrollZoomPct"] = 10;
            config.conf[name]["scanStepHz"] = 25000;
            config.conf[name]["scanDwellMs"] = 300;
            config.conf[name]["presets"] = json::object();
        }
        std::string serialStr = config.conf[name].value("serialFilter", std::string(""));
        strncpy(serialFilterBuf, serialStr.c_str(), sizeof(serialFilterBuf) - 1);
        serialFilterBuf[sizeof(serialFilterBuf) - 1] = '\0';

        jogStepHz = config.conf[name].value("jogStepHz", 100.0);
        shuttleStepHz = config.conf[name].value("shuttleStepHz", 25000.0);
        snapGridHz = config.conf[name].value("snapGridHz", 5000.0);
        trimStepHz = config.conf[name].value("trimStepHz", 500.0);
        scrollZoomPct = config.conf[name].value("scrollZoomPct", 10.0);
        scanStepHz = config.conf[name].value("scanStepHz", 25000.0);
        scanDwellMs = config.conf[name].value("scanDwellMs", 300);

        // Load presets
        if (config.conf[name].contains("presets")) {
            for (auto& [key, val] : config.conf[name]["presets"].items()) {
                int idx = std::atoi(key.c_str());
                if (idx >= 1 && idx <= 9) {
                    Preset& p = presets[idx - 1];
                    p.freq = val.value("freq", 0.0);
                    p.bandwidth = val.value("bw", 0.0f);
                    p.label = val.value("label", "");
                    std::string modeStr = val.value("mode", "");
                    p.mode = -1;
                    for (int i = 0; i < MODE_CYCLE_COUNT; i++) {
                        if (modeStr == MODE_CYCLE[i].name) {
                            p.mode = MODE_CYCLE[i].radioMode;
                            break;
                        }
                    }
                }
            }
        }
        config.release(true);

        gui::menu.registerEntry(name, menuHandler, this, NULL);

        hidRunning = true;
        hidThread = std::thread(&SpeedEditorModule::hidReaderLoop, this);

        flog::info("Speed Editor: module created ({})", name);
    }

    ~SpeedEditorModule() {
        hidRunning = false;
        if (hidThread.joinable()) {
            hidThread.join();
        }

        gui::menu.removeEntry(name);
        flog::info("Speed Editor: module destroyed ({})", name);
    }

    void postInit() {}
    void enable() { enabled = true; }
    void disable() { enabled = false; }
    bool isEnabled() { return enabled; }

private:
    // ── SDR++ API helpers ──────────────────────────────────────────

    double getCurrentFreq() {
        std::string vfo = gui::waterfall.selectedVFO;
        double freq = gui::waterfall.getCenterFrequency();
        if (!vfo.empty() && sigpath::vfoManager.vfoExists(vfo)) {
            freq += sigpath::vfoManager.getOffset(vfo);
        }
        return freq;
    }

    int getCurrentMode() {
        std::string vfo = gui::waterfall.selectedVFO;
        if (!vfo.empty() && core::modComManager.interfaceExists(vfo) &&
            core::modComManager.getModuleName(vfo) == "radio") {
            int mode = -1;
            core::modComManager.callInterface(vfo, RADIO_IFACE_CMD_GET_MODE, NULL, &mode);
            return mode;
        }
        return -1;
    }

    void setMode(int radioMode) {
        std::string vfo = gui::waterfall.selectedVFO;
        if (!vfo.empty() && core::modComManager.interfaceExists(vfo) &&
            core::modComManager.getModuleName(vfo) == "radio") {
            core::modComManager.callInterface(vfo, RADIO_IFACE_CMD_SET_MODE, &radioMode, NULL);
        }
    }

    void setDemodBandwidth(float bw) {
        std::string vfo = gui::waterfall.selectedVFO;
        if (!vfo.empty() && core::modComManager.interfaceExists(vfo) &&
            core::modComManager.getModuleName(vfo) == "radio") {
            core::modComManager.callInterface(vfo, RADIO_IFACE_CMD_SET_BANDWIDTH, &bw, NULL);
        }
    }

    float getDemodBandwidth() {
        std::string vfo = gui::waterfall.selectedVFO;
        if (!vfo.empty() && core::modComManager.interfaceExists(vfo) &&
            core::modComManager.getModuleName(vfo) == "radio") {
            float bw = 0;
            core::modComManager.callInterface(vfo, RADIO_IFACE_CMD_GET_BANDWIDTH, NULL, &bw);
            return bw;
        }
        return 0;
    }

    void tuneToFreq(double freq) {
        std::string vfo = gui::waterfall.selectedVFO;
        if (vfo.empty()) { return; }
        tuner::tune(tuner::TUNER_MODE_NORMAL, vfo, freq);
    }

    bool hasRadioInterface() {
        std::string vfo = gui::waterfall.selectedVFO;
        return !vfo.empty() && core::modComManager.interfaceExists(vfo) &&
               core::modComManager.getModuleName(vfo) == "radio";
    }

    // ── Find the first recorder instance ───────────────────────────

    std::string findRecorder() {
        for (auto const& [_name, inst] : core::moduleManager.instances) {
            std::string mod = core::moduleManager.getInstanceModuleName(_name);
            if (mod == "recorder") { return _name; }
        }
        return "";
    }

    // ── Config persistence ─────────────────────────────────────────

    void savePresetsToConfig() {
        config.acquire();
        json& presetJson = config.conf[name]["presets"];
        presetJson = json::object();
        for (int i = 0; i < 9; i++) {
            if (presets[i].freq > 0) {
                std::string key = std::to_string(i + 1);
                presetJson[key]["freq"] = presets[i].freq;
                presetJson[key]["mode"] = presets[i].mode >= 0 ? modeToName(presets[i].mode) : "";
                presetJson[key]["bw"] = presets[i].bandwidth;
                presetJson[key]["label"] = presets[i].label;
            }
        }
        config.release(true);
    }

    void saveConfigValue(const std::string& key, double value) {
        config.acquire();
        config.conf[name][key] = value;
        config.release(true);
    }

    // ── Handle wheel event (GUI thread) ────────────────────────────

    void handleWheel(const WheelEvent& ev) {
        // Always use activeWheelMode (set by JOG/SHTL/SCRL button presses).
        // The HID ev.mode reflects the hardware encoder mode, not the
        // logical function we want — all three hardware modes produce
        // the same delta format, only the step size should differ.
        switch (activeWheelMode) {
            case ActiveWheelMode::JOG: {
                double freq = getCurrentFreq();
                double newFreq = freq + (jogStepHz * ev.delta);
                if (newFreq < 0) { newFreq = 0; }
                prevFreq = freq;
                tuneToFreq(newFreq);
                break;
            }
            case ActiveWheelMode::SHUTTLE: {
                double freq = getCurrentFreq();
                double newFreq = freq + (shuttleStepHz * ev.delta);
                if (newFreq < 0) { newFreq = 0; }
                prevFreq = freq;
                tuneToFreq(newFreq);
                break;
            }
            case ActiveWheelMode::SCROLL: {
                // Waterfall zoom: adjust view bandwidth by percentage per tick
                double viewBw = gui::waterfall.getViewBandwidth();
                double fullBw = gui::waterfall.getBandwidth();
                if (fullBw <= 0) { break; }

                double factor = 1.0 + (scrollZoomPct / 100.0) * ev.delta;
                // Clamp factor to prevent inversion
                if (factor < 0.1) { factor = 0.1; }

                double newViewBw = viewBw * factor;
                // Clamp to [1kHz, fullBw]
                if (newViewBw > fullBw) { newViewBw = fullBw; }
                if (newViewBw < 1000) { newViewBw = 1000; }

                gui::waterfall.setViewBandwidth(newViewBw);
                break;
            }
        }
    }

    // ── Handle button press (GUI thread) ───────────────────────────

    void handleButton(const ButtonEvent& ev) {
        if (ev.isRelease()) {
            closeUpHeld = false;
            return;
        }

        // Track CLOSE UP modifier state
        bool closeUp = false;
        for (uint16_t kc : ev.keycodes) {
            if (kc == KEY_CLOSE_UP) { closeUp = true; break; }
        }
        closeUpHeld = closeUp;

        for (uint16_t kc : ev.keycodes) {
            if (kc == KEY_CLOSE_UP) { continue; }  // modifier only, handled above

            switch (kc) {

            // ── Jog mode selection ─────────────────────────────────
            case KEY_JOG:
                activeWheelMode = ActiveWheelMode::JOG;
                break;
            case KEY_SHTL:
                activeWheelMode = ActiveWheelMode::SHUTTLE;
                break;
            case KEY_SCRL:
                activeWheelMode = ActiveWheelMode::SCROLL;
                break;

            // ── CAM 1–9: recall or store presets ──────────────────
            case KEY_CAM1: closeUp ? storePreset(0) : recallPreset(0); break;
            case KEY_CAM2: closeUp ? storePreset(1) : recallPreset(1); break;
            case KEY_CAM3: closeUp ? storePreset(2) : recallPreset(2); break;
            case KEY_CAM4: closeUp ? storePreset(3) : recallPreset(3); break;
            case KEY_CAM5: closeUp ? storePreset(4) : recallPreset(4); break;
            case KEY_CAM6: closeUp ? storePreset(5) : recallPreset(5); break;
            case KEY_CAM7: closeUp ? storePreset(6) : recallPreset(6); break;
            case KEY_CAM8: closeUp ? storePreset(7) : recallPreset(7); break;
            case KEY_CAM9: closeUp ? storePreset(8) : recallPreset(8); break;

            // ── TRANS: cycle demod mode ────────────────────────────
            case KEY_TRANS:
                cycleMode();
                break;

            // ── SNAP: round frequency to grid ─────────────────────
            case KEY_SNAP:
                snapToGrid();
                break;

            // ── ESC: recall previous frequency ────────────────────
            case KEY_ESC:
                if (prevFreq > 0) {
                    double currentFreq = getCurrentFreq();
                    tuneToFreq(prevFreq);
                    prevFreq = currentFreq;
                }
                break;

            // ── TRIM IN/OUT: adjust VFO bandwidth ─────────────────
            //     CLOSE UP held = fine step (100 Hz)
            case KEY_TRIM_IN: {
                float bw = getDemodBandwidth();
                if (bw > 0) {
                    float step = closeUp ? 100.0f : trimStepHz;
                    float newBw = bw - step;
                    if (newBw < 100) { newBw = 100; }
                    setDemodBandwidth(newBw);
                }
                break;
            }
            case KEY_TRIM_OUT: {
                float bw = getDemodBandwidth();
                if (bw > 0) {
                    float step = closeUp ? 100.0f : trimStepHz;
                    float newBw = bw + step;
                    setDemodBandwidth(newBw);
                }
                break;
            }

            // ── STOP/PLAY: stop scan, or toggle source ────────────
            case KEY_STOP_PLAY: {
                if (scanDir != ScanDir::OFF) {
                    // If scanning, stop the scan (don't toggle source)
                    scanDir = ScanDir::OFF;
                    flog::info("Speed Editor: scan stopped");
                }
                else {
                    bool playing = gui::mainWindow.isPlaying();
                    gui::mainWindow.setPlayState(!playing);
                    flog::info("Speed Editor: source {}", playing ? "stopped" : "started");
                }
                break;
            }

            // ── TIMELINE: start scan forward ──────────────────────
            case KEY_TIMELINE: {
                scanDir = ScanDir::FORWARD;
                scanNextTime = std::chrono::steady_clock::now();
                flog::info("Speed Editor: scan forward");
                break;
            }

            // ── SOURCE: start scan reverse ────────────────────────
            case KEY_SOURCE: {
                scanDir = ScanDir::REVERSE;
                scanNextTime = std::chrono::steady_clock::now();
                flog::info("Speed Editor: scan reverse");
                break;
            }

            // ── IN: set scan lower bound to current freq ──────────
            case KEY_IN: {
                scanLowerHz = getCurrentFreq();
                flog::info("Speed Editor: scan lower bound = {} Hz", (uint64_t)scanLowerHz);
                break;
            }

            // ── OUT: set scan upper bound to current freq ─────────
            case KEY_OUT: {
                scanUpperHz = getCurrentFreq();
                flog::info("Speed Editor: scan upper bound = {} Hz", (uint64_t)scanUpperHz);
                break;
            }

            // ── CUT: toggle recording ─────────────────────────────
            case KEY_CUT: {
                std::string rec = findRecorder();
                if (!rec.empty()) {
                    if (recording) {
                        core::modComManager.callInterface(rec, RECORDER_IFACE_CMD_STOP, NULL, NULL);
                        recording = false;
                        flog::info("Speed Editor: recording stopped");
                    }
                    else {
                        core::modComManager.callInterface(rec, RECORDER_IFACE_CMD_START, NULL, NULL);
                        recording = true;
                        flog::info("Speed Editor: recording started");
                    }
                }
                break;
            }

            // ── FULL VIEW: reset waterfall zoom to full bandwidth ─
            case KEY_FULL_VIEW: {
                double fullBw = gui::waterfall.getBandwidth();
                if (fullBw > 0) {
                    gui::waterfall.setViewBandwidth(fullBw);
                    gui::waterfall.setViewOffset(0);
                    flog::info("Speed Editor: waterfall reset to full view");
                }
                break;
            }

            // ── AUDIO ONLY: toggle mute ───────────────────────────
            // Note: SinkManager::Stream volume/mute is not publicly
            // accessible. Using squelch toggle as a workaround.
            case KEY_AUDIO_ONLY: {
                if (hasRadioInterface()) {
                    std::string vfo = gui::waterfall.selectedVFO;
                    int sqEnabled = 0;
                    core::modComManager.callInterface(vfo, RADIO_IFACE_CMD_GET_SQUELCH_ENABLED, NULL, &sqEnabled);
                    sqEnabled = sqEnabled ? 0 : 1;
                    core::modComManager.callInterface(vfo, RADIO_IFACE_CMD_SET_SQUELCH_ENABLED, &sqEnabled, NULL);
                    flog::info("Speed Editor: squelch {}", sqEnabled ? "enabled" : "disabled");
                }
                break;
            }

            // ── AUDIO LEVEL: switch to squelch adjust mode ────────
            // (Uses jog wheel to adjust squelch level when held)
            case KEY_AUDIO_LEVEL:
                // Reserved — currently no public volume API
                break;

            default:
                break;
            }
        }
    }

    // ── Recall a preset ────────────────────────────────────────────

    void recallPreset(int idx) {
        if (idx < 0 || idx >= 9) { return; }
        Preset& p = presets[idx];
        if (p.freq <= 0) { return; }

        prevFreq = getCurrentFreq();
        tuneToFreq(p.freq);

        if (p.mode >= 0) {
            setMode(p.mode);
        }
        if (p.bandwidth > 0) {
            setDemodBandwidth(p.bandwidth);
        }

        activePreset = idx;
        flog::info("Speed Editor: preset {} recalled — {} Hz ({})",
                   idx + 1, (uint64_t)p.freq, p.label);
    }

    // ── Store current state to a preset ──────────────────────────────

    void storePreset(int idx) {
        if (idx < 0 || idx >= 9) { return; }
        Preset& p = presets[idx];
        p.freq = getCurrentFreq();
        p.mode = getCurrentMode();
        std::string vfo = gui::waterfall.selectedVFO;
        if (!vfo.empty() && sigpath::vfoManager.vfoExists(vfo)) {
            p.bandwidth = sigpath::vfoManager.getBandwidth(vfo);
        }
        // Keep existing label if present
        activePreset = idx;
        savePresetsToConfig();
        flog::info("Speed Editor: stored preset {} — {} Hz", idx + 1, (uint64_t)p.freq);
    }

    // ── Cycle demod mode ───────────────────────────────────────────

    void cycleMode() {
        int curMode = getCurrentMode();
        if (curMode < 0) { return; }

        int idx = modeCycleIndex(curMode);
        int nextIdx = (idx + 1) % MODE_CYCLE_COUNT;

        setMode(MODE_CYCLE[nextIdx].radioMode);
        if (MODE_CYCLE[nextIdx].defaultBw > 0) {
            setDemodBandwidth(MODE_CYCLE[nextIdx].defaultBw);
        }

        flog::info("Speed Editor: mode -> {}", MODE_CYCLE[nextIdx].name);
    }

    // ── Snap frequency to grid ─────────────────────────────────────

    void snapToGrid() {
        if (snapGridHz <= 0) { return; }
        double freq = getCurrentFreq();
        double snapped = round(freq / snapGridHz) * snapGridHz;
        if (snapped != freq) {
            prevFreq = freq;
            tuneToFreq(snapped);
        }
    }

    // ── Scan tick (called every GUI frame) ───────────────────────────

    void tickScan() {
        if (scanDir == ScanDir::OFF) { return; }

        auto now = std::chrono::steady_clock::now();
        if (now < scanNextTime) { return; }

        double step = scanStepHz * (scanDir == ScanDir::FORWARD ? 1.0 : -1.0);
        double freq = getCurrentFreq();
        double newFreq = freq + step;

        // Respect scan bounds if set
        if (scanLowerHz > 0 && scanUpperHz > 0 && scanUpperHz > scanLowerHz) {
            if (newFreq > scanUpperHz) {
                newFreq = scanLowerHz;
            }
            else if (newFreq < scanLowerHz) {
                newFreq = scanUpperHz;
            }
        }

        if (newFreq < 0) { newFreq = 0; }
        tuneToFreq(newFreq);

        scanNextTime = now + std::chrono::milliseconds(scanDwellMs);
    }

    // ── Update LEDs to reflect state ───────────────────────────────

    void updateLeds() {
        if (hid.getState() != SpeedEditorHID::State::Connected) { return; }

        auto now = std::chrono::steady_clock::now();
        if (std::chrono::duration_cast<std::chrono::milliseconds>(now - lastLedUpdate).count() < 200) {
            return;
        }
        lastLedUpdate = now;

        uint8_t b0 = 0, b1 = 0, b2 = 0;

        // Active preset CAM LED
        if (activePreset >= 0 && activePreset < 9) {
            int ledBit = ledForCam(activePreset + 1);
            if (ledBit >= 0) {
                int byteIdx = ledBit / 8;
                int bit = ledBit % 8;
                if (byteIdx == 0) b0 |= (1 << bit);
                else if (byteIdx == 1) b1 |= (1 << bit);
                else if (byteIdx == 2) b2 |= (1 << bit);
            }
        }

        // SNAP LED: lit when frequency is grid-aligned
        if (snapGridHz > 0) {
            double freq = getCurrentFreq();
            double remainder = fmod(freq, snapGridHz);
            if (remainder < 0.5 || (snapGridHz - remainder) < 0.5) {
                b0 |= (1 << LED_SNAP);
            }
        }

        // TRANS LED: on when radio interface is available
        if (hasRadioInterface()) {
            b0 |= (1 << LED_TRANS);
        }

        // LIVE O/WR LED: on when source is running
        if (gui::mainWindow.isPlaying()) {
            b1 |= (1 << (LED_LIVE_OWR - 8));
        }

        // CUT LED: blink while recording
        if (recording) {
            auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count();
            if ((ms / 500) % 2 == 0) {
                b0 |= (1 << LED_CUT);
            }
        }

        // SMTH CUT LED: on when squelch is enabled
        if (hasRadioInterface()) {
            std::string vfo = gui::waterfall.selectedVFO;
            int sqEnabled = 0;
            core::modComManager.callInterface(vfo, RADIO_IFACE_CMD_GET_SQUELCH_ENABLED, NULL, &sqEnabled);
            if (sqEnabled) {
                b0 |= (1 << LED_SMTH_CUT);
            }
        }

        // CLOSE UP LED: on when modifier held
        if (closeUpHeld) {
            b0 |= (1 << LED_CLOSE_UP);
        }

        // DIS LED: on while scanning
        if (scanDir != ScanDir::OFF) {
            b0 |= (1 << LED_DIS);
        }

        hid.setLeds(b0, b1, b2);

        // Jog mode LEDs
        hid.setJogLeds(
            activeWheelMode == ActiveWheelMode::JOG,
            activeWheelMode == ActiveWheelMode::SHUTTLE,
            activeWheelMode == ActiveWheelMode::SCROLL
        );
    }

    // ── HID reader thread ──────────────────────────────────────────

    void hidReaderLoop() {
        flog::info("Speed Editor: HID reader thread started");

        while (hidRunning) {
            // Handle reconnect request from GUI thread
            if (reconnectRequested) {
                reconnectRequested = false;
                hid.close();
            }

            if (hid.getState() == SpeedEditorHID::State::Disconnected) {
                std::string filter(serialFilterBuf);
                if (!hid.open(filter)) {
                    for (int i = 0; i < 20 && hidRunning && !reconnectRequested; i++) {
                        std::this_thread::sleep_for(std::chrono::milliseconds(100));
                    }
                    continue;
                }

                if (!hid.authenticate()) {
                    hid.close();
                    continue;
                }

                hid.sendInitReports();
                hid.ledChaseDemo();
            }

            SpeedEditorEvent event;
            if (hid.readEvent(event, 100)) {
                std::lock_guard<std::mutex> lock(eventMtx);
                eventQueue.push_back(event);
                while (eventQueue.size() > 256) {
                    eventQueue.pop_front();
                }
            }
        }

        hid.close();
        flog::info("Speed Editor: HID reader thread stopped");
    }

    // ── ImGui menu handler (runs on GUI thread) ────────────────────

    static void menuHandler(void* ctx) {
        SpeedEditorModule* _this = (SpeedEditorModule*)ctx;
        float menuWidth = ImGui::GetContentRegionAvail().x;

        // Drain event queue and dispatch
        {
            std::lock_guard<std::mutex> lock(_this->eventMtx);
            while (!_this->eventQueue.empty()) {
                SpeedEditorEvent& ev = _this->eventQueue.front();
                if (ev.type == EventType::Wheel) {
                    _this->handleWheel(ev.wheel);
                }
                else if (ev.type == EventType::Button) {
                    _this->handleButton(ev.button);
                }
                _this->eventQueue.pop_front();
            }
        }

        _this->tickScan();
        _this->updateLeds();

        // ── Device section ─────────────────────────────────────────
        if (ImGui::CollapsingHeader("Device", ImGuiTreeNodeFlags_DefaultOpen)) {
            auto state = _this->hid.getState();
            const char* stateStr;
            ImVec4 stateColor;
            switch (state) {
                case SpeedEditorHID::State::Connected:
                    stateStr = "Connected";
                    stateColor = ImVec4(0.0f, 1.0f, 0.0f, 1.0f);
                    break;
                case SpeedEditorHID::State::Authenticating:
                    stateStr = "Authenticating...";
                    stateColor = ImVec4(1.0f, 1.0f, 0.0f, 1.0f);
                    break;
                default:
                    stateStr = "Disconnected";
                    stateColor = ImVec4(1.0f, 0.3f, 0.3f, 1.0f);
                    break;
            }

            ImGui::TextColored(stateColor, "Status: %s", stateStr);

            std::string ser = _this->hid.getSerial();
            if (!ser.empty()) {
                ImGui::Text("Serial: %s", ser.c_str());
            }

            ImGui::LeftLabel("Serial filter");
            ImGui::SetNextItemWidth(menuWidth - ImGui::GetCursorPosX());
            if (ImGui::InputText("##serial_filter", _this->serialFilterBuf, sizeof(_this->serialFilterBuf))) {
                config.acquire();
                config.conf[_this->name]["serialFilter"] = std::string(_this->serialFilterBuf);
                config.release(true);
            }
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("Leave blank to use the first device found");
            }

            if (ImGui::Button("Reconnect", ImVec2(menuWidth, 0))) {
                _this->reconnectRequested = true;
            }
        }

        ImGui::Spacing();

        // ── Current state display ──────────────────────────────────
        if (ImGui::CollapsingHeader("Status", ImGuiTreeNodeFlags_DefaultOpen)) {
            double freq = _this->getCurrentFreq();
            int mode = _this->getCurrentMode();
            const char* modeName = mode >= 0 ? modeToName(mode) : "N/A";

            char freqBuf[64];
            if (freq >= 1e9) {
                snprintf(freqBuf, sizeof(freqBuf), "%.6f GHz", freq / 1e9);
            }
            else if (freq >= 1e6) {
                snprintf(freqBuf, sizeof(freqBuf), "%.6f MHz", freq / 1e6);
            }
            else if (freq >= 1e3) {
                snprintf(freqBuf, sizeof(freqBuf), "%.3f kHz", freq / 1e3);
            }
            else {
                snprintf(freqBuf, sizeof(freqBuf), "%.0f Hz", freq);
            }

            ImGui::Text("Freq: %s", freqBuf);
            ImGui::Text("Mode: %s", modeName);

            // Demod bandwidth
            float demodBw = _this->getDemodBandwidth();
            if (demodBw > 0) {
                if (demodBw >= 1000) {
                    ImGui::Text("BW: %.1f kHz", demodBw / 1000.0f);
                }
                else {
                    ImGui::Text("BW: %.0f Hz", demodBw);
                }
            }

            // Wheel mode
            const char* wheelModeNames[] = {"JOG (fine)", "SHUTTLE (coarse)", "SCROLL (zoom)"};
            ImGui::Text("Wheel: %s", wheelModeNames[(int)_this->activeWheelMode]);

            // Source and recording status
            bool playing = gui::mainWindow.isPlaying();
            ImGui::TextColored(playing ? ImVec4(0, 1, 0, 1) : ImVec4(0.5f, 0.5f, 0.5f, 1),
                               "Source: %s", playing ? "Running" : "Stopped");
            if (_this->recording) {
                ImGui::TextColored(ImVec4(1, 0.2f, 0.2f, 1), "Recording...");
            }

            // Scan status
            if (_this->scanDir != ScanDir::OFF) {
                const char* scanDirStr = _this->scanDir == ScanDir::FORWARD ? ">> Forward" : "<< Reverse";
                ImGui::TextColored(ImVec4(1, 1, 0, 1), "Scan: %s", scanDirStr);
            }

            // Scan bounds
            if (_this->scanLowerHz > 0 || _this->scanUpperHz > 0) {
                char boundsBuf[128];
                snprintf(boundsBuf, sizeof(boundsBuf), "Bounds: %.3f - %.3f MHz",
                         _this->scanLowerHz / 1e6, _this->scanUpperHz / 1e6);
                ImGui::Text("%s", boundsBuf);
            }

            // Active preset
            if (_this->activePreset >= 0) {
                Preset& p = _this->presets[_this->activePreset];
                ImGui::Text("Preset: CAM %d", _this->activePreset + 1);
                if (!p.label.empty()) {
                    ImGui::SameLine();
                    ImGui::Text("(%s)", p.label.c_str());
                }
            }
        }

        ImGui::Spacing();

        // ── Presets ────────────────────────────────────────────────
        if (ImGui::CollapsingHeader("Presets", ImGuiTreeNodeFlags_DefaultOpen)) {
            for (int i = 0; i < 9; i++) {
                Preset& p = _this->presets[i];
                ImGui::PushID(i);

                if (p.freq > 0) {
                    // Preset info line
                    char buf[128];
                    snprintf(buf, sizeof(buf), "CAM %d: %.3f MHz %s",
                             i + 1, p.freq / 1e6,
                             p.mode >= 0 ? modeToName(p.mode) : "");
                    ImGui::Text("%s", buf);

                    // Label edit + action buttons on next line
                    float btnW = ImGui::CalcTextSize("Clear").x + ImGui::GetStyle().FramePadding.x * 2;
                    ImGui::SetNextItemWidth(menuWidth - btnW - ImGui::GetStyle().ItemSpacing.x);
                    if (ImGui::InputText("##label", _this->presetLabelBufs[i], sizeof(_this->presetLabelBufs[i]),
                                         ImGuiInputTextFlags_EnterReturnsTrue)) {
                        p.label = _this->presetLabelBufs[i];
                        _this->savePresetsToConfig();
                    }
                    // Sync buffer from preset when not actively editing
                    if (!ImGui::IsItemActive()) {
                        strncpy(_this->presetLabelBufs[i], p.label.c_str(), sizeof(_this->presetLabelBufs[i]) - 1);
                        _this->presetLabelBufs[i][sizeof(_this->presetLabelBufs[i]) - 1] = '\0';
                    }

                    ImGui::SameLine();
                    if (ImGui::Button("Clear")) {
                        p.freq = 0;
                        p.mode = -1;
                        p.bandwidth = 0;
                        p.label.clear();
                        _this->presetLabelBufs[i][0] = '\0';
                        if (_this->activePreset == i) { _this->activePreset = -1; }
                        _this->savePresetsToConfig();
                    }
                }
                else {
                    ImGui::TextDisabled("CAM %d: (empty)", i + 1);
                    ImGui::SameLine();
                    if (ImGui::SmallButton("Store")) {
                        p.freq = _this->getCurrentFreq();
                        p.mode = _this->getCurrentMode();
                        std::string vfo = gui::waterfall.selectedVFO;
                        if (!vfo.empty() && sigpath::vfoManager.vfoExists(vfo)) {
                            p.bandwidth = sigpath::vfoManager.getBandwidth(vfo);
                        }
                        p.label.clear();
                        _this->activePreset = i;
                        _this->savePresetsToConfig();
                        flog::info("Speed Editor: stored preset {}", i + 1);
                    }
                }

                ImGui::PopID();
            }

            ImGui::Spacing();

            if (ImGui::Button("Store current to...", ImVec2(menuWidth, 0))) {
                ImGui::OpenPopup("StorePreset");
            }
            if (ImGui::BeginPopup("StorePreset")) {
                for (int i = 0; i < 9; i++) {
                    char label[32];
                    snprintf(label, sizeof(label), "CAM %d", i + 1);
                    if (ImGui::Selectable(label)) {
                        _this->presets[i].freq = _this->getCurrentFreq();
                        _this->presets[i].mode = _this->getCurrentMode();
                        std::string vfo = gui::waterfall.selectedVFO;
                        if (!vfo.empty() && sigpath::vfoManager.vfoExists(vfo)) {
                            _this->presets[i].bandwidth = sigpath::vfoManager.getBandwidth(vfo);
                        }
                        _this->presets[i].label = "";
                        _this->activePreset = i;
                        _this->savePresetsToConfig();
                        flog::info("Speed Editor: stored preset {}", i + 1);
                    }
                }
                ImGui::EndPopup();
            }
        }

        ImGui::Spacing();

        // ── Tuning settings ────────────────────────────────────────
        if (ImGui::CollapsingHeader("Tuning")) {
            ImGui::LeftLabel("JOG step (Hz)");
            ImGui::SetNextItemWidth(menuWidth - ImGui::GetCursorPosX());
            if (ImGui::InputDouble("##jog_step", &_this->jogStepHz, 10, 1000, "%.0f")) {
                if (_this->jogStepHz < 1) { _this->jogStepHz = 1; }
                _this->saveConfigValue("jogStepHz", _this->jogStepHz);
            }

            ImGui::LeftLabel("SHUTTLE step (Hz)");
            ImGui::SetNextItemWidth(menuWidth - ImGui::GetCursorPosX());
            if (ImGui::InputDouble("##shuttle_step", &_this->shuttleStepHz, 1000, 100000, "%.0f")) {
                if (_this->shuttleStepHz < 1) { _this->shuttleStepHz = 1; }
                _this->saveConfigValue("shuttleStepHz", _this->shuttleStepHz);
            }

            ImGui::LeftLabel("SNAP grid (Hz)");
            ImGui::SetNextItemWidth(menuWidth - ImGui::GetCursorPosX());
            if (ImGui::InputDouble("##snap_grid", &_this->snapGridHz, 100, 10000, "%.0f")) {
                if (_this->snapGridHz < 1) { _this->snapGridHz = 1; }
                _this->saveConfigValue("snapGridHz", _this->snapGridHz);
            }

            ImGui::LeftLabel("TRIM step (Hz)");
            ImGui::SetNextItemWidth(menuWidth - ImGui::GetCursorPosX());
            if (ImGui::InputDouble("##trim_step", &_this->trimStepHz, 50, 500, "%.0f")) {
                if (_this->trimStepHz < 10) { _this->trimStepHz = 10; }
                _this->saveConfigValue("trimStepHz", _this->trimStepHz);
            }

            ImGui::LeftLabel("Zoom %/tick");
            ImGui::SetNextItemWidth(menuWidth - ImGui::GetCursorPosX());
            if (ImGui::InputDouble("##scroll_zoom", &_this->scrollZoomPct, 1, 5, "%.0f")) {
                if (_this->scrollZoomPct < 1) { _this->scrollZoomPct = 1; }
                if (_this->scrollZoomPct > 50) { _this->scrollZoomPct = 50; }
                _this->saveConfigValue("scrollZoomPct", _this->scrollZoomPct);
            }
        }

        ImGui::Spacing();

        // ── Scan settings ──────────────────────────────────────────
        if (ImGui::CollapsingHeader("Scan")) {
            ImGui::LeftLabel("Scan step (Hz)");
            ImGui::SetNextItemWidth(menuWidth - ImGui::GetCursorPosX());
            if (ImGui::InputDouble("##scan_step", &_this->scanStepHz, 1000, 100000, "%.0f")) {
                if (_this->scanStepHz < 100) { _this->scanStepHz = 100; }
                _this->saveConfigValue("scanStepHz", _this->scanStepHz);
            }

            ImGui::LeftLabel("Scan dwell (ms)");
            ImGui::SetNextItemWidth(menuWidth - ImGui::GetCursorPosX());
            if (ImGui::InputInt("##scan_dwell", &_this->scanDwellMs, 50, 500)) {
                if (_this->scanDwellMs < 50) { _this->scanDwellMs = 50; }
                if (_this->scanDwellMs > 5000) { _this->scanDwellMs = 5000; }
                _this->saveConfigValue("scanDwellMs", (double)_this->scanDwellMs);
            }

            // Show current scan bounds (set via IN/OUT buttons on hardware)
            if (_this->scanLowerHz > 0 || _this->scanUpperHz > 0) {
                ImGui::Text("Lower: %.3f MHz", _this->scanLowerHz / 1e6);
                ImGui::Text("Upper: %.3f MHz", _this->scanUpperHz / 1e6);
                if (ImGui::SmallButton("Clear bounds")) {
                    _this->scanLowerHz = 0;
                    _this->scanUpperHz = 0;
                }
            }
            else {
                ImGui::TextDisabled("Bounds: not set (use IN/OUT)");
            }
        }
    }

    // ── Member data ────────────────────────────────────────────────
    std::string name;
    bool enabled = true;

    // HID device and reader thread
    SpeedEditorHID hid;
    std::atomic<bool> hidRunning{false};
    std::atomic<bool> reconnectRequested{false};
    std::thread hidThread;
    char serialFilterBuf[64] = {};

    // Event queue (protected by mutex, drained on GUI thread)
    std::mutex eventMtx;
    std::deque<SpeedEditorEvent> eventQueue;

    // Bridge state (GUI thread only)
    ActiveWheelMode activeWheelMode = ActiveWheelMode::JOG;
    int activePreset = -1;
    double prevFreq = 0;
    bool recording = false;
    bool closeUpHeld = false;

    // Scan state
    ScanDir scanDir = ScanDir::OFF;
    std::chrono::steady_clock::time_point scanNextTime;
    double scanLowerHz = 0;
    double scanUpperHz = 0;

    // Tuning parameters
    double jogStepHz = 100;
    double shuttleStepHz = 25000;
    double snapGridHz = 5000;
    double trimStepHz = 500;
    double scrollZoomPct = 10;
    double scanStepHz = 25000;
    int scanDwellMs = 300;

    // Presets
    Preset presets[9];
    char presetLabelBufs[9][64] = {};

    // LED throttle
    std::chrono::steady_clock::time_point lastLedUpdate;
};

// ── Module entry points ────────────────────────────────────────────

MOD_EXPORT void _INIT_() {
    config.setPath(core::args["root"].s() + "/speed_editor_ctrl_config.json");
    config.load(json::object());
    config.enableAutoSave();
}

MOD_EXPORT ModuleManager::Instance* _CREATE_INSTANCE_(std::string name) {
    return new SpeedEditorModule(name);
}

MOD_EXPORT void _DELETE_INSTANCE_(void* instance) {
    delete (SpeedEditorModule*)instance;
}

MOD_EXPORT void _END_() {
    config.disableAutoSave();
}
