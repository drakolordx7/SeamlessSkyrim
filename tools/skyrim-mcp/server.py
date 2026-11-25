"""MCP server that lets an agent run and drive Skyrim for SeamlessSkyrim development.

Talks newline-delimited JSON to the bridge inside the SeamlessSkyrim SKSE plugin
(src/Bridge, 127.0.0.1:47800). Things the plugin can't do from inside the game -- launching,
killing, reading crash logs, OS-level key presses -- live here.
"""

from __future__ import annotations

import ctypes
import io
import json
import math
import os
import socket
import subprocess
import sys
import tempfile
import threading
import time
from ctypes import wintypes
from pathlib import Path
from typing import Any

import psutil
from mcp.server.fastmcp import FastMCP, Image
import comtypes
from PIL import Image as PILImage
from pyvda import AppView, VirtualDesktop, get_virtual_desktops

import retools

# ---- configuration ------------------------------------------------------------------------
#
# Defaults are the MO2 portable instance of the Skyrim modding project (D:\AI\Active\Skyrim\instance).
# Override with environment variables, or with a harness.json next to this file (not in git:
# {"game_dir": "...", "mo2_dir": ...}; keys are the lower-case names below without SKYRIM_);
# the environment wins over the file. For the original Vortex/Steam setup:
#   SKYRIM_LAUNCH=skse SKYRIM_GAME_DIR="E:\SteamLibrary\steamapps\common\Skyrim Special Edition"
#   SKYRIM_SAVES_DIR="" (empty: derived from Skyrim.ini's sLocalSavePath in SKYRIM_DOCS_DIR)
#
#   SKYRIM_LAUNCH       "mo2" (default: ModOrganizer.exe moshortcut://:<SKYRIM_MO2_EXE>, so the mods
#                       are in the game's virtual file system) or "skse" (skse64_loader.exe directly)
#   SKYRIM_MO2_DIR      the portable MO2 folder holding ModOrganizer.exe
#   SKYRIM_MO2_PROFILE  MO2 profile (default "SeamlessTest": a copy of the user's Default that the Skyrim
#                       project's build.py keeps in sync; own saves folder, autosaves off, so test runs
#                       never touch the user's saves. game_launch refuses a profile without LocalSaves)
#   SKYRIM_BASE_SAVE    the clean new-game save routes start from (default "SeamlessTestBase", make_base_save)
#   SKYRIM_MO2_EXE      the executable's title in MO2 (default "SKSE")
#   SKYRIM_GAME_DIR     the game folder: SkyrimSE.exe, for the disassembler (default: MO2's Stock Game)
#   SKYRIM_DOCS_DIR     Documents\My Games\Skyrim Special Edition (SKSE and plugin logs go there)
#   SKYRIM_SAVES_DIR    the folder load_game/list_saves read (default: the MO2 profile's saves)


def _settings() -> dict[str, str]:
    file = Path(__file__).resolve().parent / "harness.json"
    values = {k.lower(): str(v) for k, v in json.loads(file.read_text("utf-8")).items()} if file.exists() else {}
    values.update({k[len("SKYRIM_"):].lower(): v for k, v in os.environ.items() if k.startswith("SKYRIM_")})
    return values


_cfg = _settings()
MO2_DIR = Path(_cfg.get("mo2_dir", r"D:\AI\Active\Skyrim\instance"))
MO2_PROFILE = _cfg.get("mo2_profile", "SeamlessTest")
BASE_SAVE = _cfg.get("base_save", "SeamlessTestBase")
MO2_EXE_TITLE = _cfg.get("mo2_exe", "SKSE")
LAUNCH_MODE = _cfg.get("launch", "mo2")
GAME_DIR = Path(_cfg.get("game_dir", str(MO2_DIR / "Stock Game")))
DOCS_DIR = Path(_cfg.get("docs_dir", str(Path.home() / "Documents" / "My Games" / "Skyrim Special Edition")))
SAVES_DIR = Path(_cfg["saves_dir"]) if _cfg.get("saves_dir") else (MO2_DIR / "profiles" / MO2_PROFILE / "saves" if LAUNCH_MODE == "mo2" else None)
SKSE_LOG_DIR = DOCS_DIR / "SKSE"
PLUGIN_LOG = SKSE_LOG_DIR / "SeamlessSkyrim.log"
BRIDGE_ADDR = ("127.0.0.1", 47800)
GAME_EXE = "SkyrimSE.exe"

mcp = FastMCP("skyrim")


class BridgeError(RuntimeError):
    pass


class Bridge:
    """One persistent connection; reconnects transparently because the game restarts often."""

    def __init__(self) -> None:
        self._sock: socket.socket | None = None
        self._buffer = b""
        self._next_id = 0

    def _connect(self) -> socket.socket:
        if self._sock is None:
            sock = socket.create_connection(BRIDGE_ADDR, timeout=3)
            # Longer than the plugin's 10 s main-thread timeout so its error message wins.
            sock.settimeout(15)
            self._sock, self._buffer = sock, b""
        return self._sock

    def close(self) -> None:
        if self._sock is not None:
            self._sock.close()
        self._sock = None

    def call(self, cmd: str, **args: Any) -> Any:
        self._next_id += 1
        request = json.dumps({"id": self._next_id, "cmd": cmd, "args": args}).encode() + b"\n"
        try:
            sock = self._connect()
            sock.sendall(request)
            while b"\n" not in self._buffer:
                chunk = sock.recv(65536)
                if not chunk:
                    raise ConnectionError("bridge closed the connection")
                self._buffer += chunk
        except OSError as e:
            self.close()
            raise BridgeError(f"bridge unreachable ({e}); is the game running with SeamlessSkyrim?") from e
        line, self._buffer = self._buffer.split(b"\n", 1)
        reply = json.loads(line)
        if not reply.get("ok"):
            raise BridgeError(reply.get("error", "unknown bridge error"))
        return reply.get("result")


bridge = Bridge()


# ---- helpers ------------------------------------------------------------------------------


def game_process() -> psutil.Process | None:
    for proc in psutil.process_iter(["name"]):
        if (proc.info["name"] or "").lower() == GAME_EXE.lower():
            return proc
    return None


def agent_game() -> psutil.Process | None:
    """The running game, for ending it: only one launched for an agent (game_launch's
    SEAMLESS_AGENT, inherited from the loader) may be. A game without it is the user playing; a
    quit that fell back to killing the process when the bridge didn't answer took theirs."""
    proc = game_process()
    if not proc:
        return None
    try:
        ours = proc.environ().get("SEAMLESS_AGENT") == "1"
    except psutil.Error:
        ours = False  # started elevated or by another user: not by game_launch
    if not ours:
        raise BridgeError("the Skyrim that is running was not launched for an agent (the user is playing): left alone")
    return proc


class LogWatcher:
    """Collects plugin log lines written while a call runs, so probe output comes back inline."""

    def __enter__(self) -> "LogWatcher":
        self.start = PLUGIN_LOG.stat().st_size if PLUGIN_LOG.exists() else 0
        self.lines: list[str] = []
        return self

    def __exit__(self, *exc: object) -> None:
        if not PLUGIN_LOG.exists():
            return
        size = PLUGIN_LOG.stat().st_size
        # The log is truncated when the game restarts; then everything in it is new.
        start = self.start if size >= self.start else 0
        with PLUGIN_LOG.open("rb") as f:
            f.seek(start)
            self.lines = f.read().decode("utf-8", "replace").splitlines()


def filter_log(lines: list[str], grep: str | None, limit: int = 60, width: int = 400) -> list[str]:
    """Only the lines matching grep (a regex), each cut to width: probes log far more than one
    question needs, and every line returned costs the agent context."""
    import re

    if not grep:
        return []
    pattern = re.compile(grep)
    picked = [line[:width] for line in lines if pattern.search(line)]
    return picked[:limit] + ([f"... {len(picked) - limit} more"] if len(picked) > limit else [])


def with_log(cmd: str, grep: str | None = None, **args: Any) -> dict[str, Any]:
    """grep None: every log line written during the call; otherwise only the matching ones."""
    with LogWatcher() as watcher:
        result = bridge.call(cmd, **args)
        time.sleep(0.2)  # give the log file a moment to be flushed before we read it
    if grep is None:
        return {"result": result, "log": watcher.lines}
    return {"result": result, "logLines": len(watcher.lines), "log": filter_log(watcher.lines, grep)}


