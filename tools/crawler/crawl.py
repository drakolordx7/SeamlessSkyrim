"""Door crawler: visits load doors of the user's load order in the running game and reports every
door that misbehaves.

Run from the PowerShell tool (the game and MO2 must not be running unless you pass --attach for a
game this harness launched earlier; a game that is not an agent game is the user playing and is
never touched):

    cd D:\\AI\\Active\\SeamlessSkyrim\\tools\\skyrim-mcp
    uv run python ..\\crawler\\crawl.py --preset whiterun --run whiterun
    uv run python ..\\crawler\\crawl.py --list --space WhiterunWorld --kind interior-exterior --max 10
    uv run python ..\\crawler\\crawl.py --run whiterun            # resumes: results.json is the checkpoint
    uv run python ..\\crawler\\crawl.py --run whiterun --report-only

Output: .weaver/scratch/crawl/<run>/{report.md, results.json, shots/, contact-*.jpg}.

One visit: put the player ~6 m in front of the door's outside end looking at it (coc/cow + move_to +
setangle), let the portal settle, picture + portal_status + scene_check + frame times; activate the
door, picture; walk through, check cell and height, pictures; activate the partner door from inside,
walk back out, check again. See classify() for the result classes.
"""

from __future__ import annotations

import argparse
import json
import math
import sys
import time
import traceback
from pathlib import Path
from typing import Any

HERE = Path(__file__).resolve().parent
REPO = HERE.parent.parent
sys.path.insert(0, str(REPO / "tools" / "skyrim-mcp"))
sys.path.insert(0, str(HERE))

import doors as D  # noqa: E402
import imaging  # noqa: E402
import report  # noqa: E402

# Worst first: the class of a door is its most severe problem.
PRIORITY = ["hang", "setup-failed", "wrong-cell", "fall-through", "timeout", "no-portal", "loading-screen", "scene-problem", "visual-suspect"]
TRANSIENT = ("timed out waiting for the game's main thread", "bridge unreachable")


class Hang(Exception):
    """The bridge stopped answering (or the game died) during a door."""


class Foreign(Exception):
    """A game we did not start is running: stop, never touch it."""


