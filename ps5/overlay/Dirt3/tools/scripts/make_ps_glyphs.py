#!/usr/bin/env python3
"""Draws the PlayStation controller glyphs and writes them as DXT5 data for the game's menu textures.

The game's frontend (frontend/b_persistent.pssg) holds one 32x32 DXT5 texture per controller button
("controller_icons_xbox_a" ...), with a 16x16 mip behind it: 1280 bytes. This script draws a
replacement for each button that differs on a PlayStation controller, from nothing but shapes and a
system bold font, and writes platform/ps5/ps_glyphs.inc: for each texture id its 1280 bytes. The
title patches them into a copy of the pack at start-up (shims/frontend/glyph_patch.cpp).

    make_ps_glyphs.py [--preview PNG] [--font TTF]      (needs pillow and numpy)
"""
import argparse
import os
import numpy as np
from PIL import Image, ImageDraw, ImageFont

S = 8            # drawing scale
N = 32 * S

BLUE, RED, PINK, GREEN = (84, 140, 225), (232, 88, 92), (228, 136, 204), (64, 204, 150)
LIGHT, EDGE, TEXT = (226, 229, 233), (58, 60, 66), (84, 87, 94)
STICK, STICK_RING = (74, 76, 82), (205, 207, 212)


def font(path, size):
    return ImageFont.truetype(path, size)


def mix(c, k, to=(0, 0, 0)):
    return tuple(int(c[i] * (1 - k) + to[i] * k) for i in range(3))


def canvas():
    im = Image.new("RGBA", (N, N), (0, 0, 0, 0))
    return im, ImageDraw.Draw(im)


