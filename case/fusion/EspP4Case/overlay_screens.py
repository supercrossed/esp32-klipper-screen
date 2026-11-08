"""
Put the real UI on the display in the finished-case renders.

render_build_guide.py (run in Fusion) writes, for each final-assembly shot, the image-pixel
corners of the display's active area plus a mask render where the glass is flat green. This
script perspective-warps a UI screenshot from docs/images into that quad, but only paints
pixels where the mask shows the glass is actually visible (so the bezel edge stays in front).

Run with plain Python (needs Pillow and numpy), after render_build_guide.py:
    python overlay_screens.py
"""

import json, os, sys
import numpy as np
from PIL import Image, ImageDraw, ImageFilter

HERE = os.path.dirname(os.path.abspath(__file__))
PROJECT = os.path.normpath(os.path.join(HERE, "..", "..", ".."))
EXPORT = os.path.join(PROJECT, "case", "export")
RENDER = os.path.join(EXPORT, "_render")
UI_DIR = os.path.join(PROJECT, "docs", "images")

SCREEN_BRIGHTNESS = 0.93     # a lit panel behind glass reads slightly darker than the raw screenshot
GLARE = 0.05                 # faint diagonal reflection on the glass


def perspective_coeffs(dst_quad, src_w, src_h):
    """Coefficients for Image.transform(PERSPECTIVE): output pixel (dst) -> source pixel."""
    src = [(0, 0), (src_w, 0), (src_w, src_h), (0, src_h)]
    A, b = [], []
    for (x, y), (u, v) in zip(dst_quad, src):
        A.append([x, y, 1, 0, 0, 0, -u * x, -u * y]); b.append(u)
        A.append([0, 0, 0, x, y, 1, -v * x, -v * y]); b.append(v)
    return np.linalg.solve(np.array(A, float), np.array(b, float)).tolist()


def key_mask(mask_png, quad, size):
    """Visible-glass pixels (flat green in the mask render), clipped to the active-area quad."""
    m = np.asarray(Image.open(mask_png).convert("RGB")).astype(int)
    r, g, b = m[..., 0], m[..., 1], m[..., 2]
    green = (g > r + 18) & (g > b + 18)
    poly = Image.new("L", size, 0)
    ImageDraw.Draw(poly).polygon([tuple(p) for p in quad], fill=255)
    a = np.where(green & (np.asarray(poly) > 0), 255, 0).astype(np.uint8)
    return Image.fromarray(a).filter(ImageFilter.GaussianBlur(0.6))


def overlay(shot, info):
    base_path = os.path.join(EXPORT, shot)
    base = Image.open(base_path).convert("RGB")
    W, H = base.size
    quad = info["quad"]
    ui = Image.open(os.path.join(UI_DIR, info["ui"])).convert("RGB")

    # pre-shrink the UI towards its on-image size so the warp doesn't alias
    span = max(np.hypot(quad[1][0] - quad[0][0], quad[1][1] - quad[0][1]),
               np.hypot(quad[2][0] - quad[3][0], quad[2][1] - quad[3][1]))
    scale = min(1.0, 1.6 * span / ui.width)
    if scale < 1.0:
        ui = ui.resize((int(ui.width * scale), int(ui.height * scale)), Image.LANCZOS)
    ui = Image.eval(ui, lambda v: int(v * SCREEN_BRIGHTNESS))

    warped = ui.transform((W, H), Image.PERSPECTIVE, perspective_coeffs(quad, ui.width, ui.height),
                          Image.BICUBIC)

    # faint glare: brighter towards the top-left of the screen
    if GLARE > 0:
        g = np.zeros((H, W), float)
        (x0, y0), (x2, y2) = quad[0], quad[2]
        yy, xx = np.mgrid[0:H, 0:W]
        d = ((xx - x0) * (x2 - x0) + (yy - y0) * (y2 - y0)) / max(1.0, (x2 - x0) ** 2 + (y2 - y0) ** 2)
        g = np.clip(1.0 - d * 1.6, 0, 1) * 255 * GLARE
        w = np.asarray(warped).astype(float) + g[..., None]
        warped = Image.fromarray(np.clip(w, 0, 255).astype(np.uint8))

    mask = key_mask(os.path.join(RENDER, shot.replace(".png", "_mask.png")), quad, (W, H))
    base.paste(warped, (0, 0), mask)
    base.save(base_path)
    return shot, info["ui"]


def main():
    with open(os.path.join(RENDER, "screen_quads.json")) as fh:
        quads = json.load(fh)
    for shot, info in quads.items():
        print("%s <- %s" % overlay(shot, info))


if __name__ == "__main__":
    sys.exit(main())
