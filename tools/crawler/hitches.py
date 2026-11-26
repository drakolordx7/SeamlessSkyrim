"""Reads a crawl's hitches.txt (the plugin's hitch watcher, bridge `hitch_watch`; the crawler starts it)
and says, for every frame over a threshold, which stage and door it was in and where the main thread was.

    cd tools\\skyrim-mcp
    uv run python ..\\crawler\\hitches.py <crawl run folder> [--min 100] [--top 8] [--all]
    uv run python ..\\crawler\\hitches.py <run> --summary        # frames over 40 / 100 ms per stage, no stacks

A hitch is "H <time> <frame ms>" and its "S <ms into the frame> <stack>" samples (leaf first, module+offset);
"M <time> <text>" lines are the crawler's marks (stage, door, the commands it sent). Names: SkyrimSE frames as
Address Library IDs, SeamlessSkyrim ones from the build's PDB (the DLL in the game must be this build).
Frames the crawler's own commands caused (a screenshot, scene_check) are listed apart as "own".
"""
import argparse
import collections
import glob
import subprocess
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).parents[1] / "skyrim-mcp"))

OWN = ("screenshot", "scene_check", "move_to", "console", "camera_view", "ray_down", "refs_near")


def parse(path: Path):
    """[(kind, time, payload...)] in file order; hitches carry their samples."""
    events = []
    for line in path.read_text("utf-8", "replace").splitlines():
        parts = line.split("\t")
        if parts[0] == "M" and len(parts) >= 3:
            events.append(("M", parts[1], parts[2]))
        elif parts[0] == "H" and len(parts) >= 3:
            events.append(("H", parts[1], float(parts[2]), []))
        elif parts[0] == "S" and len(parts) >= 3 and events and events[-1][0] == "H":
            events[-1][3].append((float(parts[1]), [f for f in parts[2].split(",") if f]))
    return events


def seconds(t: str) -> float:
    h, m, s = t.split(":")
    return int(h) * 3600 + int(m) * 60 + float(s)


def classify(events):
    """Each hitch with the stage and door in force, and the marks in the 1.5 s before it ended."""
    door = stage = ""
    marks: list[tuple[float, str]] = []
    out = []
    for e in events:
        if e[0] == "M":
            text = e[2]
            if text.startswith("DOOR "):
                door, stage = text.split()[1], "start"
            elif text.startswith("stage "):
                stage = text.split()[1]
            marks.append((seconds(e[1]), text))
        else:
            start, ms = seconds(e[1]), e[2]
            near = [t for s, t in marks if start - 2.5 <= s <= start + ms / 1000.0 + 0.2]
            # A command the crawler sent just before (a screenshot, a teleport, a console load) is the frame's own cause.
            own = next((t for s, t in reversed(marks) if start - 0.6 <= s <= start + ms / 1000.0 and t.split()[0] in OWN), None)
            out.append({"time": e[1], "ms": ms, "door": door, "stage": stage, "near": near[-4:], "own": own, "samples": e[3]})
    return out


