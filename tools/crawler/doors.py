"""Door census loading, filtering, ordering and the geometry of one visit.

The census (tools/census, Mutagen, offline) lists every placed teleport door with both ends. A
crawl visits a door from its "outside end" (the exterior end of an interior-exterior pair; for
the other kinds the end that sorts first) and goes in and back out, so one visit covers both
ends of the link.
"""

from __future__ import annotations

import json
import math
import struct
from dataclasses import dataclass, field
from pathlib import Path
from typing import Any

# How far in front of the door the player is put, and how far past it the walk aims.
FRONT_DISTANCE = 420.0  # ~6 m (1 unit = ~1.4 cm)
BEYOND_DISTANCE = 260.0

# Named filters. "space": editor ids of worldspaces/interiors whose doors are in; "near": [worldspace
# editor id, "door:<census id>" or "x,y", radius].
PRESETS: dict[str, dict[str, Any]] = {
    "whiterun": {
        "space": ["WhiterunWorld"],
        "near": [["Tamriel", "door:Skyrim.esm:01B1F1", 4000]],
    },
    # The other cities: the city worldspace plus the Tamriel doors around its gate (positions from the census).
    "solitude": {
        "space": ["SolitudeWorld"],
        "near": [["Tamriel", "door:Skyrim.esm:101D9E", 4000]],
    },
    "windhelm": {
        "space": ["WindhelmWorld"],
        "near": [["Tamriel", "door:Skyrim.esm:055FCB", 4000]],
    },
    # Riften has two gate pairs (the north gate and the south one) and a third pair a mod (cutting room floor) added.
    "riften": {
        "space": ["RiftenWorld"],
        "near": [["Tamriel", "door:Skyrim.esm:078EDB", 4000], ["Tamriel", "door:Skyrim.esm:042286", 4500]],
    },
    "markarth": {
        "space": ["MarkarthWorld"],
        "near": [["Tamriel", "door:Skyrim.esm:01C389", 4000]],
    },
    # Fort Dawnguard (Dayspring Canyon is its own worldspace, entered from the gate in Tamriel).
    "dawnguard": {
        "space": ["DLC1HunterHQWorld", "DLC1DawnguardHQ01"],
        "near": [["Tamriel", "door:Dawnguard.esm:0110B5", 4000]],
    },
    # Castle Volkihar: the courtyard worldspace and the castle's doors on its island in Tamriel.
    "volkihar": {
        "space": ["DLC1VampireCastleCourtyard", "DLC1VampireCastleGuildhall"],
        "near": [["Tamriel", "-172500,150700", 6000]],
    },
    # Raven Rock is part of Solstheim's worldspace: the doors of the town and the Bulwark around it.
    "ravenrock": {
        "near": [["DLC2SolstheimWorld", "30500,35000", 7000]],
    },
    # Interiors and caves: a spread of doors that no city crawl reaches. Each group is {"space": [...], "kind": [...]}
    # (kind: of the doors in those spaces; none: every kind); one visit per link, started from the first space's side.
    "interiors": {
        "groups": [
            # Inner doors of Dragonsreach, Understone Keep and a ruin of several cells (Bleak Falls Barrow).
            {"space": ["WhiterunDragonsreach", "WhiterunDragonsreachBasement", "WhiterunDragonsreachJarlsQuarters"], "kind": ["interior-interior"]},
            {"space": ["MarkarthUnderStoneKeep"], "kind": ["interior-interior"]},
            {"space": ["BleakFallsBarrow01", "BleakFallsBarrow02"], "kind": ["interior-interior"]},
            # Caves and ruins with load doors between their cells.
            {"space": ["TolvaldsCave01", "TolvaldsCave02", "TolvaldsCave03"], "kind": ["interior-interior"]},
            {"space": ["ForsakenCave01", "ShimmermistCave01", "FellglowKeep02"], "kind": ["interior-interior"]},
            # Blackreach: its doors to the interior cells (and its lifts), and the one interior-interior pair there.
            {"space": ["Blackreach", "BlackreachZcell04"]},
            # Cave worldspaces.
            {"space": ["BlindCliffCaveWorld", "FallowstoneCaveWorldStart", "FallowstoneCaveWorldEnd"]},
        ],
    },
    # The smaller towns: Tamriel doors around a point in each.
    "towns": {
        "near": [
            ["Tamriel", "22000,-46000", 3500],      # Riverwood
            ["Tamriel", "-31500,-87000", 4500],     # Falkreath
            ["Tamriel", "31500,104500", 6000],      # Dawnstar
            ["Tamriel", "-37500,65000", 4500],      # Morthal
            ["Tamriel", "109500,102000", 3000],     # Winterhold
            ["Tamriel", "115500,111500", 3500],     # College of Winterhold
            ["Tamriel", "-84000,8000", 3500],       # Rorikstead
            ["Tamriel", "76500,-64000", 3500],      # Ivarstead
            ["Tamriel", "-102000,84000", 3500],     # Dragon Bridge
            ["Tamriel", "-133500,39500", 4000],     # Karthwasten
        ],
    },
}