class Session:
    def __init__(self, args: argparse.Namespace, census: D.Census, out: Path) -> None:
        import server  # imported late: it reads harness settings and talks to Windows

        self.server = server
        self.args = args
        self.census = census
        self.out = out
        self.ours_pid: int | None = None
        self.blocked = 0.0  # seconds the main thread did not answer, summed over transient retries
        self.hang_s = args.hang_seconds

    # ---- bridge ----------------------------------------------------------------------------

    def call(self, cmd: str, **a: Any) -> Any:
        """A bridge call that waits out loading screens (main thread busy) and raises Hang when the
        game stops answering or is gone. Time spent waiting adds to self.blocked."""
        s = self.server
        started = time.monotonic()
        while True:
            try:
                result = s.bridge.call(cmd, **a)
                self.blocked += time.monotonic() - started
                return result
            except s.BridgeError as e:
                if not any(t in str(e) for t in TRANSIENT):
                    self.blocked += time.monotonic() - started
                    raise
                proc = s.game_process()
                if not proc or proc.pid != self.ours_pid:
                    raise Hang(f"the game is gone ({e})") from e
                if time.monotonic() - started > self.hang_s:
                    raise Hang(f"bridge silent for {self.hang_s:.0f}+ s ({e})") from e
                s.background.apply()
                time.sleep(1)

    def state(self) -> dict[str, Any]:
        return self.call("state")

    def console(self, command: str) -> None:
        self.call("console", command=command)

    def wait_in_game(self, timeout: float = 150) -> dict[str, Any]:
        deadline = time.monotonic() + timeout
        good = 0
        while time.monotonic() < deadline:
            self.server.background.apply()
            st = self.state()
            if st.get("inGame") and not st.get("loading"):
                good += 1
                if good >= 2:
                    return st
            else:
                good = 0
            time.sleep(0.7)
        raise Hang(f"not back in the game after {timeout:.0f} s")

    # ---- game lifecycle --------------------------------------------------------------------

    def ensure_game(self) -> None:
        s = self.server
        proc = s.game_process()
        if proc and proc.pid != self.ours_pid:
            if not self.args.attach:
                raise Foreign("SkyrimSE.exe is running and this crawl did not start it (use --attach only for an agent game left by an earlier crawl)")
            s.agent_game()  # raises unless it was launched for an agent
            self.ours_pid = proc.pid
            self.prepare(god_mode=False)  # tgm toggles: an earlier crawl of ours already set it
            return
        if proc:
            return
        if s.mo2_running():
            raise Foreign("Mod Organizer 2 is open: close it (a game it starts has no agent bridge)")
        print("launching the game ...", flush=True)
        s.game_launch(save=self.args.save, timeout_seconds=420, background_mode=True)
        proc = s.game_process()
        if not proc:
            raise Hang("game did not stay up after launch")
        self.ours_pid = proc.pid
        self.prepare()

    def prepare(self, god_mode: bool = True) -> None:
        """Same conditions for every picture: noon, frozen clock, first person, god mode, portals on."""
        self.wait_in_game(200)
        for command in (["tgm"] if god_mode else []) + ["set gamehour to 13", "set timescale to 0"]:
            self.console(command)
        self.call("camera_view", firstPerson=True)
        status = self.call("portals_enable", enabled=True)
        if not status.get("enabled"):
            raise RuntimeError("portals could not be enabled in this build")

    def kill_ours(self) -> None:
        """End the game this crawl started, and only that one."""
        s = self.server
        proc = s.game_process()
        if proc and proc.pid == self.ours_pid:
            try:
                proc.kill()
                proc.wait(20)
            except Exception as e:  # psutil errors: already gone
                print(f"kill: {e}", flush=True)
        s.bridge.close()
        s.restore_mo2_profile(s.launched_selected_profile)
        self.ours_pid = None

    def quit_game(self) -> None:
        s = self.server
        proc = s.game_process()
        if not proc or proc.pid != self.ours_pid:
            return
        try:
            s.bridge.call("console", command="qqq")
            proc.wait(timeout=30)
            s.bridge.close()
            s.restore_mo2_profile(s.launched_selected_profile)
            self.ours_pid = None
        except Exception:
            self.kill_ours()

    # ---- pictures --------------------------------------------------------------------------

    def snap(self, rec: dict[str, Any], stage: str):
        """Screenshot into shots/<door>/<stage>.jpg; returns the PIL image, or None when it failed."""
        from PIL import Image

        path = self.out / "shots" / rec["dir"] / f"{stage}.jpg"
        try:
            self.server.take_picture(path, max_width=960)
        except self.server.BridgeError as e:
            rec["notes"].append(f"screenshot {stage} failed: {e}")
            return None
        rec["shots"][stage] = path.relative_to(self.out).as_posix()
        return Image.open(path).convert("RGB")


# ---- one door ------------------------------------------------------------------------------------


def parse_form(text: str) -> int:
    return int(text, 16) if text else 0


def dist3(a: list[float], b: tuple[float, float, float]) -> float:
    return math.sqrt(sum((x - y) ** 2 for x, y in zip(a, b)))


def go_front(S: Session, door: dict[str, Any], front: dict[str, Any]) -> None:
    c = S.census
    at = door["at"]
    state = S.state()
    player = state["player"]
    cell = player["cell"]
    exterior = D.is_exterior(door)
    fx, fy, fz = front["pos"]
    z = fz + (150 if exterior else 48)
    if exterior:
        world = c.space_form_id(at["spaceId"])
        reload = cell["interior"] or parse_form(player.get("worldspace", "")) != world or dist3(player["pos"], (fx, fy, z)) > 4800
        command = f"cow {c.space_edid(at['spaceId'])} {D.grid_of(door['pos'])[0]} {D.grid_of(door['pos'])[1]}"
    else:
        reload = not (cell["interior"] and cell["editorId"] == c.space_edid(at["spaceId"]))
        command = f"coc {c.space_edid(at['spaceId'])}"
    if reload:
        S.console(command)
        time.sleep(3)
        S.wait_in_game(200)
    S.call("move_to", pos=[fx, fy, z])
    face(S, -front["dir"][0], -front["dir"][1])
    time.sleep(1.0)


