#pragma once
#include <cstdint>
#include <map>
#include <string>

// Device identifiers
constexpr uint16_t SPEED_EDITOR_VID = 0x1EDB;  // Blackmagic Design
constexpr uint16_t SPEED_EDITOR_PID = 0xDA0E;  // Speed Editor

// Button keycodes
constexpr uint16_t KEY_SMART_INSRT = 0x0001;
constexpr uint16_t KEY_APPND       = 0x0002;
constexpr uint16_t KEY_RIPL_OWR    = 0x0003;
constexpr uint16_t KEY_CLOSE_UP    = 0x0004;
constexpr uint16_t KEY_PLACE_ON_TOP= 0x0005;
constexpr uint16_t KEY_SRC_OWR     = 0x0006;
constexpr uint16_t KEY_IN          = 0x0007;
constexpr uint16_t KEY_OUT         = 0x0008;
constexpr uint16_t KEY_TRIM_IN     = 0x0009;
constexpr uint16_t KEY_TRIM_OUT    = 0x000A;
constexpr uint16_t KEY_ROLL        = 0x000B;
constexpr uint16_t KEY_SLIP_SRC    = 0x000C;
constexpr uint16_t KEY_SLIP_DEST   = 0x000D;
constexpr uint16_t KEY_TRANS_DUR   = 0x000E;
constexpr uint16_t KEY_CUT         = 0x000F;
constexpr uint16_t KEY_DIS         = 0x0010;
constexpr uint16_t KEY_SMTH_CUT    = 0x0011;
constexpr uint16_t KEY_SOURCE      = 0x001A;
constexpr uint16_t KEY_TIMELINE    = 0x001B;
constexpr uint16_t KEY_SHTL        = 0x001C;
constexpr uint16_t KEY_JOG         = 0x001D;
constexpr uint16_t KEY_SCRL        = 0x001E;
constexpr uint16_t KEY_SYNC_BIN    = 0x001F;
constexpr uint16_t KEY_TRANS       = 0x0022;
constexpr uint16_t KEY_VIDEO_ONLY  = 0x0025;
constexpr uint16_t KEY_AUDIO_ONLY  = 0x0026;
constexpr uint16_t KEY_RIPL_DEL    = 0x002B;
constexpr uint16_t KEY_AUDIO_LEVEL = 0x002C;
constexpr uint16_t KEY_FULL_VIEW   = 0x002D;
constexpr uint16_t KEY_SNAP        = 0x002E;
constexpr uint16_t KEY_SPLIT       = 0x002F;
constexpr uint16_t KEY_LIVE_OWR    = 0x0030;
constexpr uint16_t KEY_ESC         = 0x0031;
constexpr uint16_t KEY_CAM1        = 0x0033;
constexpr uint16_t KEY_CAM2        = 0x0034;
constexpr uint16_t KEY_CAM3        = 0x0035;
constexpr uint16_t KEY_CAM4        = 0x0036;
constexpr uint16_t KEY_CAM5        = 0x0037;
constexpr uint16_t KEY_CAM6        = 0x0038;
constexpr uint16_t KEY_CAM7        = 0x0039;
constexpr uint16_t KEY_CAM8        = 0x003A;
constexpr uint16_t KEY_CAM9        = 0x003B;
constexpr uint16_t KEY_STOP_PLAY   = 0x003C;

