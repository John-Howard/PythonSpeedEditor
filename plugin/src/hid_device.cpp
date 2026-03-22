#include "hid_device.h"
#include "key_map.h"
#include <utils/flog.h>
#include <cstring>
#include <thread>
#include <chrono>

// ── Authentication tables (from smunaut's reverse-engineering) ──────

static const uint64_t AUTH_EVEN[] = {
    0x3AE1206F97C10BC8ULL, 0x2A9AB32BEBF244C6ULL,
    0x20A6F8B8DF9ADF0AULL, 0xAF80ECE52CFC1719ULL,
    0xEC2EE2F7414FD151ULL, 0xB055ADFD73344A15ULL,
    0xA63D2E3059001187ULL, 0x751BF623F42E0DDEULL,
};

static const uint64_t AUTH_ODD[] = {
    0x3E22B34F502E7FDEULL, 0x24656B981875AB1CULL,
    0xA17F3456DF7BF8C3ULL, 0x6DF72E1941AEF698ULL,
    0x72226F011E66AB94ULL, 0x3831A3C606296B42ULL,
    0xFD7FF81881332C89ULL, 0x61A3F6474FF236C6ULL,
};

static const uint64_t AUTH_MASK = 0xA79A63F585D37BF0ULL;

// ── Authentication math ────────────────────────────────────────────

uint64_t SpeedEditorHID::rol8(uint64_t value) {
    return ((value << 56) | (value >> 8)) & 0xFFFFFFFFFFFFFFFFULL;
}

uint64_t SpeedEditorHID::rol8n(uint64_t value, int n) {
    for (int i = 0; i < n; i++) {
        value = rol8(value);
    }
    return value;
}

uint64_t SpeedEditorHID::calculateResponse(uint64_t challenge) {
    int n = challenge & 7;
    uint64_t v = rol8n(challenge, n);
    uint64_t k;

    if ((v & 1) == ((0x78 >> n) & 1)) {
        k = AUTH_EVEN[n];
    }
    else {
        v ^= rol8(v);
        k = AUTH_ODD[n];
    }

    return v ^ (rol8(v) & AUTH_MASK) ^ k;
}

// ── Constructor / Destructor ───────────────────────────────────────

SpeedEditorHID::SpeedEditorHID() {}

SpeedEditorHID::~SpeedEditorHID() {
    close();
}

// ── Low-level HID I/O ─────────────────────────────────────────────

void SpeedEditorHID::sendFeatureReport(const uint8_t* data, size_t len) {
    if (!dev) { return; }
    int res = hid_send_feature_report(dev, data, len);
    if (res < 0) {
        flog::error("Speed Editor: send_feature_report failed");
    }
}

int SpeedEditorHID::recvFeatureReport(uint8_t reportId, uint8_t* buf, size_t len) {
    if (!dev) { return -1; }
    buf[0] = reportId;
    int res = hid_get_feature_report(dev, buf, len);
    if (res < 0) {
        flog::error("Speed Editor: get_feature_report failed");
    }
    return res;
}

void SpeedEditorHID::writeOutputReport(const uint8_t* data, size_t len) {
    if (!dev) { return; }
    int res = hid_write(dev, data, len);
    if (res < 0) {
        flog::error("Speed Editor: write failed");
    }
}

// ── Open / Close ───────────────────────────────────────────────────

bool SpeedEditorHID::open(const std::string& serialFilter) {
    close();

    state = State::Disconnected;

    // Enumerate to find the device
    struct hid_device_info* devs = hid_enumerate(SPEED_EDITOR_VID, SPEED_EDITOR_PID);
    if (!devs) {
        return false;
    }

    struct hid_device_info* target = nullptr;
    struct hid_device_info* cur = devs;
    while (cur) {
        if (!serialFilter.empty() && cur->serial_number) {
            // Convert wide string serial to std::string for comparison
            std::string devSerial;
            for (const wchar_t* p = cur->serial_number; *p; p++) {
                devSerial += (char)*p;
            }
            if (devSerial == serialFilter) {
                target = cur;
                break;
            }
        }
        else {
            // Take the first one found
            target = cur;
            break;
        }
        cur = cur->next;
    }

    if (!target) {
        hid_free_enumeration(devs);
        return false;
    }

    dev = hid_open_path(target->path);
    if (!dev) {
        hid_free_enumeration(devs);
        return false;
    }

    // Store serial number
    if (target->serial_number) {
        serial.clear();
        for (const wchar_t* p = target->serial_number; *p; p++) {
            serial += (char)*p;
        }
    }

    hid_free_enumeration(devs);

    flog::info("Speed Editor: opened device (serial: {})", serial.empty() ? "none" : serial);
    return true;
}

void SpeedEditorHID::close() {
    if (dev) {
        allLedsOff();
        hid_close(dev);
        dev = nullptr;
    }
    state = State::Disconnected;
    serial.clear();
}

// ── Authentication ─────────────────────────────────────────────────