def face(S: Session, dx: float, dy: float) -> float:
    heading = D.heading_deg(dx, dy)
    S.console(f"player.setangle z {heading:.2f}")
    S.console("player.setangle x 0")
    return heading


def portal_summary(S: Session) -> dict[str, Any]:
    """What the plugin shows for doors: the one merged portal (active), doorways drawn as stand-in
    pictures, rooms shown live beside doors, the doors that were used and are open."""
    st = S.call("portal_status")
    active = st.get("active")
    rooms = (st.get("shownRooms") or {}).get("beside") or []
    return {
        "enabled": st.get("enabled"), "merged": st.get("merged"),
        "active": {"sourceDoor": active["sourceDoor"], "linkedDoor": active["linkedDoor"], "mapping": active.get("mapping")} if active else None,
        "standIns": [{k: s[k] for k in ("door", "swing", "fade", "cameThrough")} for s in st.get("standIns") or []],
        "rooms": [{"door": r["door"], "cell": r["cell"].split("'")[1] if "'" in r["cell"] else r["cell"], "shapes": r.get("shapes"), "seen": r.get("seen"), "paused": r.get("paused")} for r in rooms],
        "used": [u["door"] for u in (st.get("doorUse") or {}).get("used", [])] + [u["linked"] for u in (st.get("doorUse") or {}).get("used", [])],
        "pictures": st.get("doorwayPictures"), "lastEvent": st.get("lastEvent"), "lod": st.get("lod"), "travel": st.get("travel"),
    }


def wait_portal(S: Session, timeout: float, ref: str, linked: str) -> tuple[dict[str, Any], float]:
    """Waits until the plugin shows this door (the manager looks at its doors every few frames)."""
    t0 = time.monotonic()
    status = portal_summary(S)
    while time.monotonic() - t0 < timeout and not is_mine(status, ref, linked):
        time.sleep(1.0)
        status = portal_summary(S)
    return status, time.monotonic() - t0


def is_mine(status: dict[str, Any], ref: str, linked: str | None = None) -> bool:
    """The plugin shows this door (or its partner): as the merged portal, a stand-in doorway or a live room."""
    ids = {ref, linked} - {None}
    active = status.get("active")
    shown = {s["door"] for s in status["standIns"]} | {r["door"] for r in status["rooms"]}
    if active:
        shown |= {active["sourceDoor"], active["linkedDoor"]}
    return bool(ids & shown)


def use_door(S: Session, rec: dict[str, Any], ref: str, linked: str, label: str) -> bool:
    """Activate `ref` and check at once that the plugin counts it as used (open). Returns True when
    the use blocked the game or a loading screen showed (vanilla loading: nothing seamless about it)."""
    before = S.blocked
    result = S.call("activate", ref=ref)
    rec["activate"][label] = result
    # The plugin registers the use a frame or two later (its own thread): poll for it.
    saw_loading, counted = False, False
    deadline = time.monotonic() + 3.0
    while time.monotonic() < deadline:
        time.sleep(0.3)
        if S.state().get("loading"):
            saw_loading = True
            S.wait_in_game(200)
            break
        if {ref, linked} & set(portal_summary(S)["used"]):
            counted = True
            break
    # Observation only: the bridge's activate does not always register as a use of the door (doorUse
    # stays empty for some doors that then cross fine), so it can't be a verdict.
    rec["activate"][label] = {**result, "counted": counted}
    blocked = S.blocked - before
    if saw_loading or blocked >= 3:
        rec["problems"].append(("loading-screen", f"{label}: the door's use blocked the game for {blocked:.1f} s" + (" with a loading screen" if saw_loading else " (loading screen or hitch)")))
        return True
    return False