def causes(hitches, args) -> None:
    """One line per cause: how many frames, how long in all, the worst, and in which stages."""
    import server  # noqa: E402

    d = server.disassembler()
    dll = args.dll or Path(__file__).parents[2] / "build/RelWithDebInfo/SeamlessSkyrim.dll"
    symbolizer = next(iter(sorted(glob.glob("C:/Program Files/Microsoft Visual Studio/*/*/VC/Tools/MSVC/*/bin/Hostx64/x64/llvm-symbolizer.exe"))), None)
    shown = [h for h in hitches if h["ms"] >= args.min and (args.all or not h["own"])]
    offsets = sorted({int(f.split("+")[1], 16) for h in shown for _, st in h["samples"] for f in st if f.startswith("SeamlessSkyrim+")})
    names: dict[int, str] = {}
    if offsets and symbolizer:
        out = subprocess.run([symbolizer, "--obj", str(dll), "--inlines", "-s"], input="\n".join(hex(0x180000000 + o) for o in offsets), capture_output=True, text=True).stdout
        for offset, block in zip(offsets, out.strip().split("\n\n")):
            names[offset] = block.split("\n")[0].replace("`anonymous namespace'::", "")[:70]

    def tag_of(stack: list[str]) -> str:
        labels = []
        for f in stack:
            module, offset = f.split("+")
            offset = int(offset, 16)
            if module == "SeamlessSkyrim":
                labels.append("SS:" + names.get(offset, f"+{offset:X}"))
            elif module == "SkyrimSE":
                near = d.ids.nearest(offset)
                labels.append(f"ID {near[0]}" if near else f"SkyrimSE+{offset:X}")
            else:
                labels.append(module)
        text = " ".join(labels)
        if "HoldFlash" in text:
            return "flash guard (waits for the GPU)"
        if "D3DCOMPILER_47" in text:
            ours = next((x for x in labels if x.startswith("SS:")), "?")
            return f"shader compiled on the main thread ({ours})"
        if "ID 30451" in text:
            return "task-queue wait at the end of SetCurrentCell (ID 30451)"
        if "gameoverlayrenderer64" in text or ("dxgi" in labels[:12] and "SS:Render::Present::PresentThunk" not in text and "ID 107644" not in text):
            return "Present waits for the GPU (frame queue full)"
        if "nvwgf2umx" in labels[:8]:
            ours = next((x for x in labels if x.startswith("SS:")), "")
            return f"GPU driver wait ({ours or next((x for x in labels if x.startswith('ID')), '?')})"
        ours = next((x for x in labels if x.startswith("SS:")), None)
        if ours:
            return "ours: " + ours
        engine = [x for x in labels if x.startswith("ID")]
        return "engine: " + " < ".join(engine[:3])

    rows: dict[str, list] = collections.defaultdict(list)
    for h in shown:
        votes = collections.Counter(tag_of(st) for _, st in h["samples"])
        if votes:
            rows[votes.most_common(1)[0][0]].append(h)
    print(f"{len(shown)} frames of {args.min:.0f} ms or more, by cause")
    for tag, hs in sorted(rows.items(), key=lambda kv: -sum(h["ms"] for h in kv[1])):
        stages = collections.Counter(h["stage"] for h in hs)
        print(f"{len(hs):4d} frames {sum(h['ms'] for h in hs):7.0f} ms total, worst {max(h['ms'] for h in hs):4.0f}  {tag}   [{', '.join(f'{k} {v}' for k, v in stages.most_common())}]")