bool SpeedEditorHID::authenticate() {
    if (!dev) { return false; }

    state = State::Authenticating;
    flog::info("Speed Editor: authenticating...");

    // Step 1 — request challenge
    uint8_t req[10] = {0x06, 0x00};
    sendFeatureReport(req, sizeof(req));

    uint8_t challengeBuf[10] = {};
    if (recvFeatureReport(0x06, challengeBuf, sizeof(challengeBuf)) < 0) {
        state = State::Disconnected;
        return false;
    }

    // Step 2 — acknowledge
    uint8_t ack[10] = {0x06, 0x01};
    sendFeatureReport(ack, sizeof(ack));

    uint8_t ackResp[10] = {};
    recvFeatureReport(0x06, ackResp, sizeof(ackResp));

    // Step 3 — compute and send response
    uint64_t challenge;
    memcpy(&challenge, &challengeBuf[2], sizeof(uint64_t));
    uint64_t response = calculateResponse(challenge);

    uint8_t respReport[10] = {0x06, 0x03};
    memcpy(&respReport[2], &response, sizeof(uint64_t));
    sendFeatureReport(respReport, sizeof(respReport));

    // Step 4 — verify acceptance
    uint8_t result[10] = {};
    if (recvFeatureReport(0x06, result, sizeof(result)) < 0) {
        state = State::Disconnected;
        return false;
    }

    if (result[0] != 0x06 || result[1] != 0x04) {
        flog::error("Speed Editor: authentication FAILED");
        state = State::Disconnected;
        return false;
    }

    state = State::Connected;
    flog::info("Speed Editor: authentication OK");
    return true;
}

// ── Post-auth init ─────────────────────────────────────────────────

void SpeedEditorHID::sendInitReports() {
    if (!dev) { return; }

    // Report 0x03: set jog wheel mode to 0 (relative)
    uint8_t jogInit[] = {0x03, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
    writeOutputReport(jogInit, sizeof(jogInit));
}

// ── Event reading ──────────────────────────────────────────────────

bool SpeedEditorHID::readEvent(SpeedEditorEvent& event, int timeoutMs) {
    if (!dev) { return false; }

    uint8_t buf[64] = {};
    int res = hid_read_timeout(dev, buf, sizeof(buf), timeoutMs);
    if (res == 0) { return false; }  // timeout — no data
    if (res < 0) {
        // Read error — device likely disconnected
        flog::warn("Speed Editor: read error, closing device");
        close();
        return false;
    }

    uint8_t reportId = buf[0];

    if (reportId == 0x03 && res >= 6) {
        // Jog wheel: [0x03, mode, delta_i32_le]
        event.type = EventType::Wheel;
        event.wheel.mode = (WheelMode)buf[1];
        int32_t delta;
        memcpy(&delta, &buf[2], sizeof(int32_t));
        event.wheel.delta = delta;
        return true;
    }

    if (reportId == 0x04) {
        // Button report: up to 6 keycodes as uint16_le
        event.type = EventType::Button;
        event.button.keycodes.clear();
        for (int i = 0; i < 6; i++) {
            int offset = 1 + i * 2;
            if (offset + 1 < res) {
                uint16_t kc;
                memcpy(&kc, &buf[offset], sizeof(uint16_t));
                if (kc != 0) {
                    event.button.keycodes.push_back(kc);
                }
            }
        }
        return true;
    }

    event.type = EventType::Unknown;
    return true;
}

// ── LED control ────────────────────────────────────────────────────

void SpeedEditorHID::setLeds(uint8_t b0, uint8_t b1, uint8_t b2) {
    uint8_t report[] = {0x02, b0, b1, b2, 0x00};
    writeOutputReport(report, sizeof(report));
}

void SpeedEditorHID::setJogLeds(bool jog, bool shtl, bool scrl) {
    uint8_t val = (jog ? 1 : 0) | (shtl ? 2 : 0) | (scrl ? 4 : 0);
    uint8_t report[] = {0x04, val};
    writeOutputReport(report, sizeof(report));
}

void SpeedEditorHID::allLedsOff() {
    setLeds(0, 0, 0);
    setJogLeds(false, false, false);
}

// ── LED chase demo ─────────────────────────────────────────────────

void SpeedEditorHID::ledChaseDemo() {
    if (!dev) { return; }

    flog::info("Speed Editor: running LED chase demo...");

    // Walk through 18 main panel LEDs
    for (int i = 0; i < 18; i++) {
        int byteIdx = i / 8;
        int bit = i % 8;
        uint8_t bytes[3] = {0, 0, 0};
        bytes[byteIdx] = 1 << bit;
        setLeds(bytes[0], bytes[1], bytes[2]);
        setJogLeds(false, false, false);
        std::this_thread::sleep_for(std::chrono::milliseconds(120));
    }
    setLeds(0, 0, 0);

    // Walk through 3 jog LEDs
    for (int i = 0; i < 3; i++) {
        setJogLeds(i == 0, i == 1, i == 2);
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }

    allLedsOff();
    flog::info("Speed Editor: LED chase complete");
}