@dataclass
class Census:
    doors: list[dict[str, Any]]
    by_id: dict[str, dict[str, Any]]
    spaces: dict[str, dict[str, Any]]
    runtime: dict[str, int] = field(default_factory=dict)  # plugin name (lower case) -> high bits of its form ids

    # ---- ids -------------------------------------------------------------------------------

    def form_id(self, census_id: str) -> int:
        """Run-time FormID of "Plugin.esp:00ABCD" in this load order."""
        plugin, local = census_id.rsplit(":", 1)
        base = self.runtime[plugin.lower()]
        value = int(local, 16)
        return base | (value & 0xFFF if base >> 24 == 0xFE else value & 0xFFFFFF)

    def hex_id(self, census_id: str) -> str:
        return f"{self.form_id(census_id):08X}"

    def space_form_id(self, space_id: str) -> int:
        """"worldspace:Skyrim.esm:01A26F" / "interior:..." -> run-time FormID."""
        return self.form_id(space_id.split(":", 1)[1])

    # ---- spaces ----------------------------------------------------------------------------

    def space_edid(self, space_id: str) -> str:
        return self.spaces.get(space_id, {}).get("editorId", "") or space_id

    def space_label(self, space_id: str) -> str:
        space = self.spaces.get(space_id, {})
        edid = space.get("editorId") or space_id
        name = space.get("name")
        return f"{edid} ({name})" if name and name != edid else edid

    def partner(self, door: dict[str, Any]) -> dict[str, Any] | None:
        return self.by_id.get(door["dest"]["doorId"])


def plugin_is_light(path: str | None, name: str) -> bool:
    """ESL flag (0x200) of the plugin's header record; a .esl file is always light."""
    if name.lower().endswith(".esl"):
        return True
    try:
        with open(path or "", "rb") as f:
            head = f.read(12)
        return head[:4] == b"TES4" and bool(struct.unpack("<I", head[8:12])[0] & 0x200)
    except OSError:
        return False


def load_census(path: Path) -> Census:
    data = json.loads(path.read_text("utf-8"))
    runtime: dict[str, int] = {}
    full, light = 0, 0
    for plugin in sorted((p for p in data["plugins"] if p.get("loaded")), key=lambda p: p["index"]):
        if plugin_is_light(plugin.get("path"), plugin["name"]):
            runtime[plugin["name"].lower()] = 0xFE000000 | (light << 12)
            light += 1
        else:
            runtime[plugin["name"].lower()] = full << 24
            full += 1
    doors = data["doors"]
    return Census(doors, {d["id"]: d for d in doors}, {s["id"]: s for s in data["spaces"]}, runtime)


# ---- selection ------------------------------------------------------------------------------


def is_exterior(door: dict[str, Any]) -> bool:
    return door["at"]["kind"] == "worldspace"


def outside_ends(census: Census, doors: list[dict[str, Any]], both: bool) -> list[dict[str, Any]]:
    """One end per link: the exterior end of interior-exterior pairs, else the end that sorts first
    among those given (both=True: every end given)."""
    if both:
        return list(doors)
    chosen: dict[frozenset[str], dict[str, Any]] = {}
    for door in doors:
        partner = census.partner(door)
        key = frozenset((door["id"], partner["id"] if partner else door["id"]))
        have = chosen.get(key)
        if not have:
            chosen[key] = door
            continue
        # Prefer the exterior end, then the smaller id.
        if (is_exterior(door), door["id"]) < (is_exterior(have), have["id"]) and not (is_exterior(door) and not is_exterior(have)):
            chosen[key] = door
        elif is_exterior(door) and not is_exterior(have):
            chosen[key] = door
    return list(chosen.values())


