"""AE 1.7.104 and SE 1.5.97 code side by side, for porting an address to SE.

Run from tools/skyrim-mcp (its venv has capstone): uv run python ../se-port/sepair.py <command>
  dis ae|se <id> [n]           disassemble, offsets from the function start
  site <ae fn> <ae off> <ae callee>
                               the same call in SE: the SE call sites to the mapped callee, and the
                               one at the AE call's position among them
  refs <ae id> ...             SE candidates for an AE global or function, by where the AE
                               functions referencing it reference things in their SE versions

Needs build/se-port/SkyrimSE-1.5.97-mem.exe (dump.py), se-ids.csv (addrlib.py) and
ae2se-all.txt (fullmap.py). AE code comes from the exe on disk: set SKYRIM_AE_EXE to the AE game's SkyrimSE.exe.
"""
import bisect, collections, csv, json, os, pathlib, struct, sys
ROOT = pathlib.Path(__file__).resolve().parents[2]
DATA = ROOT / "build" / "se-port"
sys.path.insert(0, str(ROOT / "tools" / "skyrim-mcp"))
import retools
from capstone import CS_GRP_CALL, CS_GRP_JUMP
from capstone.x86 import X86_OP_IMM, X86_OP_MEM, X86_REG_RIP

CACHE = ROOT / "tools" / "skyrim-mcp" / ".cache"
AE_EXE = pathlib.Path(os.environ["SKYRIM_AE_EXE"])  # the AE game's SkyrimSE.exe
AE_IDS = next(CACHE.glob("ids-*.csv"))  # exported by the MCP server from a running AE game


class Game:
    def __init__(self, exe, ids):
        self.img = retools.Image(exe)
        self.ids = {int(r[0]): int(r[1], 16) for r in csv.reader(open(ids)) if r[0] != "id"}
        self.starts = sorted(set(self.ids.values()))
        self.dis = retools.Disassembler(self.img, retools.IdMap(ids), {})

    def end_of(self, start):
        i = bisect.bisect_right(self.starts, start)
        return self.starts[i] if i < len(self.starts) else start + 0x1000

    def calls(self, fid, callee, limit=0x4000):
        s = self.ids[fid]; e = min(self.end_of(s), s + limit); t = self.ids[callee]
        code = self.img.read(s, e - s + 5)
        return [i for i in range(len(code) - 5) if code[i] == 0xE8 and s + i + 5 + struct.unpack_from("<i", code, i + 1)[0] == t]

    def refs(self, fid, limit=0x3000):
        s = self.ids[fid]; e = min(self.end_of(s), s + limit)
        out = []
        for insn in self.dis.cs.disasm(self.img.read(s, e - s), s):
            for op in insn.operands:
                t = None
                if op.type == X86_OP_MEM and op.mem.base == X86_REG_RIP:
                    t = insn.address + insn.size + op.mem.disp
                elif op.type == X86_OP_IMM and (insn.group(CS_GRP_CALL) or insn.group(CS_GRP_JUMP)):
                    t = op.imm
                if t is not None and not (s <= t < e) and (h := self.dis.ids.nearest(t)) and h[1] == 0:
                    out.append(h[0])
        return out


AE = Game(AE_EXE, AE_IDS)
AE.dis.names = {int(k): v for k, v in json.loads((CACHE / "names.json").read_text()).items()}
SE = Game(DATA / "SkyrimSE-1.5.97-mem.exe", DATA / "se-ids.csv")
AE2SE = {}
for line in open(DATA / "ae2se-all.txt"):
    a, s = map(int, line.split()); AE2SE[a] = s


def dis(game, fid, n=200):
    g = AE if game == "ae" else SE
    s = g.ids[fid]
    for line in g.dis.disassemble(s, n):
        if not line.startswith("..."):
            print(f"+{int(line.split()[0], 16) - s:04X} {line[10:]}")


def site(caller, off, callee):
    ae_hits = AE.calls(caller, callee)
    se_hits = SE.calls(AE2SE[caller], AE2SE[callee])
    n = ae_hits.index(off) if off in ae_hits else None
    guess = se_hits[n] if n is not None and len(se_hits) == len(ae_hits) else None
    print(f"AE {caller}+0x{off:X} -> {callee}: calls at {[hex(x) for x in ae_hits]} | SE {AE2SE[caller]} -> {AE2SE[callee]}: {[hex(x) for x in se_hits]}"
          f" => {hex(guess) if guess is not None else 'not matched, read both'}")


def refs(ae_id):
    fns = set()
    for x in AE.dis.xrefs(AE.ids[ae_id], 200):
        a = int(x.split()[0], 16)
        start = AE.starts[bisect.bisect_right(AE.starts, a) - 1]
        fns.update(k for k in [AE.dis.ids.nearest(a)[0]] if AE.ids[k] == start)
    votes = collections.Counter()
    for f in fns:
        if AE2SE.get(f) not in SE.ids:
            continue
        ka, ks = AE.refs(f), SE.refs(AE2SE[f])
        for n, i in enumerate(ka):
            if i == ae_id and len(ka) == len(ks):
                votes[ks[n]] += 1
    print(ae_id, "table:", AE2SE.get(ae_id), "votes:", votes.most_common(4), f"({len(fns)} referencing functions)")


if __name__ == "__main__":
    cmd, args = sys.argv[1], sys.argv[2:]
    if cmd == "dis":
        dis(args[0], int(args[1]), int(args[2]) if len(args) > 2 else 200)
    elif cmd == "site":
        site(int(args[0]), int(args[1], 0), int(args[2]))
    elif cmd == "refs":
        for a in args:
            refs(int(a))