def walk_once(S: Session, target: tuple[float, float]) -> dict[str, Any]:
    S.call("walk_to", pos=[target[0], target[1]], stopDistance=40, run=False, timeoutSeconds=25)
    deadline = time.monotonic() + 35
    while time.monotonic() < deadline:
        time.sleep(0.3)
        walk = S.call("state")["walk"]
        if not walk["active"]:
            return walk
    S.call("walk_stop")
    return {"status": "harness-timeout"}


def walk_through(S: Session, rec: dict[str, Any], target: tuple[float, float], label: str) -> dict[str, Any]:
    """Walk to a point past the door. Stopped at the door (stuck) with the crosshair on it: press the
    activate key as a player would (bridge use_pick) and walk again once."""
    before = S.blocked
    walk = walk_once(S, target)
    crossed = ("travelled through a door", "arrived")
    first = walk.get("status")
    if first not in crossed:
        here = S.state()["player"]["pos"]
        rec["walks"][label + "-first"] = {"status": first, "stoppedAt": [round(v) for v in here], "distanceToTarget": round(math.hypot(here[0] - target[0], here[1] - target[1]))}
        S.snap(rec, f"{label}-stuck")
        pick = S.call("use_pick")
        rec["walks"][label + "-first"]["usePick"] = pick
        if pick.get("target"):
            time.sleep(2.0)
            walk = walk_once(S, target)
            if walk.get("status") in crossed:
                rec["notes"].append(f"{label}: the walk stopped at the closed door (activate through the bridge had not opened it); the crosshair use (use_pick, target {pick['target']}) opened it")
    blocked = S.blocked - before
    rec["walks"][label] = {"status": walk.get("status"), "blockedS": round(blocked, 1)}
    if blocked >= 3 and not any(p[0] == "loading-screen" and p[1].startswith(label) for p in rec["problems"]):
        rec["problems"].append(("loading-screen", f"{label}: walking through blocked the game for {blocked:.1f} s (loading screen or hitch)"))
    S.wait_in_game(200)
    time.sleep(2.5)
    return walk


def where_am_i(S: Session, expect_kind: str, expect_edid: str, expect_world: int, expect_z: float) -> dict[str, Any]:
    st = S.state()
    player = st["player"]
    cell = player["cell"]
    if expect_kind == "interior":
        cell_ok = bool(cell["interior"]) and cell["editorId"] == expect_edid
    else:
        cell_ok = (not cell["interior"]) and parse_form(player.get("worldspace", "")) == expect_world
    ground = S.call("ray_down")
    time.sleep(1.2)
    z = S.state()["player"]["pos"][2]
    out = {
        "cell": cell["editorId"], "interior": cell["interior"], "worldspace": player.get("worldspace"), "z": round(z, 1),
        "expectedZ": round(expect_z, 1), "dz": round(z - expect_z, 1), "cellOk": cell_ok,
        "ground": ground.get("z") if ground.get("hit") else None, "groundHit": bool(ground.get("hit")),
    }
    return out


def judge_arrival(rec: dict[str, Any], label: str, here: dict[str, Any], source_cell_ok: bool, walk: dict[str, Any]) -> None:
    if not here["cellOk"]:
        if source_cell_ok:
            rec["problems"].append(("timeout", f"{label}: still where it started after the walk ({walk.get('status')})"))
        else:
            rec["problems"].append(("wrong-cell", f"{label}: ended in {here['cell']} (interior={here['interior']}, worldspace {here['worldspace']})"))
        return
    if not here["groundHit"] or here["dz"] < -300:
        rec["problems"].append(("fall-through", f"{label}: z {here['z']} vs marker {here['expectedZ']} (dz {here['dz']}), ground under the player: {here['ground']}"))
    elif here["dz"] > 600:
        rec["problems"].append(("wrong-cell", f"{label}: z {here['z']} is {here['dz']} above the marker"))


