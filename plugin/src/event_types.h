#pragma once
#include <cstdint>
#include <vector>
#include <string>

enum class EventType {
    Wheel,
    Button,
    Unknown,
};

enum class WheelMode : uint8_t {
    JOG     = 0,
    SHUTTLE = 1,
    SCROLL  = 2,
};

struct WheelEvent {
    WheelMode mode;
    int32_t delta;
};

struct ButtonEvent {
    std::vector<uint16_t> keycodes;

    bool isRelease() const { return keycodes.empty(); }
};

struct SpeedEditorEvent {
    EventType type;
    WheelEvent wheel;
    ButtonEvent button;
};