def wait_until_in_game(timeout: float, settle: float = 2.0) -> dict[str, Any]:
    """Polls until the player is in the world and no loading screen is up.

    During loading the main thread doesn't service bridge tasks, so errors are expected and
    retried. Requires two consecutive good polls so we don't catch the frame before a load starts.
    """
    deadline = time.monotonic() + timeout
    good = 0
    last_error = "no response yet"
    while time.monotonic() < deadline:
        background.apply()  # loads can pull the game window to the front
        try:
            state = bridge.call("state")
            if state.get("inGame") and not state.get("loading"):
                good += 1
                if good >= 2:
                    return state
            else:
                good = 0
        except BridgeError as e:
            good, last_error = 0, str(e)
            # A crashed game shows as a refused connection too; waiting out the whole timeout for
            # it cost four minutes per crash.
            if not game_process():
                raise BridgeError("the game is no longer running (crashed?); check read_log('crash')") from e
        time.sleep(settle / 2)
    raise BridgeError(f"game not ready after {timeout:.0f}s ({last_error})")


def parse_pos(pos: list[float] | None) -> list[float] | None:
    if pos is None:
        return None
    if len(pos) not in (2, 3):
        raise ValueError("pos must be [x, y] or [x, y, z]")
    return [float(v) for v in pos]


# ---- Win32 input --------------------------------------------------------------------------

user32 = ctypes.WinDLL("user32", use_last_error=True)

KEYEVENTF_SCANCODE = 0x0008
KEYEVENTF_KEYUP = 0x0002
KEYEVENTF_EXTENDEDKEY = 0x0001
INPUT_KEYBOARD = 1


class KEYBDINPUT(ctypes.Structure):
    _fields_ = [
        ("wVk", wintypes.WORD),
        ("wScan", wintypes.WORD),
        ("dwFlags", wintypes.DWORD),
        ("time", wintypes.DWORD),
        ("dwExtraInfo", ctypes.c_size_t),
    ]


class INPUT(ctypes.Structure):
    class _U(ctypes.Union):
        # MOUSEINPUT is the largest member; pad so sizeof(INPUT) matches what SendInput expects.
        _fields_ = [("ki", KEYBDINPUT), ("pad", ctypes.c_byte * 32)]

    _anonymous_ = ("u",)
    _fields_ = [("type", wintypes.DWORD), ("u", _U)]


# DirectInput scan codes, which is what Skyrim reads. Extended keys carry 0xE0 in the high byte.
SCAN_CODES: dict[str, int] = {
    "esc": 0x01, "1": 0x02, "2": 0x03, "3": 0x04, "4": 0x05, "5": 0x06, "6": 0x07, "7": 0x08,
    "8": 0x09, "9": 0x0A, "0": 0x0B, "backspace": 0x0E, "tab": 0x0F,
    "q": 0x10, "w": 0x11, "e": 0x12, "r": 0x13, "t": 0x14, "y": 0x15, "u": 0x16, "i": 0x17,
    "o": 0x18, "p": 0x19, "enter": 0x1C, "ctrl": 0x1D, "a": 0x1E, "s": 0x1F, "d": 0x20,
    "f": 0x21, "g": 0x22, "h": 0x23, "j": 0x24, "k": 0x25, "l": 0x26, "console": 0x29,
    "shift": 0x2A, "z": 0x2C, "x": 0x2D, "c": 0x2E, "v": 0x2F, "b": 0x30, "n": 0x31, "m": 0x32,
    "alt": 0x38, "space": 0x39, "capslock": 0x3A,
    "f1": 0x3B, "f2": 0x3C, "f3": 0x3D, "f4": 0x3E, "f5": 0x3F, "f6": 0x40, "f7": 0x41,
    "f8": 0x42, "f9": 0x43, "f10": 0x44, "f11": 0x57, "f12": 0x58,
    "up": 0xE048, "down": 0xE050, "left": 0xE04B, "right": 0xE04D,
}


def game_window() -> int:
    return user32.FindWindowW(None, "Skyrim Special Edition")


AGENT_DESKTOP_NAME = "Skyrim"


def agent_desktop() -> VirtualDesktop:
    """The virtual desktop the game lives on while an agent drives it.

    Prefers one named "Skyrim", else the last existing non-first desktop, else creates one.
    """
    desktops = get_virtual_desktops()
    for desktop in desktops:
        if desktop.name == AGENT_DESKTOP_NAME:
            return desktop
    if len(desktops) > 1:
        return desktops[-1]
    desktop = VirtualDesktop.create()
    desktop.rename(AGENT_DESKTOP_NAME)
    return desktop


class Background:
    """Keeps the game on its own virtual desktop so the user can keep working while an agent drives it.

    Never minimizes: that resizes the client area to 0x0, the engine recreates its render targets
    at that size and Community Shaders crashes. A window on an inactive virtual desktop keeps its
    size and keeps rendering, and with bAlwaysActive=1 the game keeps simulating.
    """

    def __init__(self) -> None:
        self.enabled = True
        self.user_window = 0
        self.user_desktop = 1
        # Applied by the plugin at the next launch (SEAMLESS_AGENT_MUTE).
        self.mute = True
        # The game window already moved away once. After that it is left alone: moving it again
        # (and switching the user back each time the game took the foreground) kept throwing the
        # user off the game's desktop when they went there to watch or play.
        self.parked_hwnd = 0

    def remember_user_window(self) -> None:
        hwnd = user32.GetForegroundWindow()
        if hwnd and hwnd != game_window():
            self.user_window = hwnd
            try:
                self.user_desktop = VirtualDesktop.current().number
            except Exception:
                pass

    def park(self) -> None:
        """Move the game to the agent desktop if it isn't there yet (never while it's active)."""
        hwnd = game_window()
        if not self.enabled or not hwnd or hwnd == self.parked_hwnd or user32.GetForegroundWindow() == hwnd:
            return
        try:
            target = agent_desktop()
            view = AppView(hwnd)
            if view.desktop.number != target.number:
                view.move(target)
            self.parked_hwnd = hwnd
        except Exception as e:  # pyvda raises COM errors while the window is still being created
            print(f"park failed: {e}", file=sys.stderr)

    def restore_user(self) -> None:
        """Undo the game grabbing the foreground: back to the user's desktop and window.

        Only called while the agent is driving (launch, loads, door transitions, walking) and
        shortly after. The game still activates itself at times despite the plugin's focus guards
        (other mods create/show the window too), and Windows then switches to its desktop.
        """
        hwnd = game_window()
        if not self.enabled or not hwnd or hwnd == self.parked_hwnd or user32.GetForegroundWindow() != hwnd:
            return
        try:
            if VirtualDesktop.current().number != self.user_desktop:
                VirtualDesktop(self.user_desktop).go()
        except Exception as e:
            print(f"desktop restore failed: {e}", file=sys.stderr)
        if self.user_window and user32.IsWindow(self.user_window):
            set_foreground(self.user_window)

    def apply(self) -> None:
        self.restore_user()
        self.park()

    def guard(self, seconds: float) -> None:
        """Keep applying for a while after an agent action, on a background thread."""
        self.guard_until = max(getattr(self, "guard_until", 0.0), time.monotonic() + seconds)
        if not getattr(self, "_guard_thread", None) or not self._guard_thread.is_alive():
            self._guard_thread = threading.Thread(target=self._guard_loop, daemon=True)
            self._guard_thread.start()

    def _guard_loop(self) -> None:
        try:
            comtypes.CoInitialize()
        except OSError:
            pass
        while time.monotonic() < self.guard_until:
            try:
                self.apply()
            except Exception as e:
                print(f"guard: {e}", file=sys.stderr)
            time.sleep(0.15)


background = Background()


def set_foreground(hwnd: int) -> bool:
    # Windows refuses SetForegroundWindow from a background process unless it "just received
    # input" (tested: AttachThreadInput and SwitchToThisWindow did not help). Any synthetic input
    # satisfies the rule, but it lands in the current foreground window: an Alt tap opened a menu
    # in the game (pausing it), so use a zero-distance mouse move that does nothing anywhere.
    send_null_mouse_move()
    user32.SetForegroundWindow(hwnd)
    time.sleep(0.3)
    return user32.GetForegroundWindow() == hwnd


def focus_game() -> bool:
    hwnd = game_window()
    if not hwnd:
        return False
    if user32.GetForegroundWindow() == hwnd:
        return True
    # Key presses need the window in front, which ends background mode until it's re-enabled.
    background.remember_user_window()
    background.enabled = False
    return set_foreground(hwnd)


class MOUSEINPUT(ctypes.Structure):
    _fields_ = [
        ("dx", wintypes.LONG),
        ("dy", wintypes.LONG),
        ("mouseData", wintypes.DWORD),
        ("dwFlags", wintypes.DWORD),
        ("time", wintypes.DWORD),
        ("dwExtraInfo", ctypes.c_size_t),
    ]


MOUSEEVENTF_MOVE = 0x0001