def test_door(S: Session, door: dict[str, Any], index: int) -> dict[str, Any]:
    c = S.census
    partner = c.partner(door)
    front = D.front_of(c, door)
    dest = door["dest"]
    dest_kind = "worldspace" if dest["at"]["kind"] == "worldspace" else "interior"
    src_edid, dest_edid = c.space_edid(door["at"]["spaceId"]), c.space_edid(dest["at"]["spaceId"])
    plugin, local = door["id"].rsplit(":", 1)
    rec: dict[str, Any] = {
        "index": index, "id": door["id"], "runtime": c.hex_id(door["id"]), "dir": f"{plugin.replace('.', '_')}-{local}",
        "base": door["base"]["editorId"], "kind": door["kind"], "origin": door["origin"], "place": c.space_label(door["at"]["spaceId"]),
        "dest": c.space_label(dest["at"]["spaceId"]), "pos": door["pos"], "approachGuess": front["guess"],
        "scripts": door["scripts"], "locked": bool(door.get("lock")), "shots": {}, "walks": {}, "activate": {}, "checks": {},
        "problems": [], "notes": [], "flags": [],
    }
    started = time.monotonic()
    if door["initiallyDisabled"]:
        rec["result"], rec["notes"] = "skipped", ["initially disabled in the plugin: nothing to see"]
        return rec
    if front["guess"]:
        rec["notes"].append("partner door does not point back (census link " + door["link"] + "): approach side guessed from the door's rotation")

    with S.server.LogWatcher() as log:
        visit(S, rec, door, partner, front, dest_kind, src_edid, dest_edid)
    rec["log"] = S.server.filter_log(log.lines, r"(?i)fallback|vanilla|assert|exception|fail|error", limit=8, width=300)
    rec["secs"] = round(time.monotonic() - started, 1)
    return rec


