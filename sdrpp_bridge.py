#!/usr/bin/env python3
# /// script
# requires-python = ">=3.14"
# dependencies = ["hidapi", "pyyaml"]
# ///
"""
SDR++ ↔ Speed Editor Bridge
============================

Maps DaVinci Speed Editor inputs to SDR++ actions via the rigctl
TCP protocol.  Requires SDR++'s rigctl_server module to be enabled
(Module Manager → rigctl_server → start on the configured port).

Usage
-----
    uv run sdrpp_bridge.py                     # default config
    uv run sdrpp_bridge.py -c my_config.yaml   # custom config
    uv run sdrpp_bridge.py --dry-run            # no TCP, just print actions
"""

import argparse
import pathlib
import socket
import sys
import time
from dataclasses import dataclass, field

import yaml

from davinci_speed_editor import (
    ButtonEvent,
    DaVinciSpeedEditor,
    WheelEvent,
    scan_for_device,
)


# ── Rigctl TCP client ──────────────────────────────────────────────

class RigctlClient:
    """Minimal hamlib rigctl TCP client for SDR++."""

    def __init__(self, host: str = "127.0.0.1", port: int = 4532):
        self.host = host
        self.port = port
        self._sock: socket.socket | None = None

    @property
    def connected(self) -> bool:
        return self._sock is not None

    def connect(self) -> None:
        self.disconnect()
        s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        s.settimeout(2.0)
        s.connect((self.host, self.port))
        s.settimeout(1.0)
        self._sock = s

    def disconnect(self) -> None:
        if self._sock:
            try:
                self._sock.close()
            except OSError:
                pass
            self._sock = None

    def _send(self, cmd: str) -> str:
        """Send a rigctl command, return the response (may be multi-line)."""
        if not self._sock:
            raise ConnectionError("Not connected to SDR++")
        try:
            self._sock.sendall((cmd + "\n").encode())
            chunks: list[bytes] = []
            while True:
                try:
                    data = self._sock.recv(4096)
                except TimeoutError:
                    break
                if not data:
                    break
                chunks.append(data)
                # rigctl responses end with RPRT or a value line;
                # once we have data and the socket would block, we're done.
                if len(data) < 4096:
                    break
            return b"".join(chunks).decode(errors="replace").strip()
        except (BrokenPipeError, ConnectionResetError, OSError) as exc:
            self.disconnect()
            raise ConnectionError(f"Lost connection: {exc}") from exc

    # ── high-level commands ────────────────────────────────────────

    def get_freq(self) -> int:
        resp = self._send("f")
        return int(resp.splitlines()[0])

    def set_freq(self, hz: int) -> bool:
        resp = self._send(f"F {hz}")
        return "RPRT 0" in resp

    def get_mode(self) -> tuple[str, int]:
        resp = self._send("m")
        lines = resp.splitlines()
        mode = lines[0] if lines else "FM"
        passband = int(lines[1]) if len(lines) > 1 else 0
        return mode, passband

    def set_mode(self, mode: str, passband: int) -> bool:
        resp = self._send(f"M {mode} {passband}")
        return "RPRT 0" in resp


class DryRunRigctl:
    """Drop-in replacement that just prints what would be sent."""

    connected = True

    def connect(self) -> None:
        print("  [dry-run] rigctl: connect")

    def disconnect(self) -> None:
        pass

    def get_freq(self) -> int:
        return 100_000_000

    def set_freq(self, hz: int) -> bool:
        print(f"  [dry-run] rigctl: F {hz}")
        return True

    def get_mode(self) -> tuple[str, int]:
        return ("FM", 12500)

    def set_mode(self, mode: str, passband: int) -> bool:
        print(f"  [dry-run] rigctl: M {mode} {passband}")
        return True


# ── Configuration ──────────────────────────────────────────────────

@dataclass
class Preset:
    freq: int
    mode: str
    passband: int
    label: str = ""


@dataclass
class ModeEntry:
    name: str
    passband: int