def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument("run")
    ap.add_argument("--min", type=float, default=100.0)
    ap.add_argument("--top", type=int, default=5)
    ap.add_argument("--depth", type=int, default=6)
    ap.add_argument("--all", action="store_true", help="also the frames the crawler's own commands were in")
    ap.add_argument("--summary", action="store_true")
    ap.add_argument("--causes", action="store_true", help="every frame over --min (not the crawler's own commands) sorted into a cause by where most of its samples were")
    ap.add_argument("--dll", type=Path, help="the SeamlessSkyrim.dll (with its PDB beside it) the game ran, when the build folder's has moved on")
    args = ap.parse_args()
    path = Path(args.run)
    path = path / "hitches.txt" if path.is_dir() else path
    hitches = classify(parse(path))

    if args.causes:
        causes(hitches, args)
        return

    if args.summary:
        table = collections.defaultdict(lambda: [0, 0, 0.0])
        for h in hitches:
            row = table[(h["door"], h["stage"])]
            row[0] += 1
            row[1] += h["ms"] >= 100
            row[2] = max(row[2], h["ms"])
        print(f"{'door':10} {'stage':8} >40 >=100 max")
        for (door, stage), (n, big, worst) in sorted(table.items()):
            print(f"{door:10} {stage:8} {n:3d} {big:4d} {worst:5.0f}")
        return

    import server  # noqa: E402  (reads harness settings)

    d = server.disassembler()
    symbols: dict[tuple[str, int], str] = {}
    own_dll = {"SeamlessSkyrim": args.dll or Path(__file__).parents[2] / "build/RelWithDebInfo/SeamlessSkyrim.dll"}
    symbolizer = next(iter(sorted(glob.glob("C:/Program Files/Microsoft Visual Studio/*/*/VC/Tools/MSVC/*/bin/Hostx64/x64/llvm-symbolizer.exe"))), None)
    shown = [h for h in hitches if h["ms"] >= args.min and (args.all or not h["own"])]
    if symbolizer:
        for module, dll in own_dll.items():
            offsets = sorted({int(f.split("+")[1], 16) for h in shown for _, st in h["samples"] for f in st if f.startswith(module + "+")})
            if offsets and dll.exists():
                out = subprocess.run([symbolizer, "--obj", str(dll), "--inlines", "-s"], input="\n".join(hex(0x180000000 + o) for o in offsets),
                                     capture_output=True, text=True).stdout
                for offset, block in zip(offsets, out.strip().split("\n\n")):
                    lines = block.split("\n")
                    symbols[(module, offset)] = f"{module}!{lines[0][:90]}"

    cache: dict[str, str] = {}

    def label(frame: str) -> str:
        if frame not in cache:
            module, offset = frame.split("+")
            offset = int(offset, 16)
            if (module, offset) in symbols:
                cache[frame] = symbols[(module, offset)]
            elif module != "SkyrimSE":
                cache[frame] = module
            else:
                nearest = d.ids.nearest(offset)
                cache[frame] = f"SkyrimSE+{offset:X}" if not nearest else f"ID {nearest[0]}{' ' + d.names.get(nearest[0], '') if d.names.get(nearest[0]) else ''}"
        return cache[frame]

    print(f"{len(hitches)} frames over the watcher's threshold; {len(shown)} of {args.min:.0f} ms or more{'' if args.all else ' (not in the crawler own commands)'}")
    for h in shown:
        print(f"\n== {h['time']}  {h['ms']:.0f} ms  door {h['door']}  stage {h['stage']}  own={h['own']}  marks={h['near']}")
        system = ("ntdll", "KERNELBASE", "KERNEL32", "ucrtbase", "VCRUNTIME140", "win32u", "MSVCP140", "user32")
        gpu = ("nvwgf2umx", "d3d11", "dxgi", "dxcore", "nvwgf2umx_cfg", "nvdxgi")
        chains: collections.Counter = collections.Counter()
        for _, stack in h["samples"]:
            names = [label(f) for f in stack]
            skip = 0
            driver = False
            while skip < len(names) and (names[skip] in system or names[skip] in gpu):
                driver = driver or names[skip] in gpu
                skip += 1
            if skip < len(names):
                waiting = ("gpu-wait:" if driver else "wait:") if skip else ""
                ours = next((n for n in names if n.startswith("SeamlessSkyrim!")), "")
                chains[waiting + " < ".join(names[skip : skip + args.depth]) + (f"  [in {ours[15:]}]" if ours else "")] += 1
        total = max(1, len(h["samples"]))
        print(f"   {len(h['samples'])} samples; where the main thread was (leaf-most non-system frames; wait: = inside ntdll/kernel):")
        for chain, count in chains.most_common(args.top):
            print(f"   {100 * count / total:5.1f}%  {chain}")


if __name__ == "__main__":
    main()


def report_lines(run: Path, top: int = 8) -> list[str]:
    """Markdown for the crawl's report: the frames the plugin's hitch watcher caught, apart from the ones the
    crawler's own commands made (hitches.txt of the run folder; nothing when there is none)."""
    path = run / "hitches.txt"
    if not path.exists():
        return []
    hitches = classify(parse(path))
    mine = [h for h in hitches if not h["own"]]
    # A frame is over a threshold when it ran longer than it: the watcher's own threshold is 40 ms.
    lines = ["## Frames over 40 ms (hitch watcher, the crawler's own screenshots, teleports and console loads left out)", ""]
    stages = collections.defaultdict(lambda: [0, 0, 0, 0.0])
    for h in mine:
        if h["stage"] == "start":
            continue  # setting the scene: the crawler's cow/coc/move_to
        row = stages[h["stage"]]
        row[0] += 1
        row[1] += h["ms"] >= 50
        row[2] += h["ms"] >= 100
        row[3] += h["ms"]
    lines.append("| stage | frames > 40 ms | > 50 ms | > 100 ms | their time |")
    lines.append("|---|--:|--:|--:|--:|")
    for stage in ("front", "open", "in", "in-back", "out"):
        n, mid, big, total = stages.get(stage, [0, 0, 0, 0.0])
        lines.append(f"| {stage} | {n} | {mid} | {big} | {total / 1000:.1f} s |")
    lines.append("")
    worst = sorted((h for h in mine if h["stage"] != "start"), key=lambda h: -h["ms"])[:top]
    lines += [f"- {h['ms']:.0f} ms: door {h['door']}, stage {h['stage']} ({h['time']})" for h in worst]
    lines.append("")
    return lines