def select(census: Census, spaces: list[str], near: list[tuple[str, tuple[float, float], float]], kinds: list[str],
           both_ends: bool, limit: int | None, origins: list[str] | None = None) -> list[dict[str, Any]]:
    """Doors passing the filters ("space" or "near" matches; no such filter: all), one end per link."""
    wanted = {s.lower() for s in spaces}
    picked = []
    for door in census.doors:
        if kinds and door["kind"] not in kinds:
            continue
        if origins and door["originKind"] not in origins:
            continue
        if wanted or near:
            ok = census.space_edid(door["at"]["spaceId"]).lower() in wanted
            for world, (x, y), radius in near:
                if census.space_edid(door["at"]["spaceId"]).lower() == world.lower() and math.hypot(door["pos"][0] - x, door["pos"][1] - y) <= radius:
                    ok = True
            if not ok:
                continue
        picked.append(door)
    picked = outside_ends(census, picked, both_ends)
    ordered = order(picked)
    return ordered[:limit] if limit else ordered


def parse_near(census: Census, text: str) -> tuple[str, tuple[float, float], float]:
    """"Tamriel:19312,-7424:4000" or "Tamriel:door:Skyrim.esm:01B1F1:4000"."""
    world, _, rest = text.partition(":")
    where, _, radius = rest.rpartition(":")
    if where.startswith("door:"):
        door = census.by_id[where[5:]]
        return world, (door["pos"][0], door["pos"][1]), float(radius)
    x, y = where.split(",")
    return world, (float(x), float(y)), float(radius)


def order(doors: list[dict[str, Any]]) -> list[dict[str, Any]]:
    """Grouped by the space the visit starts in (so cells load once), nearest neighbour inside."""
    groups: dict[str, list[dict[str, Any]]] = {}
    for door in doors:
        groups.setdefault(door["at"]["spaceId"], []).append(door)
    out: list[dict[str, Any]] = []
    # Exteriors first, then interiors; stable by id.
    for space_id in sorted(groups, key=lambda s: (not s.startswith("worldspace"), s)):
        rest = sorted(groups[space_id], key=lambda d: (d["pos"][0], d["pos"][1]))
        here = rest.pop(0)
        out.append(here)
        while rest:
            nxt = min(rest, key=lambda d: math.hypot(d["pos"][0] - here["pos"][0], d["pos"][1] - here["pos"][1]))
            rest.remove(nxt)
            out.append(nxt)
            here = nxt
    return out


# ---- geometry of one visit ----------------------------------------------------------------------


def unit(dx: float, dy: float) -> tuple[float, float]:
    length = math.hypot(dx, dy) or 1.0
    return dx / length, dy / length


def heading_deg(dx: float, dy: float) -> float:
    """Skyrim heading: 0 = north (+Y), clockwise."""
    return math.degrees(math.atan2(dx, dy)) % 360


def front_of(census: Census, door: dict[str, Any]) -> dict[str, Any]:
    """Where players stand to use `door`: on the side the partner door lands them (its landPos,
    which the census gives in this door's space). Falls back to the door's own rotation when the
    pair is not mutual (guess=True)."""
    partner = census.partner(door)
    pos = door["pos"]
    if partner and partner["dest"]["doorId"] == door["id"]:
        land = partner["dest"]["landPos"]
        dx, dy = unit(land[0] - pos[0], land[1] - pos[1])
        guess = False
        land_z = land[2]
    else:
        yaw = door["rot"][2]
        dx, dy = math.sin(yaw), math.cos(yaw)
        guess = True
        land_z = pos[2]
    return {
        "dir": (dx, dy),
        "pos": (pos[0] + dx * FRONT_DISTANCE, pos[1] + dy * FRONT_DISTANCE, land_z),
        "guess": guess,
        "landZ": land_z,
    }


def beyond_of(door_pos: list[float], dx: float, dy: float, distance: float = BEYOND_DISTANCE) -> tuple[float, float]:
    """The point `distance` past the door, walking in the direction (dx, dy) that points INTO it."""
    return door_pos[0] + dx * distance, door_pos[1] + dy * distance


def grid_of(pos: list[float]) -> tuple[int, int]:
    return math.floor(pos[0] / 4096), math.floor(pos[1] / 4096)