@dataclass
class Config:
    host: str = "127.0.0.1"
    port: int = 4532
    jog_hz: int = 1_000
    shuttle_hz: int = 25_000
    scroll_hz: int = 100_000
    snap_grid_hz: int = 5_000
    scan_step_hz: int = 25_000
    scan_dwell_ms: int = 300
    modes: list[ModeEntry] = field(default_factory=list)
    presets: dict[int, Preset] = field(default_factory=dict)


def load_config(path: pathlib.Path | None) -> Config:
    cfg = Config()

    if path and path.exists():
        raw = yaml.safe_load(path.read_text()) or {}
    else:
        raw = {}

    sdrpp = raw.get("sdrpp", {})
    cfg.host = sdrpp.get("host", cfg.host)
    cfg.port = sdrpp.get("port", cfg.port)

    tuning = raw.get("tuning", {})
    cfg.jog_hz = tuning.get("jog_hz", cfg.jog_hz)
    cfg.shuttle_hz = tuning.get("shuttle_hz", cfg.shuttle_hz)
    cfg.scroll_hz = tuning.get("scroll_hz", cfg.scroll_hz)
    cfg.snap_grid_hz = tuning.get("snap_grid_hz", cfg.snap_grid_hz)

    scan = raw.get("scan", {})
    cfg.scan_step_hz = scan.get("step_hz", cfg.scan_step_hz)
    cfg.scan_dwell_ms = scan.get("dwell_ms", cfg.scan_dwell_ms)

    for entry in raw.get("modes", []):
        cfg.modes.append(ModeEntry(entry["name"], entry["passband"]))
    if not cfg.modes:
        cfg.modes = [
            ModeEntry("WFM", 200000), ModeEntry("FM", 12500),
            ModeEntry("AM", 10000), ModeEntry("USB", 3000),
            ModeEntry("LSB", 3000), ModeEntry("CW", 500),
        ]

    for key, val in raw.get("presets", {}).items():
        cfg.presets[int(key)] = Preset(
            freq=val["freq"], mode=val["mode"],
            passband=val["passband"], label=val.get("label", ""),
        )

    return cfg


# ── LED mapping ────────────────────────────────────────────────────
#
# The Speed Editor has ~18 LEDs controlled by 4 bitmask bytes in
# report 0x02.  The mapping below is a best guess — run the driver
# with --demo to chase each bit and fill in the correct positions
# for your unit.
#
# Format:  name → (byte_index 0–3, bit 0–7)

LED_MAP: dict[str, tuple[int, int]] = {
    # Byte 0 — camera / source row (guesses — calibrate with --demo)
    "CAM1":       (0, 0),
    "CAM2":       (0, 1),
    "CAM3":       (0, 2),
    "CAM4":       (0, 3),
    "CAM5":       (0, 4),
    "CAM6":       (0, 5),
    "CAM7":       (0, 6),
    "CAM8":       (0, 7),
    # Byte 1
    "CAM9":       (1, 0),
    "LIVE_OWR":   (1, 1),
    "CUT":        (1, 2),
    "DIS":        (1, 3),
    "SMTH_CUT":   (1, 4),
    "TRANS":      (1, 5),
    # Byte 2 — transport / mode row
    "PLAY_REV":   (2, 0),
    "STOP":       (2, 1),
    "PLAY_FWD":   (2, 2),
    "SHTL":       (2, 3),
    "JOG":        (2, 4),
    "SCRL":       (2, 5),
    # Byte 3 — bottom row
    "AUDIO_ONLY": (2, 6),
    "SNAP":       (2, 7),
}


def leds_from_names(names: set[str]) -> tuple[int, int, int, int]:
    """Convert a set of LED names to the 4-byte bitmask."""
    b = [0, 0, 0, 0]
    for name in names:
        pos = LED_MAP.get(name)
        if pos:
            byte_idx, bit = pos
            b[byte_idx] |= 1 << bit
    return b[0], b[1], b[2], b[3]


# ── Bridge state ──────────────────────────────────────────────────

WHEEL_JOG     = 0
WHEEL_SHUTTLE = 1
WHEEL_SCROLL  = 2