def visit(S: Session, rec: dict[str, Any], door: dict[str, Any], partner: dict[str, Any] | None, front: dict[str, Any],
          dest_kind: str, src_edid: str, dest_edid: str) -> None:
    c = S.census
    dest = door["dest"]
    ref = c.hex_id(door["id"])
    linked = c.hex_id(dest["doorId"])
    S.server.background.remember_user_window()
    S.server.background.guard(120)
    go_front(S, door, front)

    # The door must be there: a wrong run-time id (load order differs from the census) shows up here.
    near = S.call("refs_near", radius=1200, kind="doors", limit=40)
    mine = next((r for r in near["refs"] if r["id"] == ref), None)
    if not mine:
        rec["problems"].append(("setup-failed", f"door {ref} is not among the {near['count']} doors loaded within 1200 units of the spot in front of it"))
        rec["result"] = "setup-failed"
        return
    rec["locked"] = bool(mine.get("locked"))

    heading = face(S, -front["dir"][0], -front["dir"][1])
    status, settled = wait_portal(S, S.args.portal_wait, ref, linked)
    rec["checks"]["front"] = {"settleS": round(settled, 1), "portal": status}
    eye = S.call("camera")["camera"]
    front_img = S.snap(rec, "front")
    rec["checks"]["front"]["frame"] = S.call("frame_stats")
    rec["checks"]["front"]["scene"] = S.call("scene_check", radius=3000)
    if not is_mine(status, ref, linked):
        rec["problems"].append(("no-portal", f"no portal for this door after {settled:.0f} s at 6 m from it (shown: {sorted({x['door'] for x in status['standIns']} | {x['door'] for x in status['rooms']})}, merged portal {(status['active'] or {}).get('sourceDoor')})"))
    if not rec["checks"]["front"]["scene"].get("ok", True):
        rec["problems"].append(("scene-problem", "scene_check at the door: " + "; ".join(scene_problems(rec["checks"]["front"]["scene"])[:3])))

    active = status["active"]
    half = active["mapping"].get("halfWidth") if active and ref in (active["sourceDoor"], active["linkedDoor"]) and isinstance(active.get("mapping"), dict) else None
    box = None
    if front_img is not None:
        box = imaging.doorway_box(front_img.size, eye, door["pos"], heading, half or 70.0, vfov_deg=S.args.vfov)
        if box:
            rec["checks"]["front"]["doorway"] = imaging.region_flags(front_img, box)
            rec["flags"] += [f"front:{f}" for f in rec["checks"]["front"]["doorway"]["flags"]]

    if rec["locked"]:
        rec["notes"].append("locked: looked at, not entered")
        return

    # Open it.
    vanilla = use_door(S, rec, ref, linked, "open")
    time.sleep(1.5)
    status, settled = wait_portal(S, S.args.portal_wait, ref, linked)
    open_img = S.snap(rec, "open")
    rec["checks"]["open"] = {"settleS": round(settled, 1), "portal": status, "frame": S.call("frame_stats")}
    if not vanilla:
        if not is_mine(status, ref, linked):
            rec["problems"].append(("no-portal", "the plugin no longer shows the door after opening it"))
        elif not rec["activate"]["open"]["counted"]:
            rec["notes"].append("activate was not counted as a use of the door by the plugin (doorUse empty)")
    if open_img is not None and box:
        rec["checks"]["open"]["doorway"] = imaging.region_flags(open_img, box)
        rec["flags"] += [f"open:{f}" for f in rec["checks"]["open"]["doorway"]["flags"]]
        if front_img is not None:
            change = imaging.region_change(front_img, open_img, box)
            rec["checks"]["open"]["regionChange"] = round(change, 1)
            if change < 2.0 and not vanilla:
                rec["flags"].append("open:no-visible-change")

    # Cross in.
    walk = {"status": "vanilla load"}
    if not vanilla:
        target = D.beyond_of(door["pos"], -front["dir"][0], -front["dir"][1])
        walk = walk_through(S, rec, target, "in")
    here = where_am_i(S, dest_kind, dest_edid, c.space_form_id(dest["at"]["spaceId"]), dest["landPos"][2])
    rec["checks"]["in"] = here
    if D.is_exterior(door):
        src_ok = not here["interior"] and parse_form(here["worldspace"] or "") == c.space_form_id(door["at"]["spaceId"])
    else:
        src_ok = here["cell"] == src_edid
    judge_arrival(rec, "in", here, src_ok, walk)
    S.snap(rec, "in")
    rec["checks"]["in"]["frame"] = S.call("frame_stats")
    rec["checks"]["in"]["scene"] = S.call("scene_check", radius=3000)
    if not rec["checks"]["in"]["scene"].get("ok", True):
        rec["problems"].append(("scene-problem", "scene_check inside: " + "; ".join(scene_problems(rec["checks"]["in"]["scene"])[:3])))
    if not here["cellOk"]:
        return  # nothing sensible to go back through

    # Look back at the door we came through, then leave by it.
    if partner is None or partner["dest"]["doorId"] != door["id"]:
        rec["notes"].append("no mutual partner door in the census: the way back was not tested")
        return
    P = partner
    pos = S.state()["player"]["pos"]
    face(S, P["pos"][0] - pos[0], P["pos"][1] - pos[1])
    time.sleep(1.5)
    S.snap(rec, "in-back")
    p_ref = c.hex_id(P["id"])
    vanilla_back = use_door(S, rec, p_ref, ref, "back")
    time.sleep(1.5)
    S.snap(rec, "back-open")
    walk = {"status": "vanilla load"}
    if not vanilla_back:
        out_dir = D.unit(P["pos"][0] - door["dest"]["landPos"][0], P["pos"][1] - door["dest"]["landPos"][1])
        target = D.beyond_of(P["pos"], out_dir[0], out_dir[1])
        walk = walk_through(S, rec, target, "out")
    src_kind = "worldspace" if D.is_exterior(door) else "interior"
    here = where_am_i(S, src_kind, src_edid, c.space_form_id(door["at"]["spaceId"]), front["landZ"])
    rec["checks"]["out"] = here
    judge_arrival(rec, "out", here, False, walk)
    S.snap(rec, "out")
    rec["checks"]["out"]["frame"] = S.call("frame_stats")


def scene_problems(scene: dict[str, Any]) -> list[str]:
    out = []
    for part in ("here", "merged"):
        if isinstance(scene.get(part), dict):
            out += [f"{part}: {p}" for p in scene[part].get("problems", [])]
    return out or ["scene_check not ok"]


