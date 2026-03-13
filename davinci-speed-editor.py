#!/usr/bin/env python3

# /// script

# requires-python = “>=3.14”

# dependencies = [“hidapi”]

# ///

# “””
DaVinci Speed Editor — Python HID wrapper

A Python translation of the C# project
Tractus.Hid.DaVinciSpeedEditor, which itself was derived from:

https://github.com/smunaut/blackmagic-misc
https://github.com/davidgiven/bmdkey
https://github.com/Haavard15/SpeedEditorHID/tree/test

The Speed Editor enumerates as a standard USB HID device but requires
a proprietary challenge-response authentication before it will send
input reports or accept LED-control commands.

## Usage

```
uv run davinci_speed_editor.py            # normal interactive mode
uv run davinci_speed_editor.py --demo     # LED chase demo then read inputs
uv run davinci_speed_editor.py --scan     # just scan for the device
```

“””

import argparse
import struct
import sys
import time

try:
import hid  # cython-hidapi  (provided by ‘hidapi’ on PyPI)
except ImportError:
sys.exit(
“ERROR: ‘hid’ module not found.\n”
“Run this script with:  uv run davinci_speed_editor.py”
)

# ── Device identifiers ──────────────────────────────────────────────

VENDOR_ID  = 0x1EDB   # Blackmagic Design
PRODUCT_ID = 0xDA0E   # Speed Editor

# ── Authentication tables (from smunaut’s reverse-engineering) ──────

AUTH_EVEN = [
0x3AE1206F97C10BC8, 0x2A9AB32BEBF244C6,
0x20A6F8B8DF9ADF0A, 0xAF80ECE52CFC1719,
0xEC2EE2F7414FD151, 0xB055ADFD73344A15,
0xA63D2E3059001187, 0x751BF623F42E0DDE,
]
AUTH_ODD = [
0x3E22B34F502E7FDE, 0x24656B981875AB1C,
0xA17F3456DF7BF8C3, 0x6DF72E1941AEF698,
0x72226F011E66AB94, 0x3831A3C606296B42,
0xFD7FF81881332C89, 0x61A3F6474FF236C6,
]
AUTH_MASK = 0xA79A63F585D37BF0

# ── Button keycode → name mapping ──────────────────────────────────

# Keycodes observed via the 0x04 report.  Extend as you discover more.

BUTTON_NAMES: dict[int, str] = {
0x0001: “CAM 1”,
0x0002: “CAM 2”,
0x0003: “CAM 3”,
0x0004: “CAM 4”,
0x0005: “CAM 5”,
0x0006: “CAM 6”,
0x0007: “CAM 7”,
0x0008: “CAM 8”,
0x0009: “CAM 9”,
0x000A: “CLOSE UP”,
0x000B: “CUT”,
0x000C: “DIS”,
0x000D: “SMTH CUT”,
0x000E: “TRANS”,           # Transition
0x000F: “SNAP”,
0x0010: “LIVE O/WR”,      # Live Overwrite
0x0011: “PLAY REV”,       # Play Reverse
0x0012: “STOP”,
0x0013: “PLAY FWD”,       # Play Forward
0x0020: “SHTL”,           # Shuttle
0x0021: “JOG”,
0x0022: “SCRL”,           # Scroll
0x0030: “AUDIO LEVEL”,
0x0031: “FULL VIEW”,
0x0032: “TRANS”,           # Transition (dup addr varies by FW)
0x0033: “SPLIT”,
0x0034: “ESC”,
0x0035: “SYNC BIN”,
0x0036: “RIP”,
0x0037: “TRIM IN”,
0x0038: “TRIM OUT”,
0x0039: “ROLL”,
0x003A: “SLIP SRC”,
0x003B: “SLIP DEST”,
0x003C: “SOURCE”,
0x003D: “TIMELINE”,
}

# ── Utility helpers ─────────────────────────────────────────────────

def _rol8(value: int) -> int:
“”“Rotate a 64-bit value left by 8 bits.”””
return ((value << 56) | (value >> 8)) & 0xFFFF_FFFF_FFFF_FFFF

def _rol8n(value: int, n: int) -> int:
“”“Rotate a 64-bit value left by 8 bits, *n* times.”””
for _ in range(n):
value = _rol8(value)
return value

def _calculate_response(challenge: int) -> int:
“”“Compute the authentication response for a given challenge.”””
n = challenge & 7
v = _rol8n(challenge, n)

```
if (v & 1) == ((0x78 >> n) & 1):
    k = AUTH_EVEN[n]
else:
    v ^= _rol8(v)
    k = AUTH_ODD[n]

return v ^ (_rol8(v) & AUTH_MASK) ^ k
```