def send_null_mouse_move() -> None:
    event = INPUT(type=0)  # INPUT_MOUSE
    ctypes.memmove(ctypes.addressof(event.u), ctypes.byref(MOUSEINPUT(0, 0, 0, MOUSEEVENTF_MOVE, 0, 0)), ctypes.sizeof(MOUSEINPUT))
    user32.SendInput(1, ctypes.byref(event), ctypes.sizeof(INPUT))


def send_scan(code: int, key_up: bool) -> None:
    flags = KEYEVENTF_SCANCODE | (KEYEVENTF_KEYUP if key_up else 0)
    if code > 0xFF:
        flags |= KEYEVENTF_EXTENDEDKEY
    event = INPUT(type=INPUT_KEYBOARD)
    event.ki = KEYBDINPUT(0, code & 0xFF, flags, 0, 0)
    if user32.SendInput(1, ctypes.byref(event), ctypes.sizeof(INPUT)) != 1:
        raise OSError(ctypes.get_last_error(), "SendInput failed")


# ---- tools: lifecycle ---------------------------------------------------------------------


@mcp.tool()
def game_status() -> dict[str, Any]:
    """Whether Skyrim is running, whether the SeamlessSkyrim bridge answers, and the game state."""
    proc = game_process()
    status: dict[str, Any] = {"running": proc is not None, "pid": proc.pid if proc else None}
    if proc:
        try:
            status["bridge"] = bridge.call("ping")
            status["state"] = bridge.call("state")
        except BridgeError as e:
            status["bridge"] = f"unavailable: {e}"
    return status


@mcp.tool()
def game_window_mode(mode: str) -> dict[str, Any]:
    """"background": keep the game on its own virtual desktop (it keeps running thanks to
    bAlwaysActive=1) and give focus back to the user; "show": switch to the game's desktop and
    focus it, to watch or play."""
    if mode == "background":
        background.enabled = True
        background.parked_hwnd = 0  # asked for explicitly: move it away once more
        background.apply()
    elif mode == "show":
        background.remember_user_window()
        background.enabled = False
        if hwnd := game_window():
            AppView(hwnd).desktop.go()
            set_foreground(hwnd)
    else:
        raise ValueError('mode must be "background" or "show"')
    return {"mode": mode}


@mcp.tool()
def game_audio(muted: bool) -> dict[str, Any]:
    """Mute or unmute the game (muted by default when launched through game_launch); also applies
    to later launches in this session."""
    background.mute = muted
    if game_process():
        return bridge.call("audio", muted=muted)
    return {"muted": muted}


def mo2_running() -> bool:
    return any((proc.info["name"] or "").lower() == "modorganizer.exe" for proc in psutil.process_iter(["name"]))


def profile_dir() -> Path:
    return MO2_DIR / "profiles" / MO2_PROFILE


def check_profile_saves() -> None:
    """Saves of a run must land in the profile's own folder, never in the Documents one (the user's)."""
    settings = profile_dir() / "settings.ini"
    text = settings.read_text("utf-8", "replace") if settings.exists() else ""
    if "LocalSaves=true" not in text or "LocalSettings=true" not in text:
        raise BridgeError(f"MO2 profile '{MO2_PROFILE}' is missing or has no local saves/INIs ({settings}): build.py makes it (modlist.yaml test_profile)")


def mo2_selected_profile() -> str | None:
    ini = MO2_DIR / "ModOrganizer.ini"
    for line in ini.read_text("utf-8", "replace").splitlines() if ini.exists() else []:
        if line.startswith("selected_profile="):
            return line.split("(", 1)[1].rsplit(")", 1)[0] if "(" in line else line.split("=", 1)[1]
    return None


def restore_mo2_profile(wanted: str | None, wait: float = 30) -> None:
    """MO2 may remember the profile it was started with (-p): put the user's own one back once MO2 has closed."""
    deadline = time.monotonic() + wait
    while mo2_running() and time.monotonic() < deadline:
        time.sleep(1)
    ini = MO2_DIR / "ModOrganizer.ini"
    if wanted and not mo2_running() and mo2_selected_profile() not in (None, wanted):
        text = ini.read_text("utf-8", "replace")
        ini.write_text(text.replace(f"selected_profile=@ByteArray({mo2_selected_profile()})", f"selected_profile=@ByteArray({wanted})"), "utf-8", newline="")


launched_selected_profile: str | None = None


def launch_command() -> tuple[list[str], Path]:
    if LAUNCH_MODE == "mo2":
        # MO2 starts the game inside its virtual file system with MO2's own environment, which is
        # ours: the SEAMLESS_AGENT_* variables below reach SkyrimSE.exe and the plugin.
        return [str(MO2_DIR / "ModOrganizer.exe"), "-p", MO2_PROFILE, f"moshortcut://:{MO2_EXE_TITLE}"], MO2_DIR
    return [str(GAME_DIR / "skse64_loader.exe")], GAME_DIR


@mcp.tool()
def game_launch(save: str | None = None, timeout_seconds: float = 300, background_mode: bool = True) -> dict[str, Any]:
    """Start Skyrim (through MO2 and SKSE with SKYRIM_LAUNCH=mo2, the default) and wait for the main menu.

    If `save` is given (file name without extension, see list_saves) and exists, load it and wait
    until the player is in the world; a name with no save file is ignored (use place_go / run_route:
    their {"load": name} steps fall back to starts.json). With background_mode the game runs on its
    own virtual desktop so the user can keep working; switch with game_window_mode. Muted unless
    game_audio(False) was called. Refuses while the user's game or, with MO2, MO2 itself is open.
    """
    if game_process():
        return {"error": "Skyrim is already running", "status": game_status()}
    if LAUNCH_MODE == "mo2" and mo2_running():
        # A second ModOrganizer.exe hands the launch to the one that is open, and that one's
        # environment has no agent variables: the game would start without the bridge.
        return {"error": "Mod Organizer 2 is open: close it, a game it starts has no agent bridge"}
    if LAUNCH_MODE == "mo2":
        check_profile_saves()
        global launched_selected_profile
        launched_selected_profile = mo2_selected_profile()
    if save and not save_exists(save):
        # A route's author's save: the clean base save stands in when it exists (starts.json then places the player).
        save = BASE_SAVE if save in load_json(STARTS_FILE) and save_exists(BASE_SAVE) else None
    background.enabled = background_mode
    background.parked_hwnd = 0
    background.remember_user_window()
    background.guard(timeout_seconds)
    env = dict(os.environ)
    # The plugin only runs the bridge (and its other agent-only hooks) for games we launch.
    env["SEAMLESS_AGENT"] = "1"
    if background_mode:
        # The plugin then stops the game from activating its own window (AgentMode.cpp);
        # otherwise Windows follows each activation to the game's desktop and drags the user along.
        env["SEAMLESS_AGENT_BACKGROUND"] = "1"
    if background.mute:
        # Muted inside the game (AgentMode::InstallMute), never in the Windows volume mixer: Windows
        # keeps a mixer mute across runs, and a game that crashed while muted left the user's own
        # games silent.
        env["SEAMLESS_AGENT_MUTE"] = "1"
    if save:
        # The plugin loads it itself the moment the main menu opens (AgentMode::InstallAutoLoad).
        env["SEAMLESS_AGENT_LOAD"] = save
    command, cwd = launch_command()
    subprocess.Popen(command, cwd=cwd, env=env)
    started = time.monotonic()
    deadline = started + timeout_seconds
    # The loader exits immediately and SkyrimSE.exe takes a moment to appear (MO2 first builds its
    # virtual file system), so only treat a missing process as a startup crash after a grace period.
    grace = 90 if LAUNCH_MODE == "mo2" else 20
    while time.monotonic() < deadline:
        # The game takes focus when its window appears; the plugin forces bAlwaysActive, so it
        # keeps starting up once we hand focus back.
        background.apply()
        try:
            state = bridge.call("state")
            if state.get("mainMenu") or state.get("inGame"):
                break
        except BridgeError:
            pass
        time.sleep(0.5)  # poll faster than the bridge check so the window is parked quickly
        background.apply()
        if time.monotonic() - started > grace and not game_process():
            raise BridgeError("Skyrim exited during startup; check read_log('skse') / read_log('crash')")
        time.sleep(2)
    else:
        raise BridgeError("timed out waiting for the main menu")
    background.apply()
    if save:
        # Already loading (see SEAMLESS_AGENT_LOAD above): only wait for the player.
        return wait_until_in_game(max(60.0, deadline - time.monotonic()))
    return bridge.call("state")


