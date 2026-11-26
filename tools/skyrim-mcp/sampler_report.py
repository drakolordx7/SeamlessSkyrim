"""Summarise a Sampler file (bridge command `travel_profile`, src/Bridge/Sampler.cpp).

    python sampler_report.py <file> [from_ms] [to_ms] [--top N] [--chain "ID 75708"]

Prints inclusive and exclusive sample counts per function (SkyrimSE frames resolved to Address
Library IDs, SeamlessSkyrim to functions from its PDB, other
modules by name) for the samples in the time window. With --chain, prints the most common call
chains around the given function instead (frames as "ID n+0xoffset").
"""
import argparse
import collections
import glob
import subprocess
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).parent))
import server  # noqa: E402

parser = argparse.ArgumentParser()
parser.add_argument("file")
parser.add_argument("start", nargs="?", type=float, default=0.0)
parser.add_argument("end", nargs="?", type=float, default=1e12)
parser.add_argument("--top", type=int, default=40)
parser.add_argument("--chain")
args = parser.parse_args()

d = server.disassembler()
cache: dict[tuple[str, bool], str] = {}

# Named from the build's PDB: the DLL in the game must be this build, or the names are wrong.
# The system's dbghelp found no symbols in them; Visual Studio's llvm-symbolizer does.
OWN_MODULES = {
    "SeamlessSkyrim": Path(__file__).parents[2] / "build/RelWithDebInfo/SeamlessSkyrim.dll",
}
SYMBOLIZER = next(iter(sorted(glob.glob("C:/Program Files/Microsoft Visual Studio/*/*/VC/Tools/MSVC/*/bin/Hostx64/x64/llvm-symbolizer.exe"))), None)
symbols: dict[tuple[str, int], str] = {}


def symbolize(stacks: list[list[str]]) -> None:
    if not SYMBOLIZER:
        return
    for module, dll in OWN_MODULES.items():
        offsets = sorted({int(f.split("+")[1], 16) for stack in stacks for f in stack if f.startswith(module + "+")})
        if not offsets or not dll.exists():
            continue
        # Addresses as the DLL's preferred image base has them.
        out = subprocess.run([SYMBOLIZER, "--obj", str(dll), "--no-inlines", "-s"], input="\n".join(hex(0x180000000 + o) for o in offsets),
                             capture_output=True, text=True).stdout
        for offset, block in zip(offsets, out.strip().split("\n\n")):
            lines = block.split("\n")
            where = f" ({Path(lines[1]).name})" if len(lines) > 1 else ""
            symbols[(module, offset)] = f"{module}!{lines[0][:100]}{where}"


def label(frame: str, with_offset: bool) -> str:
    key = (frame, with_offset)
    if key not in cache:
        module, offset = frame.split("+")
        offset = int(offset, 16)
        if (module, offset) in symbols:
            cache[key] = symbols[(module, offset)] + (f"+0x{offset:X}" if with_offset else "")
        elif module != "SkyrimSE":
            cache[key] = module
        else:
            nearest = d.ids.nearest(offset)
            if not nearest:
                cache[key] = f"SkyrimSE+{offset:X}"
            else:
                fid, distance = nearest
                name = d.names.get(fid, "")
                cache[key] = f"ID {fid}+0x{distance:X}" if with_offset else f"ID {fid}{' ' + name if name else ''}"
    return cache[key]


samples = []
for line in open(args.file):
    ms, stack = line.rstrip("\n").split("\t")
    if args.start <= float(ms) <= args.end:
        samples.append([f for f in stack.split(",") if f])
symbolize(samples)

if args.chain:
    chains = collections.Counter()
    for stack in samples:
        frames = [label(f, True) for f in stack]
        hits = [i for i, f in enumerate(frames) if f.startswith(args.chain + "+")]
        if hits:
            i = hits[-1]
            chains[" < ".join(frames[max(0, i - 3) : i + 4])] += 1
    for chain, count in chains.most_common(args.top):
        print(count, chain)
    sys.exit()

inclusive = collections.Counter()
exclusive = collections.Counter()
for stack in samples:
    frames = [label(f, False) for f in stack]
    if frames:
        exclusive[frames[0]] += 1
        inclusive.update(set(frames))
total = max(1, len(samples))
print(f"{len(samples)} samples in [{args.start}, {args.end}] ms")
print("--- inclusive")
for name, count in inclusive.most_common(args.top):
    print(f"{count:5d} {100 * count / total:5.1f}%  {name}")
print("--- exclusive")
for name, count in exclusive.most_common(20):
    print(f"{count:5d} {100 * count / total:5.1f}%  {name}")