def classify(rec: dict[str, Any]) -> str:
    if rec.get("result") in ("skipped", "hang"):
        return rec["result"]
    classes = {p[0] for p in rec["problems"]}
    if any(f.endswith(("black-doorway", "sky-doorway", "no-visible-change")) for f in rec["flags"]):
        classes.add("visual-suspect")
    for cls in PRIORITY:
        if cls in classes:
            return cls
    return "ok"


# ---- run ----------------------------------------------------------------------------------------


def save_results(out: Path, config: dict[str, Any], results: list[dict[str, Any]]) -> None:
    tmp = out / "results.json.tmp"
    tmp.write_text(json.dumps({"config": config, "doors": results}, indent=1, default=str), "utf-8")
    tmp.replace(out / "results.json")


def build_selection(args: argparse.Namespace, census: D.Census) -> list[dict[str, Any]]:
    spaces, near = list(args.space or []), [D.parse_near(census, n) for n in args.near or []]
    if args.preset:
        preset = D.PRESETS[args.preset]
        spaces += preset.get("space", [])
        near += [D.parse_near(census, f"{w}:{where}:{r}") for w, where, r in preset.get("near", [])]
    ids = set(args.id or [])
    selected = D.select(census, spaces, near, args.kind or [], args.ends == "both", None, args.origin)
    if ids:
        selected = [d for d in selected if d["id"] in ids]
    return selected[: args.max] if args.max else selected