// Button name lookup
inline const std::map<uint16_t, std::string>& getButtonNames() {
    static const std::map<uint16_t, std::string> names = {
        {KEY_SMART_INSRT, "SMART INSRT"},
        {KEY_APPND,       "APPND"},
        {KEY_RIPL_OWR,    "RIPL O/WR"},
        {KEY_CLOSE_UP,    "CLOSE UP"},
        {KEY_PLACE_ON_TOP,"PLACE ON TOP"},
        {KEY_SRC_OWR,     "SRC O/WR"},
        {KEY_IN,          "IN"},
        {KEY_OUT,         "OUT"},
        {KEY_TRIM_IN,     "TRIM IN"},
        {KEY_TRIM_OUT,    "TRIM OUT"},
        {KEY_ROLL,        "ROLL"},
        {KEY_SLIP_SRC,    "SLIP SRC"},
        {KEY_SLIP_DEST,   "SLIP DEST"},
        {KEY_TRANS_DUR,   "TRANS DUR"},
        {KEY_CUT,         "CUT"},
        {KEY_DIS,         "DIS"},
        {KEY_SMTH_CUT,    "SMTH CUT"},
        {KEY_SOURCE,      "SOURCE"},
        {KEY_TIMELINE,    "TIMELINE"},
        {KEY_SHTL,        "SHTL"},
        {KEY_JOG,         "JOG"},
        {KEY_SCRL,        "SCRL"},
        {KEY_SYNC_BIN,    "SYNC BIN"},
        {KEY_TRANS,       "TRANS"},
        {KEY_VIDEO_ONLY,  "VIDEO ONLY"},
        {KEY_AUDIO_ONLY,  "AUDIO ONLY"},
        {KEY_RIPL_DEL,    "RIPL DEL"},
        {KEY_AUDIO_LEVEL, "AUDIO LEVEL"},
        {KEY_FULL_VIEW,   "FULL VIEW"},
        {KEY_SNAP,        "SNAP"},
        {KEY_SPLIT,       "SPLIT"},
        {KEY_LIVE_OWR,    "LIVE O/WR"},
        {KEY_ESC,         "ESC"},
        {KEY_CAM1,        "CAM 1"},
        {KEY_CAM2,        "CAM 2"},
        {KEY_CAM3,        "CAM 3"},
        {KEY_CAM4,        "CAM 4"},
        {KEY_CAM5,        "CAM 5"},
        {KEY_CAM6,        "CAM 6"},
        {KEY_CAM7,        "CAM 7"},
        {KEY_CAM8,        "CAM 8"},
        {KEY_CAM9,        "CAM 9"},
        {KEY_STOP_PLAY,   "STOP/PLAY"},
    };
    return names;
}

// ── LED bit positions ──────────────────────────────────────────────
//
// Report 0x02 — 18 main panel LEDs as a flat bitmask across 3 bytes:
//
//   Byte 0 (bits 0–7):
//     0 CLOSE_UP    1 CUT        2 DIS        3 SMTH_CUT
//     4 TRANS       5 SNAP       6 CAM7       7 CAM8
//
//   Byte 1 (bits 8–15):
//     0 CAM9        1 LIVE_OWR   2 CAM4       3 CAM5
//     4 CAM6        5 VIDEO_ONLY 6 CAM1       7 CAM2
//
//   Byte 2 (bits 16–17):
//     0 CAM3        1 AUDIO_ONLY  (bits 2–7 unused)
//
// Report 0x04 — 3 jog mode LEDs in a single byte:
//     0 JOG         1 SHTL       2 SCRL

// Main panel LED bit indices (byte * 8 + bit)
constexpr int LED_CLOSE_UP   = 0;
constexpr int LED_CUT        = 1;
constexpr int LED_DIS        = 2;
constexpr int LED_SMTH_CUT   = 3;
constexpr int LED_TRANS      = 4;
constexpr int LED_SNAP       = 5;
constexpr int LED_CAM7       = 6;
constexpr int LED_CAM8       = 7;
constexpr int LED_CAM9       = 8;
constexpr int LED_LIVE_OWR   = 9;
constexpr int LED_CAM4       = 10;
constexpr int LED_CAM5       = 11;
constexpr int LED_CAM6       = 12;
constexpr int LED_VIDEO_ONLY = 13;
constexpr int LED_CAM1       = 14;
constexpr int LED_CAM2       = 15;
constexpr int LED_CAM3       = 16;
constexpr int LED_AUDIO_ONLY = 17;

// LED bit index for each CAM preset (1-indexed)
inline int ledForCam(int camNum) {
    static const int camLeds[] = {
        -1,         // 0 (unused)
        LED_CAM1,   // 1
        LED_CAM2,   // 2
        LED_CAM3,   // 3
        LED_CAM4,   // 4
        LED_CAM5,   // 5
        LED_CAM6,   // 6
        LED_CAM7,   // 7
        LED_CAM8,   // 8
        LED_CAM9,   // 9
    };
    if (camNum < 1 || camNum > 9) { return -1; }
    return camLeds[camNum];
}
