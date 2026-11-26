"""report.md and contact sheets from a crawl's results."""

from __future__ import annotations

import collections
from pathlib import Path
from typing import Any

import imaging

ORDER = ["hang", "setup-failed", "wrong-cell", "fall-through", "timeout", "fallback-loading-screen", "not-live", "tier-low", "scene-problem", "visual-suspect", "ok", "skipped"]
STAGES = ["front", "open", "in", "in-back", "back-open", "out"]


def frame_p95(rec: dict[str, Any]) -> float:
    return max(((c.get("frame") or {}).get("p95Ms", 0.0) for c in rec.get("checks", {}).values() if isinstance(c, dict)), default=0.0)


def worst_frame(rec: dict[str, Any]) -> float:
    return max(((c.get("frame") or {}).get("maxMs", 0.0) for c in rec.get("checks", {}).values() if isinstance(c, dict)), default=0.0)


def links(rec: dict[str, Any]) -> str:
    return " ".join(f"[{name}]({path})" for name, path in rec.get("shots", {}).items())


def tiers(rec: dict[str, Any]) -> str:
    checks = rec.get("checks", {})
    front, opened = checks.get("front") or {}, checks.get("open") or {}
    if "tierReached" not in front:
        return "n/a (old crawl)"
    return f"{front['tierReached']} / {opened.get('tierReached', '-')}" + (f" in {opened['liveS']} s" if opened.get("tierReached") == "live" else "")


def plugin_errors() -> list[str]:
    """Distinct error lines ([E]) of the plugin log of the last game this crawl ran (shader compile
    failures and the like do not show in a door's result, so the report lists them)."""
    import sys
    server = sys.modules.get("server")
    path = getattr(server, "PLUGIN_LOG", None)
    if path is None or not Path(path).exists():
        return []
    seen: dict[str, int] = {}
    for line in Path(path).read_text(encoding="utf-8", errors="replace").splitlines():
        if "] [E] " in line:
            text = line.split("] [E] ", 1)[1]
            seen[text] = seen.get(text, 0) + 1
    return [f"{n}x {t}" if n > 1 else t for t, n in seen.items()]


def write(out: Path, config: dict[str, Any], results: list[dict[str, Any]], census: Any) -> None:
    out.mkdir(parents=True, exist_ok=True)
    counts = collections.Counter(r["result"] for r in results)
    lines = [f"# Door crawl: {out.name}", ""]
    lines.append(f"{len(results)} doors visited. " + ", ".join(f"{counts[c]} {c}" for c in ORDER if counts[c]) + ".")
    lines.append("")
    lines.append("Filters: " + ", ".join(f"{k}={v}" for k, v in config.items() if k in ("preset", "space", "near", "kind", "origin", "ends", "max") and v) + ".")
    lines.append("")
    errors = plugin_errors()
    if errors:
        lines += ["## Plugin errors (last game's log)", ""] + [f"- `{e}`" for e in errors[:15]] + [""]
    bad = sorted((r for r in results if r["result"] not in ("ok", "skipped")), key=lambda r: (ORDER.index(r["result"]), -len(r["problems"])))
    if bad:
        lines += ["## Worst offenders", ""]
        for r in bad[:10]:
            lines.append(f"- **{r['result']}** {r['id']} {r['base']} ({r['place']} -> {r['dest']}): " + "; ".join(p[1] for p in r["problems"][:2] or [("", "flags " + ", ".join(r["flags"]))]))
        lines.append("")
    slow = sorted((r for r in results if r.get("checks")), key=worst_frame, reverse=True)[:5]
    if slow and worst_frame(slow[0]) > 0:
        lines += ["## Slowest frames (max ms over the visit)", ""]
        lines += [f"- {r['id']} {r['base']} ({r['place']}): max {worst_frame(r):.0f} ms, p95 {frame_p95(r):.0f} ms" for r in slow]
        lines.append("")
    reached = collections.Counter((r.get("checks", {}).get("front") or {}).get("tierReached", "?") for r in results if r["result"] != "skipped")
    lines.append("Tier of the space behind the door when approached with it shut (6 m): " + ", ".join(f"{n} {t}" for t, n in reached.most_common()) + ".")
    live = collections.Counter((r.get("checks", {}).get("open") or {}).get("tierReached", "?") for r in results if "open" in r.get("checks", {}))
    lines.append("Tier once opened: " + ", ".join(f"{n} {t}" for t, n in live.most_common()) + ".")
    lines.append("")
    lines += ["## All doors", "", "| # | door | place | result | tier 6 m / opened | notes | pictures |", "|--:|---|---|---|---|---|---|"]
    for r in results:
        notes = "; ".join([p[1] for p in r["problems"]] + [f"flag {f}" for f in r["flags"]] + r["notes"] + ["log: " + x[:120] for x in r.get("log", [])[:2]])
        lines.append(f"| {r['index']} | {r['id']} `{r['base']}` ({r['runtime']}) | {r['place']} -> {r['dest']} | {r['result']} | {tiers(r)} | {notes.replace('|', '/')} | {links(r)} |")
    lines.append("")

    # Contact sheets of everything that is not ok: six doors per sheet, one row per door.
    suspects = [r for r in results if r["result"] not in ("ok", "skipped")]
    for old in out.glob("contact-*.jpg"):
        old.unlink()
    sheets = []
    for i in range(0, len(suspects), 6):
        chunk = suspects[i:i + 6]
        rows = [(f"{r['result']}: {r['id']} {r['base']} {r['place']} -> {r['dest']}", [out / r["shots"][s] if s in r["shots"] else None for s in STAGES]) for r in chunk]
        name = f"contact-{i // 6 + 1:02}.jpg"
        imaging.contact_sheet(rows, out / name)
        sheets.append(name)
    if sheets:
        lines += ["## Contact sheets (suspects; columns: " + ", ".join(STAGES) + ")", ""] + [f"- [{s}]({s})" for s in sheets] + [""]
    (out / "report.md").write_text("\n".join(lines), "utf-8")
