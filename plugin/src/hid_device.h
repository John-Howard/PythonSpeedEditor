#pragma once
#include <cstdint>
#include <string>
#include <atomic>
#include <hidapi/hidapi.h>
#include "event_types.h"

class SpeedEditorHID {
public:
    enum class State {
        Disconnected,
        Authenticating,
        Connected,
    };

    SpeedEditorHID();
    ~SpeedEditorHID();

    // Open device by VID/PID (optionally filter by serial number).
    // Returns true on success.
    bool open(const std::string& serialFilter = "");
    void close();

    // Run the challenge-response authentication handshake.
    // Returns true on success.
    bool authenticate();

    // Send the post-auth initialization reports.
    void sendInitReports();

    // Read one event with timeout. Returns false if no event (timeout).
    bool readEvent(SpeedEditorEvent& event, int timeoutMs = 100);

    // LED control
    void setLeds(uint8_t b0, uint8_t b1, uint8_t b2);
    void setJogLeds(bool jog, bool shtl, bool scrl);
    void allLedsOff();

    // LED chase demo — walks each LED sequentially for visual confirmation
    void ledChaseDemo();

    State getState() const { return state; }
    std::string getSerial() const { return serial; }

private:
    // Authentication helpers
    static uint64_t rol8(uint64_t value);
    static uint64_t rol8n(uint64_t value, int n);
    static uint64_t calculateResponse(uint64_t challenge);

    void sendFeatureReport(const uint8_t* data, size_t len);
    int recvFeatureReport(uint8_t reportId, uint8_t* buf, size_t len);
    void writeOutputReport(const uint8_t* data, size_t len);

    hid_device* dev = nullptr;
    State state = State::Disconnected;
    std::string serial;
};