def recover(S: Session, rec: dict[str, Any], why: str) -> None:
    """Record the hang, keep the plugin log's tail, kill our game (only ours) and start a fresh one."""
    s = S.server
    rec["result"] = "hang"
    rec["problems"].append(("hang", why))
    try:
        tail = s.PLUGIN_LOG.read_text("utf-8", "replace").splitlines()[-60:] if s.PLUGIN_LOG.exists() else []
        (S.out / f"hang-{rec['dir']}.log").write_text("\n".join(tail), "utf-8")
        rec["notes"].append(f"plugin log tail: hang-{rec['dir']}.log; see read_log('crash') for a crash dump")
    except OSError:
        pass
    S.kill_ours()
    S.ensure_game()


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--census", type=Path, default=REPO / ".weaver" / "scratch" / "census" / "census.json")
    ap.add_argument("--run", default=time.strftime("run-%Y%m%d-%H%M%S"), help="folder name under .weaver/scratch/crawl; an existing one resumes")
    ap.add_argument("--out-root", type=Path, default=REPO / ".weaver" / "scratch" / "crawl")
    ap.add_argument("--preset", choices=sorted(D.PRESETS))
    ap.add_argument("--space", action="append", help="worldspace or interior editor id (repeatable): doors in it")
    ap.add_argument("--near", action="append", help="'Tamriel:x,y:radius' or 'Tamriel:door:Skyrim.esm:01B1F1:radius' (repeatable)")
    ap.add_argument("--kind", action="append", choices=["interior-exterior", "interior-interior", "exterior-exterior", "unknown"])
    ap.add_argument("--origin", action="append", choices=["base", "dlc", "mod", "creation"])
    ap.add_argument("--id", action="append", help="only these census door ids")
    ap.add_argument("--ends", choices=["outside", "both"], default="outside", help="one visit per link from its outside end (default) or from every end")
    ap.add_argument("--max", type=int)
    ap.add_argument("--save", default="SeamlessTestBase", help="save to start the game from")
    ap.add_argument("--attach", action="store_true", help="use an agent game left running by an earlier crawl")
    ap.add_argument("--list", action="store_true", help="print the visit plan and stop")
    ap.add_argument("--report-only", action="store_true")
    ap.add_argument("--keep-game", action="store_true", help="leave the game running at the end")
    ap.add_argument("--portal-wait", type=float, default=10.0, help="seconds to wait for the plugin to show the door")
    ap.add_argument("--hang-seconds", type=float, default=180.0, help="bridge silence after which the game counts as hung")
    ap.add_argument("--vfov", type=float, default=65.0, help="assumed vertical field of view for the doorway box")
    args = ap.parse_args()

    out = args.out_root / args.run
    census = D.load_census(args.census)
    config = {k: (str(v) if isinstance(v, Path) else v) for k, v in vars(args).items()}
    results: list[dict[str, Any]] = []
    if (out / "results.json").exists():
        results = json.loads((out / "results.json").read_text("utf-8"))["doors"]
    if args.report_only:
        report.write(out, config, results, census)
        print(f"report: {out / 'report.md'}")
        return 0

    selected = build_selection(args, census)
    if args.list or not selected:
        for i, d in enumerate(selected, 1):
            print(f"{i:4} {d['id']:<28} {d['base']['editorId']:<26} {census.space_label(d['at']['spaceId'])} -> {census.space_label(d['dest']['at']['spaceId'])}")
        print(f"{len(selected)} visits")
        return 0

    out.mkdir(parents=True, exist_ok=True)
    done = {r["id"] for r in results}
    todo = [d for d in selected if d["id"] not in done]
    print(f"{len(selected)} doors selected, {len(done)} already done, {len(todo)} to go -> {out}", flush=True)

    S = Session(args, census, out)
    consecutive_hangs = 0
    try:
        S.ensure_game()
        for n, door in enumerate(todo, len(done) + 1):
            print(f"[{n}/{len(selected)}] {door['id']} {door['base']['editorId']} {census.space_label(door['at']['spaceId'])} -> {census.space_label(door['dest']['at']['spaceId'])}", flush=True)
            S.ensure_game()
            rec: dict[str, Any] | None = None
            try:
                rec = test_door(S, door, n)
                rec["result"] = classify(rec) if rec.get("result") != "setup-failed" else "setup-failed"
                consecutive_hangs = 0
            except Hang as e:
                rec = {"index": n, "id": door["id"], "runtime": census.hex_id(door["id"]), "base": door["base"]["editorId"], "kind": door["kind"],
                       "dir": f"{door['id'].replace(':', '-').replace('.', '_')}", "place": census.space_label(door["at"]["spaceId"]),
                       "dest": census.space_label(door["dest"]["at"]["spaceId"]), "pos": door["pos"], "shots": {}, "walks": {}, "activate": {},
                       "checks": {}, "problems": [], "notes": [], "flags": []}
                print(f"  HANG: {e}", flush=True)
                recover(S, rec, str(e))
                consecutive_hangs += 1
            except Exception as e:  # a harness or bridge error on this door: record it and go on
                if isinstance(e, Foreign):
                    raise
                traceback.print_exc()
                rec = {"index": n, "id": door["id"], "runtime": census.hex_id(door["id"]), "base": door["base"]["editorId"], "kind": door["kind"],
                       "dir": f"{door['id'].replace(':', '-').replace('.', '_')}", "place": census.space_label(door["at"]["spaceId"]),
                       "dest": census.space_label(door["dest"]["at"]["spaceId"]), "pos": door["pos"], "shots": {}, "walks": {}, "activate": {},
                       "checks": {}, "problems": [("setup-failed", f"{type(e).__name__}: {e}")], "notes": [], "flags": [], "result": "setup-failed"}
            results.append(rec)
            print(f"  -> {rec['result']}" + ("  " + "; ".join(p[1] for p in rec["problems"][:2]) if rec["problems"] else ""), flush=True)
            save_results(out, config, results)
            if n % 5 == 0:
                report.write(out, config, results, census)
            if consecutive_hangs >= 3:
                print("three hangs in a row: stopping (rerun to resume)", flush=True)
                break
    except Foreign as e:
        print(f"STOP: {e}", flush=True)
    finally:
        save_results(out, config, results)
        report.write(out, config, results, census)
        if not args.keep_game:
            try:
                S.quit_game()
            except Exception as e:
                print(f"quit: {e}", flush=True)
    print(f"report: {out / 'report.md'}", flush=True)
    return 0


if __name__ == "__main__":
    sys.exit(main())