SCAN_OFF = 0
SCAN_FWD = 1
SCAN_REV = -1

# Keycode constants for buttons we handle
KC_CAM1     = 0x0001
KC_CAM9     = 0x0009
KC_CLOSE_UP = 0x000A
KC_CUT      = 0x000B
KC_DIS      = 0x000C
KC_SMTH_CUT = 0x000D
KC_TRANS    = 0x000E
KC_SNAP     = 0x000F
KC_LIVE_OWR = 0x0010
KC_PLAY_REV = 0x0011
KC_STOP     = 0x0012
KC_PLAY_FWD = 0x0013
KC_SHTL     = 0x0020
KC_JOG      = 0x0021
KC_SCRL     = 0x0022
KC_TRIM_IN  = 0x0037
KC_TRIM_OUT = 0x0038
KC_ESC      = 0x0034


@dataclass
class BridgeState:
    freq: int = 100_000_000
    mode: str = "FM"
    passband: int = 12500
    mode_index: int = 1          # index into config.modes
    wheel_mode: int = WHEEL_JOG
    active_preset: int = 0       # 0 = none, 1–9 = CAM key
    prev_freq: int = 100_000_000 # for ESC recall
    scan_dir: int = SCAN_OFF
    scan_next_time: float = 0.0
    prev_keycodes: set[int] = field(default_factory=set)


# ── Bridge logic ──────────────────────────────────────────────────