@mcp.tool()
def game_quit(force: bool = False, timeout_seconds: float = 30) -> dict[str, Any]:
    """Quit Skyrim. Tries the clean `qqq` console command first; `force` kills the process."""
    proc = agent_game()
    if not proc:
        return {"quit": True, "note": "was not running"}
    if not force:
        try:
            bridge.call("console", command="qqq")
            proc.wait(timeout=timeout_seconds)
            bridge.close()
            restore_mo2_profile(launched_selected_profile)
            return {"quit": True, "method": "qqq"}
        except (BridgeError, psutil.TimeoutExpired):
            pass
    proc.kill()
    bridge.close()
    restore_mo2_profile(launched_selected_profile)
    return {"quit": True, "method": "kill"}


# ---- tools: state and world ---------------------------------------------------------------


@mcp.tool()
def get_state() -> dict[str, Any]:
    """Menus (main menu, loading, console, paused), player position/heading/cell/worldspace, walk status."""
    return bridge.call("state")


@mcp.tool()
def refs_near(radius: float = 2000, kind: str = "all", limit: int = 50) -> dict[str, Any]:
    """References around the player in loaded cells, nearest first.

    kind: "all", "doors" (includes load-door teleport targets) or "actors". Units: 1 = ~1.4 cm.
    """
    return bridge.call("refs_near", radius=radius, kind=kind, limit=limit)


@mcp.tool()
def console(command: str) -> dict[str, Any]:
    """Run a console command (e.g. "coc WhiterunBanneredMare", "player.setav speedmult 300", "tgm").

    Console output is not captured; use get_state or screenshot to see the effect.
    """
    background.remember_user_window()
    background.guard(10)  # coc/load commands load cells
    return with_log("console", command=command)


# ---- tools: saves -------------------------------------------------------------------------


def save_dir() -> Path:
    """The folder the game actually loads from.

    SKYRIM_SAVES_DIR (default with MO2: the profile's saves folder). Otherwise Vortex profiles
    redirect saves via SLocalSavePath in Skyrim.ini; saves outside that folder belong to other
    profiles and the game silently ignores load requests for them.
    """
    if SAVES_DIR:
        return SAVES_DIR
    ini = DOCS_DIR / "Skyrim.ini"
    if ini.exists():
        for line in ini.read_text("utf-8", "replace").splitlines():
            key, _, value = line.partition("=")
            if key.strip().lower() == "slocalsavepath" and value.strip():
                return DOCS_DIR / value.strip()
    return DOCS_DIR / "Saves"


@mcp.tool()
def list_saves(limit: int = 20) -> list[dict[str, Any]]:
    """Most recent save files of the active profile (name to pass to load_game, modified time)."""
    saves = sorted(save_dir().glob("*.ess"), key=lambda p: p.stat().st_mtime, reverse=True)
    return [
        {"name": p.stem, "modified": time.strftime("%Y-%m-%d %H:%M", time.localtime(p.stat().st_mtime))}
        for p in saves[:limit]
    ]


@mcp.tool()
def load_game(name: str, timeout_seconds: float = 180) -> dict[str, Any]:
    """Load a save by file name (without .ess) and wait until the player is in the world."""
    background.remember_user_window()
    background.guard(timeout_seconds + 5)
    bridge.call("load_game", name=name)
    time.sleep(3)  # let the loading screen open before polling for "done"
    return wait_until_in_game(timeout_seconds)


@mcp.tool()
def save_game(name: str) -> dict[str, Any]:
    """Save the game under `name`."""
    return bridge.call("save_game", name=name)


# ---- tools: player control ----------------------------------------------------------------


@mcp.tool()
def move_to(ref: str | None = None, pos: list[float] | None = None) -> dict[str, Any]:
    """Teleport the player to a reference (hex form ID) or to [x, y, z] in the current cell/worldspace."""
    if ref:
        return bridge.call("move_to", ref=ref)
    return bridge.call("move_to", pos=parse_pos(pos))


@mcp.tool()
def activate(ref: str, wait_for_load: bool = True, timeout_seconds: float = 120) -> dict[str, Any]:
    """Make the player activate a reference (door, NPC, container...).

    For load doors, waits for the cell transition to finish and returns the new state plus the
    plugin log lines produced (door probe output).
    """
    background.remember_user_window()
    background.guard(timeout_seconds + 5)
    with LogWatcher() as watcher:
        result = bridge.call("activate", ref=ref)
        state = None
        if wait_for_load:
            time.sleep(1.5)
            state = wait_until_in_game(timeout_seconds)
        time.sleep(0.2)
    return {"result": result, "state": state, "log": watcher.lines}


@mcp.tool()
def walk_to(
    ref: str | None = None,
    pos: list[float] | None = None,
    stop_distance: float = 64,
    run: bool = False,
    timeout_seconds: float = 30,
    wait: bool = True,
) -> dict[str, Any]:
    """Walk the player (engine autorun + steering, real collision) toward a reference or [x, y(, z)].

    Stops when within stop_distance, on timeout, or when stuck (e.g. against a wall). Steering is
    straight-line: no pathfinding around obstacles. Needs the game to be unpaused and focused or
    running with bAlwaysActive=1.
    """
    args: dict[str, Any] = {"stopDistance": stop_distance, "run": run, "timeoutSeconds": timeout_seconds}
    if ref:
        args["ref"] = ref
    else:
        args["pos"] = parse_pos(pos)
    background.remember_user_window()
    background.guard(timeout_seconds + 10)  # walking through a load door loads a cell
    bridge.call("walk_to", **args)
    if not wait:
        return bridge.call("state")
    deadline = time.monotonic() + timeout_seconds + 5
    while time.monotonic() < deadline:
        time.sleep(0.25)
        try:
            state = bridge.call("state")
        except BridgeError:
            continue  # a load screen (walked through a door) blocks the main thread briefly
        if not state["walk"]["active"]:
            return state
    return bridge.call("walk_stop")


@mcp.tool()
def walk_stop() -> dict[str, Any]:
    """Cancel an ongoing walk_to."""
    return bridge.call("walk_stop")


@mcp.tool()
def make_base_save(name: str = "", settle_seconds: float = 90) -> dict[str, Any]:
    """Make the clean start routes load (default SeamlessTestBase): a new game from the main menu (`coc`
    into Whiterun), every message box that pauses it answered No (the Survival Mode prompt: `d`, `e`
    through the bridge) until the game has run quietly for a while, then saved in the test profile's own
    saves folder. Launches the game if it isn't running (a game that is must be ours, at the main menu)."""
    name = name or BASE_SAVE
    if not game_process():
        game_launch()
    else:
        agent_game()
    background.remember_user_window()
    background.guard(settle_seconds + 60)
    bridge.call("console", command="coc WhiterunPlainsDistrict03")
    time.sleep(3)
    wait_until_in_game(90)
    answered, quiet_since, start = 0, time.monotonic(), time.monotonic()
    while time.monotonic() - start < settle_seconds or time.monotonic() - quiet_since < 25:
        if bridge.call("state").get("paused"):
            bridge.call("key", code=SCAN_CODES["d"], repeat=1)  # menus move with WASD; the arrow keys did nothing
            time.sleep(0.5)
            bridge.call("key", code=SCAN_CODES["e"], repeat=1)
            answered += 1
            quiet_since = time.monotonic()
            time.sleep(1.5)
        else:
            time.sleep(2)
        if time.monotonic() - start > settle_seconds + 240:
            raise BridgeError("the game stays paused after answering its message boxes: look at a screenshot")
    bridge.call("save_game", name=name)
    time.sleep(3)
    if not save_exists(name):
        raise BridgeError(f"saved '{name}' but {save_dir() / (name + '.ess')} does not exist")
    return {"saved": name, "folder": str(save_dir()), "messageBoxesAnswered": answered}


# ---- tools: places and routes -------------------------------------------------------------
#
# A test scene is usually "stand here, look there" after a fixed walk. Replayed by hand that was
# a dozen tool calls (launch, walks, door, turn) per rebuild; stored once, it is one call.

PLACES_FILE = Path(__file__).resolve().parent / "places.json"
ROUTES_FILE = Path(__file__).resolve().parent / "routes.json"


def load_json(path: Path) -> dict[str, Any]:
    return json.loads(path.read_text("utf-8")) if path.exists() else {}


def store_json(path: Path, data: dict[str, Any]) -> None:
    # LF as git has them: written through Windows' text mode, every place_save and route_save
    # turned the whole file's line ends over.
    path.write_text(json.dumps(data, indent=2, sort_keys=True) + "\n", "utf-8", newline="\n")


def brief(state: dict[str, Any]) -> str:
    """One line for a state: cell, position, heading, walk status."""
    player = state.get("player") or {}
    cell = (player.get("cell") or {}).get("editorId") or (player.get("cell") or {}).get("id", "?")
    pos = ",".join(f"{v:.0f}" for v in player.get("pos", []))
    walk = (state.get("walk") or {}).get("status", "")
    return f"{cell} ({pos}) heading {player.get('headingDeg', 0):.0f}" + (f", walk {walk}" if walk and walk != "idle" else "")