def _unpack_u64(data: bytes, offset: int = 0) -> int:
return struct.unpack_from(”<Q”, data, offset)[0]

def _pack_u64(value: int) -> bytes:
return struct.pack(”<Q”, value)

def _hex(data: bytes) -> str:
return “ “.join(f”{b:02X}” for b in data)

# ── Main wrapper class ──────────────────────────────────────────────

class DaVinciSpeedEditor:
“”“High-level wrapper for the Blackmagic DaVinci Speed Editor.”””

```
def __init__(self, verbose: bool = True):
    self.verbose = verbose
    self._dev: hid.device | None = None
    self._is_open = False

# ── connection ──────────────────────────────────────────────────

def open(self) -> None:
    """Open the HID device."""
    self._dev = hid.device()
    self._dev.open(VENDOR_ID, PRODUCT_ID)
    self._is_open = True
    if self.verbose:
        mfr = self._dev.get_manufacturer_string() or "?"
        prod = self._dev.get_product_string() or "?"
        ser = self._dev.get_serial_number_string() or "?"
        print(f"Opened  : {mfr} — {prod}")
        print(f"Serial  : {ser}")

def close(self) -> None:
    if self._dev and self._is_open:
        try:
            self._dev.close()
        except Exception:
            pass
        self._is_open = False
    self._dev = None

# ── low-level HID helpers ──────────────────────────────────────

def _send_feature(self, data: bytes) -> None:
    assert self._dev is not None and self._is_open
    if self.verbose:
        print(f"  TX feature : [{_hex(data)}]")
    self._dev.send_feature_report(data)

def _recv_feature(self, report_id: int, length: int) -> bytes:
    assert self._dev is not None and self._is_open
    buf = bytes(self._dev.get_feature_report(report_id, length))
    if self.verbose:
        print(f"  RX feature : [{_hex(buf)}]")
    return buf

def _write(self, data: bytes) -> None:
    assert self._dev is not None and self._is_open
    if self.verbose:
        print(f"  TX output  : [{_hex(data)}]")
    self._dev.write(data)

def _read(self, length: int = 64, timeout_ms: int = 0) -> bytes:
    """Read an input report.  *timeout_ms* 0 = blocking."""
    assert self._dev is not None and self._is_open
    data = self._dev.read(length, timeout_ms)
    return bytes(data) if data else b""

# ── authentication ─────────────────────────────────────────────

def authenticate(self) -> None:
    """
    Run the challenge-response handshake that unlocks input
    reports and LED control on the Speed Editor.
    """
    print("Authenticating …")

    # Step 1 – request challenge
    self._send_feature(bytes([0x06, 0x00, 0x00, 0x00, 0x00,
                               0x00, 0x00, 0x00, 0x00, 0x00]))
    challenge_report = self._recv_feature(0x06, 10)

    # Step 2 – "acknowledge" (purpose not fully understood)
    self._send_feature(bytes([0x06, 0x01, 0x00, 0x00, 0x00,
                               0x00, 0x00, 0x00, 0x00, 0x00]))
    self._recv_feature(0x06, 10)

    # Step 3 – compute and send response
    challenge_value = _unpack_u64(challenge_report, 2)
    response_value  = _calculate_response(challenge_value)
    response_report = bytes([0x06, 0x03]) + _pack_u64(response_value)
    self._send_feature(response_report)

    # Step 4 – verify acceptance
    result = self._recv_feature(0x06, 10)
    if result[0] != 0x06 or result[1] != 0x04:
        raise RuntimeError("Authentication FAILED – device rejected response.")
    print("Authentication OK ✓")

# ── LED control ────────────────────────────────────────────────

def set_leds(self, b1: int = 0x00, b2: int = 0x00,
             b3: int = 0x00, b4: int = 0x00) -> None:
    """
    Set LED state.  The Speed Editor has ~18 LEDs mapped across
    four bytes (report id 0x02).  Each bit toggles one LED.

    Parameters are raw bitmask bytes; pass 0xFF to turn everything on.
    """
    self._write(bytes([0x02, b1 & 0xFF, b2 & 0xFF,
                       b3 & 0xFF, b4 & 0xFF]))

def all_leds_on(self) -> None:
    self.set_leds(0xFF, 0xFF, 0xFF, 0xFF)

def all_leds_off(self) -> None:
    self.set_leds(0x00, 0x00, 0x00, 0x00)

# ── initialization reports ─────────────────────────────────────

def init_reports(self) -> None:
    """Send the post-auth initialisation reports seen in the C# code."""
    self._write(bytes([0x03, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00]))
    self._write(bytes([0x04, 0xFF]))

# ── input processing ───────────────────────────────────────────

def process_packet(self, data: bytes) -> None:
    """Decode and print a single input report from the device."""
    if not data:
        return

    report_id = data[0]

    if report_id == 0x03:
        # Jog wheel / shuttle encoder
        if len(data) >= 6:
            delta = struct.unpack_from("<i", data, 2)[0]
            mode_byte = data[1]
            mode_label = {0: "JOG", 1: "SHUTTLE", 2: "SCROLL"}.get(
                mode_byte, f"MODE-{mode_byte:#04x}"
            )
            print(f"  ⟳  Wheel  {mode_label:>8s}  delta={delta:+d}")
        else:
            print(f"  ⟳  Wheel  (short packet: {_hex(data)})")

    elif report_id == 0x04:
        # Button presses — up to 6 simultaneous keycodes
        keys_down: list[str] = []
        for i in range(6):
            offset = 1 + i * 2
            if offset + 1 < len(data):
                kc = struct.unpack_from("<H", data, offset)[0]
                if kc != 0:
                    name = BUTTON_NAMES.get(kc, f"0x{kc:04X}")
                    keys_down.append(name)
        if keys_down:
            print(f"  ●  Button  {', '.join(keys_down)}")
        else:
            print("  ○  Button  (all released)")

    else:
        print(f"  ?  Unknown report 0x{report_id:02X}: {_hex(data)}")

# ── high-level loops ───────────────────────────────────────────

def read_loop(self) -> None:
    """Block forever, printing decoded input reports."""
    print("\nListening for input — press Ctrl-C to quit.\n")
    try:
        while True:
            data = self._read(64)
            if data:
                self.process_packet(data)
    except KeyboardInterrupt:
        print("\nStopped.")

def led_chase_demo(self) -> None:
    """
    Walk each bit across the four LED-control bytes so you can
    see which physical LED maps to which bit position.
    """
    print("\nLED chase demo — watch the panel …\n")
    for byte_index in range(4):
        for bit in range(8):
            vals = [0, 0, 0, 0]
            vals[byte_index] = 1 << bit
            label = f"byte[{byte_index + 1}] bit {bit}"
            print(f"  LED  {label:18s}  → {vals}")
            self.set_leds(*vals)
            time.sleep(0.25)
    self.all_leds_off()
    print("  LED chase complete.\n")
```