class Bridge:
    """Main bridge: reads Speed Editor, talks to SDR++ via rigctl."""

    def __init__(
        self,
        editor: DaVinciSpeedEditor,
        rigctl: RigctlClient | DryRunRigctl,
        config: Config,
    ):
        self.editor = editor
        self.rigctl = rigctl
        self.cfg = config
        self.state = BridgeState()

    # ── sync current frequency from SDR++ ─────────────────────────

    def sync_from_sdrpp(self) -> None:
        """Pull current freq/mode from SDR++ into our state."""
        try:
            self.state.freq = self.rigctl.get_freq()
            self.state.mode, self.state.passband = self.rigctl.get_mode()
            # find matching mode index
            for i, m in enumerate(self.cfg.modes):
                if m.name == self.state.mode:
                    self.state.mode_index = i
                    break
        except ConnectionError:
            pass
        self._log(f"Synced: {self._fmt_freq(self.state.freq)}  "
                  f"{self.state.mode}/{self.state.passband}")

    # ── event dispatch ────────────────────────────────────────────

    def handle_event(self, event: WheelEvent | ButtonEvent) -> None:
        if isinstance(event, WheelEvent):
            self._on_wheel(event)
        elif isinstance(event, ButtonEvent):
            self._on_buttons(event)

    def _on_wheel(self, ev: WheelEvent) -> None:
        # Wheel always tunes — step size depends on current wheel mode
        step = {
            WHEEL_JOG:     self.cfg.jog_hz,
            WHEEL_SHUTTLE: self.cfg.shuttle_hz,
            WHEEL_SCROLL:  self.cfg.scroll_hz,
        }.get(self.state.wheel_mode, self.cfg.jog_hz)

        new_freq = max(0, self.state.freq + ev.delta * step)
        self._tune(new_freq)

    def _on_buttons(self, ev: ButtonEvent) -> None:
        current = set(ev.keycodes)
        pressed = current - self.state.prev_keycodes   # newly pressed
        self.state.prev_keycodes = current

        for kc in pressed:
            self._on_key_down(kc)

    def _on_key_down(self, kc: int) -> None:
        # ── CAM 1–9: preset recall ────────────────────────────────
        if KC_CAM1 <= kc <= KC_CAM9:
            cam = kc - KC_CAM1 + 1
            preset = self.cfg.presets.get(cam)
            if preset:
                self.state.prev_freq = self.state.freq
                self._tune(preset.freq)
                self._set_mode(preset.mode, preset.passband)
                self.state.active_preset = cam
                self._log(f"Preset {cam}: {preset.label}  "
                          f"{self._fmt_freq(preset.freq)} {preset.mode}")
            else:
                self._log(f"Preset {cam}: not configured")
            return

        # ── Wheel mode selection ──────────────────────────────────
        if kc == KC_JOG:
            self.state.wheel_mode = WHEEL_JOG
            self._log("Wheel → JOG  (fine)")
        elif kc == KC_SHTL:
            self.state.wheel_mode = WHEEL_SHUTTLE
            self._log("Wheel → SHUTTLE  (coarse)")
        elif kc == KC_SCRL:
            self.state.wheel_mode = WHEEL_SCROLL
            self._log("Wheel → SCROLL  (band)")

        # ── TRANS: cycle demod mode ───────────────────────────────
        elif kc == KC_TRANS:
            self.state.mode_index = (
                (self.state.mode_index + 1) % len(self.cfg.modes)
            )
            m = self.cfg.modes[self.state.mode_index]
            self._set_mode(m.name, m.passband)
            self._log(f"Mode → {m.name}/{m.passband}")

        # ── SNAP: round to grid ───────────────────────────────────
        elif kc == KC_SNAP:
            grid = self.cfg.snap_grid_hz
            snapped = round(self.state.freq / grid) * grid
            self._tune(snapped)
            self._log(f"Snap → {self._fmt_freq(snapped)}")

        # ── ESC: recall previous frequency ────────────────────────
        elif kc == KC_ESC:
            old = self.state.freq
            self._tune(self.state.prev_freq)
            self.state.prev_freq = old
            self._log(f"Recall → {self._fmt_freq(self.state.freq)}")

        # ── TRIM IN / TRIM OUT: adjust passband ──────────────────
        elif kc == KC_TRIM_IN:
            new_pb = max(100, self.state.passband - 500)
            self._set_mode(self.state.mode, new_pb)
            self._log(f"Passband → {new_pb} Hz")
        elif kc == KC_TRIM_OUT:
            new_pb = self.state.passband + 500
            self._set_mode(self.state.mode, new_pb)
            self._log(f"Passband → {new_pb} Hz")

        # ── PLAY FWD / REV: start scan ───────────────────────────
        elif kc == KC_PLAY_FWD:
            self.state.scan_dir = SCAN_FWD
            self.state.scan_next_time = time.monotonic()
            self._log("Scan ▶ forward")
        elif kc == KC_PLAY_REV:
            self.state.scan_dir = SCAN_REV
            self.state.scan_next_time = time.monotonic()
            self._log("Scan ◀ reverse")

        # ── STOP: halt scan ──────────────────────────────────────
        elif kc == KC_STOP:
            if self.state.scan_dir != SCAN_OFF:
                self.state.scan_dir = SCAN_OFF
                self._log("Scan ⏹ stopped")

    # ── scanning ──────────────────────────────────────────────────

    def tick_scan(self) -> None:
        """Call from the main loop.  Advances scan if due."""
        if self.state.scan_dir == SCAN_OFF:
            return
        now = time.monotonic()
        if now < self.state.scan_next_time:
            return
        step = self.cfg.scan_step_hz * self.state.scan_dir
        new_freq = max(0, self.state.freq + step)
        self._tune(new_freq)
        self.state.scan_next_time = now + self.cfg.scan_dwell_ms / 1000.0

    # ── LED update ────────────────────────────────────────────────

    def update_leds(self) -> None:
        """Set Speed Editor LEDs to reflect current state."""
        lit: set[str] = set()

        # Active preset
        if 1 <= self.state.active_preset <= 9:
            lit.add(f"CAM{self.state.active_preset}")

        # Wheel mode
        lit.add({
            WHEEL_JOG: "JOG", WHEEL_SHUTTLE: "SHTL", WHEEL_SCROLL: "SCRL",
        }.get(self.state.wheel_mode, "JOG"))

        # Scan direction
        if self.state.scan_dir == SCAN_FWD:
            lit.add("PLAY_FWD")
        elif self.state.scan_dir == SCAN_REV:
            lit.add("PLAY_REV")

        b1, b2, b3, b4 = leds_from_names(lit)
        self.editor.set_leds(b1, b2, b3, b4)

    # ── helpers ───────────────────────────────────────────────────

    def _tune(self, hz: int) -> None:
        self.state.prev_freq = self.state.freq
        self.state.freq = hz
        self.state.active_preset = 0  # manual tune clears preset
        try:
            self.rigctl.set_freq(hz)
        except ConnectionError as exc:
            self._log(f"⚠ rigctl: {exc}")

    def _set_mode(self, mode: str, passband: int) -> None:
        self.state.mode = mode
        self.state.passband = passband
        try:
            self.rigctl.set_mode(mode, passband)
        except ConnectionError as exc:
            self._log(f"⚠ rigctl: {exc}")

    @staticmethod
    def _fmt_freq(hz: int) -> str:
        if hz >= 1_000_000:
            return f"{hz / 1_000_000:.3f} MHz"
        if hz >= 1_000:
            return f"{hz / 1_000:.1f} kHz"
        return f"{hz} Hz"

    @staticmethod
    def _log(msg: str) -> None:
        print(f"  » {msg}")