@mcp.tool()
def place_save(name: str, note: str = "") -> dict[str, Any]:
    """Remember where the player stands and looks (cell, position, heading) as `name`."""
    state = bridge.call("state")
    player = state["player"]
    place = {
        "cell": player["cell"]["editorId"],
        "interior": player["cell"]["interior"],
        "pos": player["pos"],
        "headingDeg": player["headingDeg"],
        "note": note,
    }
    places = load_json(PLACES_FILE)
    places[name] = place
    store_json(PLACES_FILE, places)
    return {"saved": name, **place}


def go_to_place(name: str) -> dict[str, Any]:
    """The place's cell by `coc` (an exterior cell named Wilderness, which `coc` can't pick, by
    `cow <worldspace> <grid x> <grid y>` from its "worldspace" and position), then its exact
    position and heading. A place without "pos" is just where `coc` puts the player."""
    places = load_json(PLACES_FILE)
    if name not in places:
        raise ValueError(f"no place '{name}'; known: {', '.join(sorted(places))}")
    place = places[name]
    state = bridge.call("state")
    player = state.get("player") or {}
    here = (player.get("cell") or {}).get("editorId")
    if place["cell"] == "Wilderness":
        if not place.get("worldspace"):
            raise ValueError(f"place '{name}' is in a Wilderness cell and has no 'worldspace' for cow")
        x, y = place["pos"][:2]
        near = here == "Wilderness" and math.hypot(player["pos"][0] - x, player["pos"][1] - y) < 8192
        load = None if near else f"cow {place['worldspace']} {math.floor(x / 4096)} {math.floor(y / 4096)}"
    elif here != place["cell"]:
        if not place["cell"]:
            raise ValueError(f"place '{name}' is in a cell without an editor id; coc can't reach it")
        load = f"coc {place['cell']}"
    else:
        load = None
    if load:
        background.remember_user_window()
        background.guard(60)
        bridge.call("console", command=load)
        time.sleep(3)
        wait_until_in_game(60)
    if "pos" in place:
        bridge.call("move_to", pos=place["pos"])
    if "headingDeg" in place:
        bridge.call("console", command=f"player.setangle z {place['headingDeg']:.2f}")
    time.sleep(0.5)
    return bridge.call("state")


# A route's {"load": name} names a save of the author's (SeamlessTest...). Where this machine has no
# such save, starts.json says where that save was: {"place": name} of a stored place, or {"coc":
# cell} (where `coc` puts the player, usually at the cell's front door), then "console" commands.
STARTS_FILE = Path(__file__).resolve().parent / "starts.json"


def save_exists(name: str) -> bool:
    return (save_dir() / f"{name}.ess").exists()


def start_from(name: str) -> dict[str, Any]:
    if save_exists(name):
        return load_game(name)
    starts = load_json(STARTS_FILE)
    if name not in starts:
        raise ValueError(f"no save '{name}' in {save_dir()} and no start for it in starts.json (known: {', '.join(sorted(starts))})")
    start = starts[name]
    if not (bridge.call("state").get("inGame")) and save_exists(BASE_SAVE):
        load_game(BASE_SAVE)  # from the main menu: the clean new game, no popups; then coc/cow to the place
    if "place" in start:
        state = go_to_place(start["place"])
    else:
        background.remember_user_window()
        background.guard(60)
        bridge.call("console", command=f"coc {start['coc']}")
        time.sleep(3)
        state = wait_until_in_game(60)
    for command in start.get("console", []):
        bridge.call("console", command=command)
    return state


@mcp.tool()
def place_go(name: str) -> dict[str, Any]:
    """Teleport to a place stored with place_save: coc into its cell when elsewhere (a load, so the
    "loaded there" path, not "walked in"), then its exact position and heading."""
    return {"state": brief(go_to_place(name))}


@mcp.tool()
def place_list() -> dict[str, Any]:
    """Stored places and routes."""
    return {"places": load_json(PLACES_FILE), "routes": load_json(ROUTES_FILE)}


def run_step(step: dict[str, Any]) -> str:
    (kind, arg), = step.items()
    if kind == "load":
        return brief(start_from(arg))
    if kind == "place":
        return brief(go_to_place(arg))
    if kind == "walk":
        return brief(walk_to(**arg))
    if kind == "activate":
        return brief(activate(arg)["state"] or bridge.call("state"))
    if kind == "console":
        bridge.call("console", command=arg)
        return f"console '{arg}'"
    if kind == "heading":
        bridge.call("console", command=f"player.setangle z {float(arg):.2f}")
        return f"heading {float(arg):.0f}"
    if kind == "move":
        return brief(bridge.call("move_to", pos=parse_pos(arg)))
    if kind == "wait":
        time.sleep(float(arg))
        return f"waited {float(arg):.1f} s"
    if kind == "frames":
        global route_bursts
        if route_capture is None:
            return "frames skipped (run_route without capture)"
        route_bursts += 1
        # Doesn't wait: the steps after it are what the frames should show.
        return f"capturing {start_burst(route_capture / f'burst_{route_bursts:02}', int(arg))} frames"
    if kind == "check":
        return check_step(arg or {})
    raise ValueError(f"unknown step '{kind}' (load, place, walk, activate, console, heading, move, wait, frames, check)")


# ---- checks and the test suite -------------------------------------------------------------
#
# A route's {"check": {...}} step asks the plugin whether everything that stands still around the
# player and in the space merged behind the door hangs in the scene where it belongs (bridge
# `scene_check`), and takes a picture. Pictures can't be compared automatically (time of day,
# people walking by); the check can. run_suite runs the routes listed in suite.json and puts
# each picture beside the one last accepted.

SUITE_FILE = Path(__file__).resolve().parent / "suite.json"
SUITE_REFERENCE = Path(__file__).resolve().parent / "suite-reference"
SUITE_RUNS = Path(tempfile.gettempdir()) / "skyrim-mcp" / "suite"
# Set while run_suite runs a route: where its pictures go. route_checks: the checks made since the
# route started.
suite_route: str | None = None
suite_run_dir: Path | None = None
route_checks: list[dict[str, Any]] = []


def take_picture(path: Path, max_width: int = 960) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    raw = path.with_suffix(".raw.jpg")
    raw.unlink(missing_ok=True)
    bridge.call("screenshot", path=str(raw))
    with PILImage.open(raw) as img:
        img = img.convert("RGB")
        if img.width > max_width:
            img = img.resize((max_width, round(img.height * max_width / img.width)), PILImage.LANCZOS)
        img.save(path, format="JPEG", quality=85)
    raw.unlink(missing_ok=True)


def check_step(arg: dict[str, Any]) -> str:
    """{"check": {}}: scene_check (radius 3000), and a picture while run_suite runs. Options:
    "radius"; "merged": true or false, whether a space should be merged behind the door (not
    checked when left out); "worldspace": the form ID the player must be in (a city's own, not the
    street worldspace's low-detail copy of it); "name": the picture's name within the route
    (default: its number)."""
    result = bridge.call("scene_check", radius=float(arg.get("radius", 3000)))
    problems: list[str] = []
    if "worldspace" in arg:
        here = (bridge.call("state").get("player") or {}).get("worldspace", "")
        if here.upper() != str(arg["worldspace"]).upper():
            problems.append(f"in worldspace {here or 'none'}, expected {arg['worldspace']}")
    for part in ("here", "merged"):
        if result.get(part):
            problems += [f"{part}: {p}" for p in result[part]["problems"]]
            if result[part]["problemCount"] > len(result[part]["problems"]):
                problems.append(f"{part}: ... {result[part]['problemCount']} in all")
    if "merged" in arg and bool(arg["merged"]) != (result.get("merged") is not None):
        problems.insert(0, f"expected the space behind the door {'merged' if arg['merged'] else 'not merged'}")
    number = len(route_checks) + 1
    picture = None
    if suite_run_dir is not None and suite_route:
        picture = suite_run_dir / f"{suite_route}__{arg.get('name', number)}.jpg"
        take_picture(picture)
    route_checks.append({
        "check": arg.get("name", number),
        "ok": not problems,
        "cell": result.get("cell"),
        "checked": {"here": result["here"]["checked"], "merged": (result.get("merged") or {}).get("checked")},
        "problems": problems,
        "picture": str(picture) if picture else None,
    })
    counts = f"here {result['here']['checked']}" + (f", merged {result['merged']['checked']}" if result.get("merged") else "")
    return f"check ok ({counts})" if not problems else f"check FAILED ({counts}): {len(problems)} problem(s), first: {problems[0]}"


