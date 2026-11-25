"""Picture heuristics for the crawler: flags only, a human looks at what they mark.

The doorway region is projected from the camera position and the door's size with an assumed
field of view, so it is approximate; the checks are written to survive that (a large uniform
sky or black patch where the doorway is, against a frame that isn't uniform).
"""

from __future__ import annotations

import math
from pathlib import Path
from typing import Any

import numpy as np
from PIL import Image, ImageDraw


def doorway_box(size: tuple[int, int], eye: list[float], door_pos: list[float], heading_deg: float, half_width: float,
                height: float = 230.0, vfov_deg: float = 65.0) -> tuple[int, int, int, int] | None:
    """Pixel box of the doorway seen from `eye` looking along `heading_deg` (level), or None when it is
    behind the camera. The door's origin is at its base; its front plane is taken facing the camera."""
    w, h = size
    heading = math.radians(heading_deg)
    fwd = (math.sin(heading), math.cos(heading))
    right = (math.cos(heading), -math.sin(heading))
    vx, vy = door_pos[0] - eye[0], door_pos[1] - eye[1]
    depth = vx * fwd[0] + vy * fwd[1]
    if depth < 30:
        return None
    lateral = vx * right[0] + vy * right[1]
    tan_v = math.tan(math.radians(vfov_deg) / 2)
    tan_h = tan_v * w / h

    def px(x_cam: float, z: float) -> tuple[float, float]:
        return (0.5 + (x_cam / depth) / (2 * tan_h)) * w, (0.5 - ((z - eye[2]) / depth) / (2 * tan_v)) * h

    x0, y1 = px(lateral - half_width, door_pos[2])
    x1, y0 = px(lateral + half_width, door_pos[2] + height)
    # Shrink 15 % so the frame and jambs stay out of it, then clamp to the picture.
    cx, cy, bw, bh = (x0 + x1) / 2, (y0 + y1) / 2, (x1 - x0) * 0.85, (y1 - y0) * 0.85
    box = (max(0, int(cx - bw / 2)), max(0, int(cy - bh / 2)), min(w, int(cx + bw / 2)), min(h, int(cy + bh / 2)))
    return box if box[2] - box[0] >= 12 and box[3] - box[1] >= 12 else None


def _luma(a: np.ndarray) -> np.ndarray:
    return a[..., 0] * 0.299 + a[..., 1] * 0.587 + a[..., 2] * 0.114


def region_flags(image: Image.Image, box: tuple[int, int, int, int]) -> dict[str, Any]:
    """Stats of the doorway region against the ring around it, and what looks wrong about it."""
    a = np.asarray(image.convert("RGB"), dtype=np.float32)
    x0, y0, x1, y1 = box
    inner = a[y0:y1, x0:x1]
    bw, bh = x1 - x0, y1 - y0
    rx0, ry0 = max(0, x0 - bw), max(0, y0 - bh // 2)
    rx1, ry1 = min(a.shape[1], x1 + bw), min(a.shape[0], y1 + bh // 2)
    mask = np.ones((ry1 - ry0, rx1 - rx0), dtype=bool)
    mask[y0 - ry0:y1 - ry0, x0 - rx0:x1 - rx0] = False
    ring = a[ry0:ry1, rx0:rx1][mask]
    il, rl = _luma(inner), _luma(ring)
    stats = {
        "box": list(box),
        "mean": round(float(il.mean()), 1), "std": round(float(il.std()), 1),
        "ringMean": round(float(rl.mean()), 1), "ringStd": round(float(rl.std()), 1),
        "rgb": [round(float(v)) for v in inner.reshape(-1, 3).mean(axis=0)],
    }
    flags = []
    r, g, b = stats["rgb"]
    if stats["mean"] < 12 and stats["std"] < 6 and stats["ringMean"] > 35:
        flags.append("black-doorway")
    if stats["std"] < 12 and stats["mean"] > 150 and (b >= r + 8 or stats["mean"] > 235) and stats["ringStd"] > 25:
        flags.append("sky-doorway")
    return {"stats": stats, "flags": flags}


def region_change(a: Image.Image, b: Image.Image, box: tuple[int, int, int, int]) -> float:
    """Mean absolute difference (0-255) of the doorway region between two pictures of the same view."""
    x0, y0, x1, y1 = box
    ca = np.asarray(a.convert("L").resize(b.size).crop(box), dtype=np.float32)
    cb = np.asarray(b.convert("L").crop(box), dtype=np.float32)
    return float(np.abs(ca - cb).mean())


def contact_sheet(rows: list[tuple[str, list[Path]]], out: Path, thumb_width: int = 360) -> None:
    """One row per door: its label and its stage pictures side by side (missing ones left blank)."""
    columns = max((len(r[1]) for r in rows), default=1)
    thumb_h = round(thumb_width * 9 / 16)
    label_h = 22
    sheet = Image.new("RGB", (columns * thumb_width, len(rows) * (thumb_h + label_h)), (24, 24, 24))
    draw = ImageDraw.Draw(sheet)
    for i, (label, paths) in enumerate(rows):
        y = i * (thumb_h + label_h)
        draw.text((4, y + 5), label[:200], fill=(235, 235, 235))
        for j, path in enumerate(paths):
            if path and path.exists():
                with Image.open(path) as img:
                    img = img.convert("RGB")
                    img.thumbnail((thumb_width, thumb_h))
                    sheet.paste(img, (j * thumb_width, y + label_h))
    out.parent.mkdir(parents=True, exist_ok=True)
    sheet.save(out, format="JPEG", quality=85)