# ── Main ──────────────────────────────────────────────────────────

def main() -> None:
    parser = argparse.ArgumentParser(
        description="SDR++ ↔ Speed Editor bridge"
    )
    parser.add_argument(
        "-c", "--config", type=pathlib.Path, default=pathlib.Path("config.yaml"),
        help="Path to YAML config file (default: config.yaml)",
    )
    parser.add_argument(
        "--dry-run", action="store_true",
        help="Don't connect to SDR++; print actions instead.",
    )
    parser.add_argument(
        "--quiet", action="store_true",
        help="Suppress HID-level TX/RX logging.",
    )
    args = parser.parse_args()

    cfg = load_config(args.config)

    print("=" * 58)
    print("  SDR++ ↔ Speed Editor Bridge")
    print("=" * 58)
    print()

    # ── Speed Editor ──
    scan_for_device()
    editor = DaVinciSpeedEditor(verbose=not args.quiet)

    # ── Rigctl ──
    if args.dry_run:
        rigctl = DryRunRigctl()
    else:
        rigctl = RigctlClient(cfg.host, cfg.port)

    bridge = Bridge(editor, rigctl, cfg)

    try:
        editor.open()
        editor.authenticate()
        editor.init_reports()

        if not args.dry_run:
            print(f"\nConnecting to SDR++ rigctl at {cfg.host}:{cfg.port} …")
            rigctl.connect()
            print("Connected ✓")

        bridge.sync_from_sdrpp()
        bridge.update_leds()

        print()
        print("Controls:")
        print("  Jog wheel      Tune frequency (step depends on mode)")
        print("  JOG/SHTL/SCRL  Switch step: "
              f"{cfg.jog_hz}/{cfg.shuttle_hz}/{cfg.scroll_hz} Hz")
        print("  CAM 1–9        Recall preset frequency")
        print("  TRANS           Cycle demod mode")
        print("  SNAP            Round frequency to grid")
        print("  TRIM IN/OUT     Adjust passband ±500 Hz")
        print("  PLAY ▶/◀        Scan forward/reverse")
        print("  STOP            Stop scanning")
        print("  ESC             Recall previous frequency")
        print()
        print("Listening — press Ctrl-C to quit.\n")

        led_update_counter = 0

        while True:
            event = editor.read_event(timeout_ms=50)
            if event is not None:
                bridge.handle_event(event)

            bridge.tick_scan()

            # Update LEDs every ~200ms (every 4th loop iteration)
            led_update_counter += 1
            if led_update_counter >= 4:
                led_update_counter = 0
                bridge.update_leds()

    except KeyboardInterrupt:
        print("\nShutting down.")
    except OSError as exc:
        sys.exit(f"HID error: {exc}")
    except ConnectionError as exc:
        sys.exit(f"rigctl error: {exc}")
    finally:
        if editor._is_open:
            try:
                editor.all_leds_off()
            except Exception:
                pass
        editor.close()
        if isinstance(rigctl, RigctlClient):
            rigctl.disconnect()


if __name__ == "__main__":
    main()