def suite_sheet(checks: list[dict[str, Any]]) -> bytes:
    """One row per check: the accepted picture on the left, this run's on the right."""
    from PIL import ImageDraw

    width, height, label = 640, 270, 22
    rows = [c for c in checks if c.get("picture")]
    sheet = PILImage.new("RGB", (2 * width, max(1, len(rows)) * (height + label)), (24, 24, 24))
    draw = ImageDraw.Draw(sheet)
    for i, check in enumerate(rows):
        y = i * (height + label)
        now = Path(check["picture"])
        reference = SUITE_REFERENCE / now.name
        status = "ok" if check["ok"] else f"FAILED: {check['problems'][0]}"
        draw.text((6, y + 4), f"{now.stem}  (left: accepted, right: now)  {status}"[:160], fill=(255, 255, 255) if check["ok"] else (255, 90, 90))
        for column, path in enumerate((reference, now)):
            if path.exists():
                with PILImage.open(path) as img:
                    sheet.paste(img.convert("RGB").resize((width, height)), (column * width, y + label))
            else:
                draw.text((column * width + 10, y + label + 10), "no accepted picture yet", fill=(200, 200, 200))
    out = io.BytesIO()
    sheet.save(out, format="JPEG", quality=80)
    return out.getvalue()


def main_thread_answers() -> bool:
    try:
        bridge.call("state")
        return True
    except BridgeError:
        return False


# A text and a picture: as structured output the picture can't be serialized.
@mcp.tool(structured_output=False)
def run_suite(routes: list[str] | None = None, accept: bool = False) -> list[Any]:
    """Run the test routes of suite.json (or the given ones) and report each route's checks.

    Each route's {"check": {...}} steps ask the plugin whether everything that stands still around
    the player and in the space merged behind the door hangs in the scene where it belongs (the
    merged street's land and distant LOD too), and take a picture. Returns a summary and a sheet
    with each picture beside the one last accepted; accept: this run's pictures become the
    accepted ones. Launches the game if it isn't running; a route the game crashed or hung in
    (its main thread sampled first, for sampler_report.py) is run once more after a relaunch.
    Takes a few minutes.
    """
    global suite_route, suite_run_dir, route_checks
    suite = load_json(SUITE_FILE)
    all_routes = load_json(ROUTES_FILE)
    names = routes or suite["routes"]
    agent_game()  # raises for the user's own game, before a failed step is taken for a hang in it
    suite_run_dir = fresh_dir(SUITE_RUNS / time.strftime("%Y%m%d-%H%M%S"))
    results: list[dict[str, Any]] = []
    all_checks: list[dict[str, Any]] = []
    try:
        for name in names:
            steps = all_routes[name]["steps"]
            for attempt in (1, 2):
                suite_route, route_checks = name, []
                done: list[str] = []
                outcome = "ok"
                try:
                    if not game_process():
                        game_launch(save=next((s["load"] for s in steps if "load" in s), None))
                    for step in steps:
                        done.append(f"{json.dumps(step)} -> {run_step(step)}")
                        if "load" in step:
                            # The same light in every run, for the pictures.
                            for command in suite.get("afterLoad", []):
                                bridge.call("console", command=command)
                except Exception as e:  # noqa: BLE001 -- reported with the steps that ran
                    done.append(f"FAILED: {e}")
                    outcome = "error" if game_process() else "crashed"
                    if outcome == "error" and not main_thread_answers():
                        # Hung: where its main thread spins, for sampler_report.py, then a fresh game.
                        outcome = "hung"
                        profile = suite_run_dir / f"{name}__hang.samples"
                        try:
                            bridge.call("profile", file=str(profile), seconds=2.0)
                            time.sleep(3)
                            done.append(f"main thread sampled into {profile}")
                        except BridgeError as profile_error:
                            if "not sampling" in str(profile_error):
                                done.append(f"main thread {profile_error}")
                            else:
                                outcome = "crashed"  # the bridge thread is gone too: a crash in progress
                        game_quit(force=True)
                    if outcome in ("crashed", "hung"):
                        # The relaunch below starts the plugin's log anew: without a crash log (an
                        # exit without one happens) nothing was left to say what the game did last.
                        kept = suite_run_dir / f"{name}__{outcome}.log"
                        kept.write_bytes(PLUGIN_LOG.read_bytes())
                        done.append(f"plugin log kept in {kept}")
                if outcome == "ok" and not route_checks:
                    outcome = "error"
                    done.append("the route has no check step")
                if outcome == "ok" and not all(c["ok"] for c in route_checks):
                    outcome = "failed"
                if outcome in ("crashed", "hung") and attempt == 1:
                    results.append({"route": name, "result": f"{outcome}; run once more", "steps": done})
                    continue
                break
            all_checks += route_checks
            results.append({"route": name, "result": outcome, "checks": [{k: v for k, v in c.items() if k != "picture"} for c in route_checks],
                            "steps": done if outcome != "ok" else len(done)})
    finally:
        suite_route = None
    if accept:
        import shutil

        SUITE_REFERENCE.mkdir(exist_ok=True)
        for check in all_checks:
            if check.get("picture"):
                shutil.copy2(check["picture"], SUITE_REFERENCE / Path(check["picture"]).name)
    summary = {
        "passed": sum(r["result"] == "ok" for r in results),
        "of": len(names),
        "results": results,
        "pictures": str(suite_run_dir),
        "accepted": accept,
    }
    suite_run_dir = None
    return [json.dumps(summary, indent=1), Image(data=suite_sheet(all_checks), format="jpeg")]


@mcp.tool()
def route_save(name: str, steps: list[dict[str, Any]], note: str = "") -> dict[str, Any]:
    """Store a sequence of steps as `name` for run_route. Each step is a one-key object:
    {"load": save}, {"place": name}, {"walk": walk_to arguments}, {"activate": ref},
    {"console": command}, {"heading": degrees}, {"move": [x, y, z]}, {"wait": seconds},
    {"frames": n} (with run_route's capture: a burst of the next n frames, taken while the steps
    after it run), {"check": {...}} (the scene checked, see run_suite)."""
    routes = load_json(ROUTES_FILE)
    routes[name] = {"steps": steps, "note": note}
    store_json(ROUTES_FILE, routes)
    return {"saved": name, "steps": len(steps)}


@mcp.tool()
def run_route(name: str = "", steps: list[dict[str, Any]] | None = None, grep: str = "", capture: bool = False) -> dict[str, Any]:
    """Run a stored route (name) or the given steps in one call; returns one line per step, and
    the plugin log lines written meanwhile that match grep (a regex; empty: none).

    capture: also take every frame at each door crossing (16 frames) and each merge of a room into
    the scene (40), and at the route's {"frames": n} steps. Returns one entry per burst under
    "bursts", with the frames that stand apart from their neighbours ("oddFrames"); look at one
    with frames_sheet(directory). A merge during a crossing's frames makes that burst longer rather
    than starting its own. Off by default:
    writing a burst out stalls the game for a moment, which spoils a stutter measurement.
    """
    global route_capture, route_bursts
    if name:
        routes = load_json(ROUTES_FILE)
        if name not in routes:
            raise ValueError(f"no route '{name}'; known: {', '.join(sorted(routes))}")
        steps = routes[name]["steps"]
    done: list[str] = []
    reply: dict[str, Any] = {"steps": done}
    route_capture, route_bursts = (fresh_dir(FRAMES_DIR / (name or "route")) if capture else None), 0
    try:
        if route_capture is not None:
            bridge.call("capture_events", directory=str(route_capture))
        with LogWatcher() as watcher:
            for step in steps or []:
                try:
                    done.append(f"{json.dumps(step)} -> {run_step(step)}")
                except Exception as e:  # noqa: BLE001 -- the steps so far are the useful part
                    done.append(f"{json.dumps(step)} -> FAILED: {e}")
                    break
            time.sleep(0.2)
        if route_capture is not None:
            time.sleep(2)  # a merge's 40 frames and their writing, when it came with the last step
            reply["bursts"] = [summarize_burst(folder)[0] for folder in sorted(p for p in route_capture.iterdir() if p.is_dir())]
    finally:
        if route_capture is not None:
            try:
                bridge.call("capture_events", directory="")
            except BridgeError:
                pass
        route_capture = None
    reply["log"] = filter_log(watcher.lines, grep)
    return reply