def centered(d, text, f, fill, cx=N // 2, cy=N // 2):
    box = d.textbbox((0, 0), text, font=f)
    d.text((cx - (box[0] + box[2]) / 2, cy - (box[1] + box[3]) / 2), text, font=f, fill=fill)


def face(symbol, color):
    im, d = canvas()
    d.ellipse([8, 8, N - 8, N - 8], fill=color + (255,))
    d.ellipse([8 + 14, 8 + 14, N - 8 - 14, N - 8 - 14], fill=mix(color, 0.62) + (255,))
    bright = mix(color, 0.30, (255, 255, 255)) + (255,)
    c, w = N // 2, 17
    if symbol == "cross":
        r = 50
        d.line([c - r, c - r, c + r, c + r], fill=bright, width=w)
        d.line([c - r, c + r, c + r, c - r], fill=bright, width=w)
    elif symbol == "circle":
        r = 50
        d.ellipse([c - r, c - r, c + r, c + r], outline=bright, width=w)
    elif symbol == "square":
        r = 46
        d.rectangle([c - r, c - r, c + r, c + r], outline=bright, width=w)
    else:  # triangle
        pts = [(c, c - 58), (c - 56, c + 40), (c + 56, c + 40), (c, c - 58)]
        d.line(pts, fill=bright, width=w, joint="curve")
        for p in pts[:3]:
            d.ellipse([p[0] - w // 2, p[1] - w // 2, p[0] + w // 2, p[1] + w // 2], fill=bright)
    return im


def pill(text, tall, f):
    im, d = canvas()
    if tall:   # trigger: wide at the top, narrower below
        pts = [(36, 22), (N - 36, 22), (N - 54, N - 18), (54, N - 18)]
    else:      # bumper
        pts = [(24, 58), (N - 24, 58), (N - 34, N - 66), (34, N - 66)]
    d.polygon(pts, fill=EDGE + (255,))
    inset = 12
    cx = N // 2
    inner = [(x + (inset if x < cx else -inset), y + (inset if y < N // 2 else -inset)) for x, y in pts]
    d.polygon(inner, fill=LIGHT + (255,))
    centered(d, text, f, TEXT + (255,), cy=(N // 2 + 6) if tall else N // 2 - 4)
    return im


def oval(kind):
    im, d = canvas()
    if kind == "options":
        d.ellipse([10, 36, N - 10, N - 36], fill=EDGE + (255,))
        d.ellipse([22, 48, N - 22, N - 48], fill=LIGHT + (255,))
        for y in (104, 128, 152):
            d.rounded_rectangle([88, y - 6, N - 88, y + 6], radius=6, fill=TEXT + (255,))
    else:      # touch pad
        d.rounded_rectangle([10, 40, N - 10, N - 40], radius=40, fill=EDGE + (255,))
        d.rounded_rectangle([22, 52, N - 22, N - 52], radius=30, fill=LIGHT + (255,))
        d.line([N // 2, 64, N // 2, N - 64], fill=TEXT + (255,), width=10)
    return im


def stick(text, f):
    im, d = canvas()
    d.ellipse([8, 8, N - 8, N - 8], fill=STICK_RING + (255,))
    d.ellipse([8 + 12, 8 + 12, N - 8 - 12, N - 8 - 12], fill=STICK + (255,))
    centered(d, text, f, (255, 255, 255, 255))
    return im


def downsample(im, size):
    a = np.asarray(im, dtype=np.float64) / 255.0
    f = a.shape[0] // size
    prem = a.copy()
    prem[..., :3] *= prem[..., 3:4]
    out = prem.reshape(size, f, size, f, 4).mean(axis=(1, 3))
    alpha = out[..., 3:4]
    rgb = np.where(alpha > 1e-6, out[..., :3] / np.maximum(alpha, 1e-6), 0)
    return np.clip(np.concatenate([rgb, alpha], axis=-1) * 255 + 0.5, 0, 255).astype(np.uint8)


def to565(c):
    return ((int(c[0]) >> 3) << 11) | ((int(c[1]) >> 2) << 5) | (int(c[2]) >> 3)


def from565(v):
    r, g, b = (v >> 11) & 31, (v >> 5) & 63, v & 31
    return np.array([(r << 3) | (r >> 2), (g << 2) | (g >> 4), (b << 3) | (b >> 2)], dtype=np.float64)


def encode_bc3(px):
    h, w, _ = px.shape
    out = bytearray()
    for by in range(0, h, 4):
        for bx in range(0, w, 4):
            blk = px[by:by + 4, bx:bx + 4].reshape(16, 4).astype(np.float64)
            alpha = blk[:, 3]
            a0, a1 = int(alpha.max()), int(alpha.min())
            if a0 == a1:
                a0 = min(255, a0 + 1) if a1 < 255 else 255
                a1 = a1 if a1 < 255 else 254
            if a0 < a1:
                a0, a1 = a1, a0
            pal = [a0, a1] + [((7 - i) * a0 + i * a1) // 7 for i in range(1, 7)]
            idx = [int(np.argmin([abs(float(v) - p) for p in pal])) for v in alpha]
            bits = 0
            for i, v in enumerate(idx):
                bits |= v << (3 * i)
            out += bytes([a0, a1]) + bits.to_bytes(6, "little")
            opaque = blk[alpha > 8][:, :3]
            rgb = blk[:, :3].copy()
            if len(opaque):
                rgb[alpha <= 8] = opaque.mean(axis=0)
                lo, hi = rgb.min(axis=0), rgb.max(axis=0)
                inset = (hi - lo) / 16.0
                lo, hi = lo + inset, hi - inset
            else:
                lo = hi = np.zeros(3)
            c0, c1 = to565(hi), to565(lo)
            if c0 < c1:
                c0, c1 = c1, c0
            if c0 == c1:
                c0 = min(c0 + 1, 0xFFFF) if c1 < 0xFFFF else 0xFFFF
                c1 = c1 if c1 < 0xFFFF else 0xFFFE
                if c0 < c1:
                    c0, c1 = c1, c0
            p0, p1 = from565(c0), from565(c1)
            colors = [p0, p1, (2 * p0 + p1) / 3, (p0 + 2 * p1) / 3]
            ci = 0
            for i in range(16):
                ci |= int(np.argmin([np.sum((rgb[i] - c) ** 2) for c in colors])) << (2 * i)
            out += c0.to_bytes(2, "little") + c1.to_bytes(2, "little") + ci.to_bytes(4, "little")
    return bytes(out)


def build(fontpath):
    f_big = font(fontpath, 104)
    f_mid = font(fontpath, 84)
    f_trig = font(fontpath, 78)
    f_st = font(fontpath, 112)
    return {
        "a": face("cross", BLUE), "b": face("circle", RED), "x": face("square", PINK), "y": face("triangle", GREEN),
        "lb": pill("L1", False, f_mid), "rb": pill("R1", False, f_mid),
        "lt": pill("L2", True, f_trig), "rt": pill("R2", True, f_trig),
        "start": oval("options"), "back": oval("touchpad"),
        "stick_l": stick("L", f_st), "stick_r": stick("R", f_st),
        "stick_l2": stick("L3", f_mid), "stick_r2": stick("R3", f_mid),
    }


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--font", default="/System/Library/Fonts/Supplemental/Arial Bold.ttf")
    ap.add_argument("--preview")
    ap.add_argument("--out", default=os.path.join(os.path.dirname(__file__), "../../platform/ps5/ps_glyphs.inc"))
    args = ap.parse_args()
    glyphs = build(args.font)
    lines = ["// Generated by tools/scripts/make_ps_glyphs.py: DXT5 data (32x32, then its 16x16 mip) for the",
             "// PlayStation controller glyphs, by the suffix of the game's controller_icons_xbox_* textures.", ""]
    sheet = Image.new("RGBA", (len(glyphs) * 140, 140), (50, 50, 50, 255))
    for i, (name, im) in enumerate(glyphs.items()):
        base, mip = downsample(im, 32), downsample(im, 16)
        data = encode_bc3(base) + encode_bc3(mip)
        assert len(data) == 1280
        lines.append(f'{{"{name}", {{' + ",".join(str(b) for b in data) + "}},")
        bg = Image.new("RGBA", (32, 32), (50, 50, 50, 255))
        bg.alpha_composite(Image.fromarray(base, "RGBA"))
        sheet.paste(bg.resize((128, 128), Image.NEAREST), (i * 140 + 6, 6))
    with open(args.out, "w") as f:
        f.write("\n".join(lines) + "\n")
    if args.preview:
        sheet.save(args.preview)
    print("wrote", os.path.abspath(args.out), len(glyphs), "glyphs")


if __name__ == "__main__":
    main()
