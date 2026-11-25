"""report.md and contact sheets from a crawl's results."""

from __future__ import annotations

import collections
from pathlib import Path
from typing import Any

import imaging

ORDER = ["hang", "setup-failed", "wrong-cell", "fall-through", "timeout", "no-portal", "loading-screen", "scene-problem", "visual-suspect", "ok", "skipped"]
STAGES = ["front", "open", "in", "in-back", "back-open", "out"]


def frame_p95(rec: dict[str, Any]) -> float:
    return max(((c.get("frame") or {}).get("p95Ms", 0.0) for c in rec.get("checks", {}).values() if isinstance(c, dict)), default=0.0)


def worst_frame(rec: dict[str, Any]) -> float:
    return max(((c.get("frame") or {}).get("maxMs", 0.0) for c in rec.get("checks", {}).values() if isinstance(c, dict)), default=0.0)


def links(rec: dict[str, Any]) -> str:
    return " ".join(f"[{name}]({path})" for name, path in rec.get("shots", {}).items())


def write(out: Path, config: dict[str, Any], results: list[dict[str, Any]], census: Any) -> None:
    out.mkdir(parents=True, exist_ok=True)
    counts = collections.Counter(r["result"] for r in results)
    lines = [f"# Door crawl: {out.name}", ""]
    lines.append(f"{len(results)} doors visited. " + ", ".join(f"{counts[c]} {c}" for c in ORDER if counts[c]) + ".")
    lines.append("")
    lines.append("Filters: " + ", ".join(f"{k}={v}" for k, v in config.items() if k in ("preset", "space", "near", "kind", "origin", "ends", "max") and v) + ".")
    lines.append("")
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
    lines += ["## All doors", "", "| # | door | place | result | notes | pictures |", "|--:|---|---|---|---|---|"]
    for r in results:
        notes = "; ".join([p[1] for p in r["problems"]] + [f"flag {f}" for f in r["flags"]] + r["notes"] + ["log: " + x[:120] for x in r.get("log", [])[:2]])
        lines.append(f"| {r['index']} | {r['id']} `{r['base']}` ({r['runtime']}) | {r['place']} -> {r['dest']} | {r['result']} | {notes.replace('|', '/')} | {links(r)} |")
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