@mcp.tool()
def press_key(key: str, hold_ms: int = 80, repeat: int = 1, os_input: bool = False) -> dict[str, Any]:
    """Press a key in the game: handed to the game's own input through the bridge (no focus,
    works on the background desktop). In menus the keys act through the menu-mode control map:
    "e" accepts a message box (Enter isn't mapped there), "esc" or "tab" cancels.

    key: a name from the scan-code table (letters, digits, f1-f12, esc, enter, space, tab, shift,
    ctrl, alt, console, up/down/left/right) or a hex DirectInput scan code like "0x29".
    os_input: OS-level input instead (focuses the game window first); hold_ms applies to it only.
    """
    code = int(key, 16) if key.lower().startswith("0x") else SCAN_CODES.get(key.lower())
    if code is None:
        raise ValueError(f"unknown key '{key}'; known: {', '.join(SCAN_CODES)}")
    if not os_input:
        bridge.call("key", code=code, repeat=repeat)
        time.sleep(0.1 * repeat)
        return {"pressed": key, "repeat": repeat, "via": "bridge"}
    if not focus_game():
        raise BridgeError("could not bring the Skyrim window to the foreground")
    for _ in range(repeat):
        send_scan(code, key_up=False)
        time.sleep(hold_ms / 1000)
        send_scan(code, key_up=True)
        time.sleep(0.1)
    return {"pressed": key, "repeat": repeat}


@mcp.tool()
def wait(seconds: float) -> dict[str, Any]:
    """Let the game run for a while (e.g. to observe NPC movement), then return the state."""
    time.sleep(seconds)
    return bridge.call("state")


# ---- tools: observation -------------------------------------------------------------------


@mcp.tool()
def screenshot(max_width: int = 1280, crop: list[float] | None = None) -> Image:
    """Capture the next presented frame exactly as shown on screen (HUD and menus included).

    crop: [left, top, right, bottom] as fractions of the frame, e.g. [0.3, 0.2, 0.7, 0.9] for a
    doorway in the middle: more detail of the part that matters, in a smaller picture.
    """
    path = Path(tempfile.gettempdir()) / "skyrim-mcp" / "frame.jpg"
    path.parent.mkdir(exist_ok=True)
    path.unlink(missing_ok=True)
    bridge.call("screenshot", path=str(path))
    with PILImage.open(path) as img:
        img = img.convert("RGB")
        if box := crop_box(img.size, crop):
            img = img.crop(box)
        if img.width > max_width:
            img = img.resize((max_width, round(img.height * max_width / img.width)), PILImage.LANCZOS)
        out = io.BytesIO()
        img.save(out, format="JPEG", quality=85)
    return Image(data=out.getvalue(), format="jpeg")


# ---- tools: frame bursts ------------------------------------------------------------------
#
# Much of what goes wrong at a doorway lasts a frame or a few: a closed door, a white flash, the
# low-detail town over the real one. A screenshot takes a second and falls between them, so these
# were reported fixed without having been seen. A burst is every presented frame in a row.

FRAMES_DIR = Path(tempfile.gettempdir()) / "skyrim-mcp" / "frames"
# The plugin starts a new folder under this one at each crossing and merge while a route runs
# with capture (bridge `capture_events`); a route's own {"frames": n} steps land here too.
route_capture: Path | None = None
route_bursts = 0


def fresh_dir(path: Path) -> Path:
    import shutil

    shutil.rmtree(path, ignore_errors=True)
    path.mkdir(parents=True)
    return path


def frames_dir(directory: str) -> Path:
    path = Path(directory)
    return path if path.is_absolute() else FRAMES_DIR / path


def frame_files(directory: Path) -> list[Path]:
    return sorted(directory.glob("frame_*.jpg"))


def start_burst(directory: Path, frames: int) -> int:
    return bridge.call("capture_frames", directory=str(fresh_dir(directory)), frames=frames)["frames"]


def wait_for_burst(directory: Path, frames: int, timeout: float = 20) -> list[Path]:
    """The plugin writes all files after the last frame is taken, so fewer than asked for means
    it is still taking or writing them."""
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        if len(frame_files(directory)) >= frames:
            time.sleep(0.2)  # the last file may still be open
            break
        time.sleep(0.1)
    return frame_files(directory)


def crop_box(size: tuple[int, int], crop: list[float] | None) -> tuple[int, int, int, int] | None:
    if not crop:
        return None
    if len(crop) != 4 or not (0 <= crop[0] < crop[2] <= 1 and 0 <= crop[1] < crop[3] <= 1):
        raise ValueError("crop must be [left, top, right, bottom] as fractions of the frame, e.g. [0.3, 0.2, 0.7, 0.9]")
    return (round(crop[0] * size[0]), round(crop[1] * size[1]), round(crop[2] * size[0]), round(crop[3] * size[1]))


def odd_frame_scores(images: list[PILImage.Image]) -> list[float]:
    """How much each frame stands apart from both its neighbours, beyond what they differ from
    each other (0..1 of full range, averaged over the picture). Steady motion scores about zero
    whatever its speed: the frames before and after then differ more than either does from the
    one between. A frame that shows something for that frame only scores above it."""
    from PIL import ImageChops, ImageStat

    small = [img.convert("L").resize((192, 108), PILImage.BILINEAR) for img in images]

    def apart(a: int, b: int) -> float:
        return ImageStat.Stat(ImageChops.difference(small[a], small[b])).mean[0] / 255

    scores = [0.0] * len(small)
    for i in range(1, len(small) - 1):
        scores[i] = min(apart(i - 1, i), apart(i, i + 1)) - apart(i - 1, i + 1)
    return scores


# Of full range, averaged over the picture: a door-sized area changing for one frame is well above
# this, JPEG noise and steady motion well below.
ODD_FRAME = 0.002


def summarize_burst(directory: Path, crop: list[float] | None = None) -> tuple[dict[str, Any], list[PILImage.Image], list[int], list[int]]:
    files = frame_files(directory)
    if not files:
        return {"directory": str(directory), "frames": 0, "note": "nothing written: a capture_frames call started before this one ended, or no frame was presented"}, [], [], []
    images = []
    for file in files:
        with PILImage.open(file) as img:
            img = img.convert("RGB")
            box = crop_box(img.size, crop)
            images.append(img.crop(box) if box else img)
    scores = odd_frame_scores(images)
    odd = [i for i, score in enumerate(scores) if score > ODD_FRAME]
    ms = [int(file.stem.rsplit("_", 1)[1]) for file in files]
    gaps = [b - a for a, b in zip(ms, ms[1:])]
    summary = {
        "directory": str(directory),
        "frames": len(files),
        "spanMs": ms[-1] - ms[0],
        # A hitch during the burst: which frame came late, and by how much.
        "longestFrame": {"frame": gaps.index(max(gaps)) + 1, "ms": max(gaps)} if gaps else None,
        # Frames that differ from the one before and the one after, which resemble each other.
        "oddFrames": [{"frame": i, "ms": ms[i], "score": round(scores[i], 4), "file": files[i].name} for i in odd],
        "highestScore": round(max(scores), 4),
    }
    return summary, images, odd, ms