# ── Scanning helper (no open required) ─────────────────────────────

def scan_for_device() -> bool:
“”“Print info about every matching Speed Editor on the bus.”””
found = False
for info in hid.enumerate(VENDOR_ID, PRODUCT_ID):
found = True
print(“DaVinci Speed Editor detected:”)
for key in (“manufacturer_string”, “product_string”,
“serial_number”, “path”, “release_number”,
“interface_number”, “usage_page”, “usage”):
val = info.get(key, “—”)
print(f”  {key:24s}: {val}”)
print()
if not found:
print(“No DaVinci Speed Editor found on the USB bus.”)
return found

# ── CLI entry point ─────────────────────────────────────────────────

def main() -> None:
parser = argparse.ArgumentParser(
description=“DaVinci Speed Editor — Python HID test wrapper”
)
parser.add_argument(
“–scan”, action=“store_true”,
help=“Just scan for the device and exit.”
)
parser.add_argument(
“–demo”, action=“store_true”,
help=“Run an LED chase demo before entering the read loop.”
)
parser.add_argument(
“–quiet”, action=“store_true”,
help=“Suppress low-level TX/RX logging.”
)
args = parser.parse_args()

```
# ── scan only ──
if args.scan:
    scan_for_device()
    return

# ── full interactive run ──
editor = DaVinciSpeedEditor(verbose=not args.quiet)

try:
    print("=" * 58)
    print("  DaVinci Speed Editor — Python HID Test Wrapper")
    print("=" * 58)
    print()

    scan_for_device()

    editor.open()
    editor.authenticate()
    editor.init_reports()

    if args.demo:
        editor.led_chase_demo()

    # Turn all LEDs on so the user gets visual confirmation
    editor.all_leds_on()

    editor.read_loop()

except OSError as exc:
    sys.exit(f"HID error: {exc}")
except RuntimeError as exc:
    sys.exit(f"Error: {exc}")
finally:
    if editor._is_open:
        try:
            editor.all_leds_off()
        except Exception:
            pass
    editor.close()
```

if **name** == “**main**”:
main()