def contact_sheet(images: list[PILImage.Image], ms: list[int], odd: list[int], max_width: int = 2000) -> bytes:
    """All frames of a burst in one picture, in order, each labelled with its index and time."""
    from PIL import ImageDraw, ImageFont

    columns = 4 if len(images) <= 16 else 6
    width = max_width // columns
    height = round(width * images[0].height / images[0].width)
    rows = -(-len(images) // columns)
    sheet = PILImage.new("RGB", (columns * width, rows * height))
    draw = ImageDraw.Draw(sheet)
    font = ImageFont.load_default(size=max(14, width // 22))
    for i, img in enumerate(images):
        x, y = (i % columns) * width, (i // columns) * height
        sheet.paste(img.resize((width, height), PILImage.LANCZOS), (x, y))
        label = f"{i:02}  {ms[i]} ms" + ("  ODD" if i in odd else "")
        box = draw.textbbox((x + 6, y + 4), label, font=font)
        draw.rectangle((box[0] - 4, box[1] - 2, box[2] + 4, box[3] + 2), fill=(0, 0, 0))
        draw.text((x + 6, y + 4), label, fill=(255, 80, 80) if i in odd else (255, 255, 255), font=font)
        if i in odd:
            draw.rectangle((x + 1, y + 1, x + width - 2, y + height - 2), outline=(255, 0, 0), width=3)
    out = io.BytesIO()
    sheet.save(out, format="JPEG", quality=85)
    return out.getvalue()


def burst_reply(directory: Path, crop: list[float] | None) -> list[Any]:
    summary, images, odd, ms = summarize_burst(directory, crop)
    if not images:
        return [json.dumps(summary)]
    return [json.dumps(summary), Image(data=contact_sheet(images, ms, odd), format="jpeg")]


@mcp.tool()
def capture_frames(frames: int = 16, crop: list[float] | None = None, name: str = "burst") -> list[Any]:
    """Every one of the next `frames` presented frames (at most 60), as one contact sheet.

    For anything that shows for a frame or a fraction of a second, which a screenshot misses.
    Returns right after the last frame, so start it with the thing to watch already under way
    (walk_to with wait=false), or use run_route with capture or a {"frames": n} step.

    Also returns the frames that stand apart from both their neighbours ("oddFrames": a flash, a
    door drawn closed once), and the folder holding each frame at full size, for Read.
    crop: [left, top, right, bottom] as fractions of the frame, to look at the doorway only; the
    odd-frame scores are then taken from that part alone, which makes them more sensitive.
    """
    directory = FRAMES_DIR / name
    frames = start_burst(directory, frames)
    wait_for_burst(directory, frames)
    return burst_reply(directory, crop)


# A text and a picture: as structured output the picture can't be serialized.
@mcp.tool(structured_output=False)
def frames_sheet(directory: str, crop: list[float] | None = None) -> list[Any]:
    """The contact sheet and odd frames of a burst already taken: a folder that capture_frames or
    run_route returned (absolute, or its name), optionally cropped as in capture_frames."""
    return burst_reply(frames_dir(directory), crop)


@mcp.tool()
def read_log(source: str = "plugin", lines: int = 120) -> dict[str, Any]:
    """Tail of a log: "plugin" (SeamlessSkyrim.log), "skse" (skse64.log) or "crash" (newest Crash Logger report)."""
    if source == "plugin":
        path = PLUGIN_LOG
    elif source == "skse":
        path = SKSE_LOG_DIR / "skse64.log"
    elif source == "crash":
        crashes = sorted(SKSE_LOG_DIR.glob("Crashlogs/*.log"), key=lambda p: p.stat().st_mtime, reverse=True)
        if not crashes:
            return {"path": None, "lines": [], "note": "no crash logs"}
        path = crashes[0]
    else:
        raise ValueError('source must be "plugin", "skse" or "crash"')
    if not path.exists():
        return {"path": str(path), "lines": [], "note": "file does not exist"}
    text = path.read_text("utf-8", "replace").splitlines()
    return {
        "path": str(path),
        "modified": time.strftime("%Y-%m-%d %H:%M:%S", time.localtime(path.stat().st_mtime)),
        "lines": text[-lines:],
    }


# ---- tools: reverse engineering -----------------------------------------------------------

CACHE_DIR = Path(__file__).resolve().parent / ".cache"
_disassembler: retools.Disassembler | None = None


def disassembler() -> retools.Disassembler:
    """Built lazily. Keyed on the exe's size and mtime so a game update rebuilds the id table."""
    global _disassembler
    if _disassembler is None:
        exe = GAME_DIR / GAME_EXE
        stat = exe.stat()
        CACHE_DIR.mkdir(exist_ok=True)
        ids_csv = CACHE_DIR / f"ids-{stat.st_size}-{int(stat.st_mtime)}.csv"
        if not ids_csv.exists():
            # Only the running game knows how to decode its Address Library database.
            bridge.call("re_export_ids", path=str(ids_csv))
        names_json = CACHE_DIR / "names.json"
        if names_json.exists():
            names = {int(k): v for k, v in json.loads(names_json.read_text()).items()}
        else:
            names = retools.build_names()
            names_json.write_text(json.dumps(names))
        _disassembler = retools.Disassembler(retools.Image(exe), retools.IdMap(ids_csv), names)
        _disassembler.files = (exe, ids_csv, names_json)
    return _disassembler


@mcp.tool()
def re_disassemble(target: str, max_instructions: int = 150) -> dict[str, Any]:
    """Disassemble a game function from SkyrimSE.exe (original code, without other mods' hooks).

    target: "id:13316" (AE Address Library id), "0x1A2B3C" (offset from the image base) or a
    CommonLib name such as "TES::SetCurrentCell". Calls and RIP-relative references are labelled
    with the nearest Address Library id and the CommonLib name when known ("+0x.." = inside it).
    Needs the game running once per game version to export the id table.
    """
    d = disassembler()
    rva = d.resolve(target)
    return {"offset": f"0x{rva:X}", "label": d.label(rva), "code": d.disassemble(rva, max_instructions)}


@mcp.tool()
def re_callers(target: str, limit: int = 60) -> dict[str, Any]:
    """Direct call/jmp sites of a function (same target syntax as re_disassemble).

    Indirect calls (vtables, function pointers) are not found.
    """
    d = disassembler()
    rva = d.resolve(target)
    return {"target": d.label(rva), "callers": d.callers(rva, limit)}


@mcp.tool()
def re_call(id: int, args: list[dict[str, Any]] | None = None) -> dict[str, Any]:
    """Call an engine function by AE Address Library id on the game's main thread. EXPERIMENTS ONLY:
    a wrong signature or argument crashes the game (check read_log("crash") afterwards).

    Up to 4 integer/pointer arguments (no floats), each a descriptor:
    {"cell": "portal" | "<formid>"}, {"ref": "<formid>"}, {"node": "portalRoot"}, {"tes": true},
    {"player": true}, {"singleton": <id of a global pointer>}, {"int": n}, {"bool": b}.
    Returns rax/al plus the plugin log lines written during the call.
    """
    return with_log("re_call", id=id, args=args or [])


@mcp.tool()
def re_xrefs(target: str, limit: int = 60) -> dict[str, Any]:
    """Code referencing an address: direct calls/jumps plus RIP-relative loads (lea/mov/cmp),
    e.g. constructors loading a vtable or functions reading a global. Same target syntax as
    re_disassemble; vtables/globals by id ("id:208040") or offset."""
    d = disassembler()
    rva = d.resolve(target)
    # In a process of its own, with a time limit (see the end of retools.py): it takes a second or
    # two, and a call that hangs holds up every other tool of this server.
    try:
        scan = subprocess.run(
            [sys.executable, str(Path(retools.__file__)), *map(str, d.files), str(rva), str(limit)],
            stdin=subprocess.DEVNULL, capture_output=True, text=True, timeout=60,
        )
    except subprocess.TimeoutExpired:
        raise RuntimeError("re_xrefs gave up after 60 s (it normally takes one or two); try again, or use re_callers for direct calls") from None
    if scan.returncode:
        raise RuntimeError(f"re_xrefs failed: {scan.stderr.strip()[-400:]}")
    return {"target": d.label(rva), "xrefs": json.loads(scan.stdout)}


@mcp.tool()
def re_lookup(target: str) -> dict[str, Any]:
    """Offset, Address Library id and CommonLib name for an id/offset/name."""
    d = disassembler()
    rva = d.resolve(target)
    return {"offset": f"0x{rva:X}", "label": d.label(rva)}


# ---- tools: SeamlessSkyrim portals --------------------------------------------------------


@mcp.tool()
def dump_world(grep: str = "") -> dict[str, Any]:
    """Probe: player cell, resident interiors with their state/actors, high-process actors per cell.

    Everything goes to the plugin log; only lines matching grep (a regex, e.g. "lod near|lod double")
    come back. Empty: just the line count.
    """
    return with_log("dump_world", grep=grep)


@mcp.tool()
def portals_enable(enabled: bool = True) -> dict[str, Any]:
    """Switch the door portals on/off for this session (the INI default is on:
    Data/SKSE/Plugins/SeamlessSkyrim.ini, [Portals] bEnabled=0 to disable permanently)."""
    return with_log("portals_enable", enabled=enabled)


@mcp.tool()
def portal_status(keys: list[str] | None = None, grep: str = "") -> dict[str, Any]:
    """Active portal (doors), the remote cell with its NPCs, light scene counts, last frame info.

    keys: only these top-level fields (e.g. ["lod", "merged", "travel"]); None: all of them.
    grep: log lines written meanwhile that match (a regex); empty: none.
    """
    reply = with_log("portal_status", grep=grep)
    if keys and isinstance(reply["result"], dict):
        reply["result"] = {k: v for k, v in reply["result"].items() if k in keys}
    return reply


@mcp.tool()
def portal_render_file(name: str = "portal") -> Image:
    """The active portal's raw render (before compositing into the door), as an image."""
    path = Path(tempfile.gettempdir()) / "skyrim-mcp" / f"{name}.jpg"
    path.parent.mkdir(exist_ok=True)
    path.unlink(missing_ok=True)
    bridge.call("portal_render_file", path=str(path))
    with PILImage.open(path) as img:
        img = img.convert("RGB")
        if img.width > 1600:
            img = img.resize((1600, round(img.height * 1600 / img.width)), PILImage.LANCZOS)
        out = io.BytesIO()
        img.save(out, format="JPEG", quality=85)
    return Image(data=out.getvalue(), format="jpeg")


if __name__ == "__main__":
    mcp.run()
