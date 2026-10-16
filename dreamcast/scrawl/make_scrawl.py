#!/usr/bin/env python3
"""Scratch "n't" into the title screen's beach, after the Nintendo logo.

usage: make_scrawl.py ASSETS_DIR OUT_NORMALS.png OUT_HEADER.h [PREVIEW.png]

The title screen (ASSET_LEVEL_TITLESCREENSEQUENCE) has the Nintendo logo
pressed into the sand: one translucent batch of its level model, textured
with ASSET_TEX3D_COMMON_NINTENDOLOGO (128x32, clamped). This finds where that
texture's rectangle lies on the beach and places "n't" right after it at the
same scale, as if someone had drawn it with a finger: wobbly strokes, a round
groove with a little sand pushed up along its sides.

OUT_NORMALS.png is the groove's normal map, for pvrtex -f NORMAL; the port
draws it as the game's one bump-mapped surface (dreamcast/scrawl.c).
OUT_HEADER.h has the level, segment and batch to draw after, and a grid of
vertices on the sand: world position and texture coordinates.
"""
import json
import math
import random
import struct
import sys
import zlib
from pathlib import Path

import numpy as np

LEVEL = 23                 # ASSET_LEVEL_TITLESCREENSEQUENCE
LOGO_TEXTURE = 222         # ASSET_TEX3D_COMMON_NINTENDOLOGO
LOGO_W, LOGO_H = 128, 32   # texels; level UVs are in 1/32 texel
GAP = 3                    # texels between the logo's frame and the "n"
WIDTH = 44                 # texels the scrawl covers, at the logo's scale: the letters and their ridges
TEX = 512                  # normal map resolution, square as mipmaps must be; VQ compressed by pvrtex
GRID_U, GRID_V = 13, 7     # vertices along and across, close enough to follow the sand
LIFT = 1.0                 # world units above the sand


def level_model(assets):
    headers = json.loads((assets / "asset_level_headers.meta.json").read_text())["files"]
    models = json.loads((assets / "asset_level_models.meta.json").read_text())["files"]
    header = json.loads((assets / "levels/headers" / headers["sections"][headers["order"][LEVEL]]["filename"]).read_text())
    desc = assets / "levels/models" / models["sections"][header["model"]]["filename"]
    raw = (desc.parent / json.loads(desc.read_text())["raw"]).read_bytes()
    return zlib.decompress(raw[5:], -15)


def triangles(model):
    """Every level triangle: (segment, batch, texture id, world points, uvs)."""
    textures_at, segments_at = struct.unpack_from(">II", model, 0)
    texture_ids = [struct.unpack_from(">I", model, textures_at + 8 * i)[0]
                   for i in range(struct.unpack_from(">h", model, 0x18)[0])]
    for s in range(struct.unpack_from(">h", model, 0x1A)[0]):
        base = segments_at + 0x44 * s
        verts_at, tris_at, _, batches_at = struct.unpack_from(">IIII", model, base)
        for b in range(struct.unpack_from(">h", model, base + 0x20)[0]):
            tex, _, v0, f0, _, _, _ = struct.unpack_from(">BbhhBBI", model, batches_at + 12 * b)
            f1 = struct.unpack_from(">h", model, batches_at + 12 * (b + 1) + 4)[0]
            tex_id = texture_ids[tex] if tex != 0xFF else None
            for k in range(f0, f1):
                _, a, c, d = struct.unpack_from(">BBBB", model, tris_at + 16 * k)
                uv = struct.unpack_from(">6h", model, tris_at + 16 * k + 4)
                pts = [np.array(struct.unpack_from(">hhh", model, verts_at + 10 * (v0 + x)), float) for x in (a, c, d)]
                yield s, b, tex_id, pts, [np.array(uv[i:i + 2], float) for i in (0, 2, 4)]


def logo_frame(model):
    """The affine map from logo UVs to world, from the logo triangle holding the logo's origin."""
    for s, b, tex, pts, uvs in triangles(model):
        if tex != LOGO_TEXTURE:
            continue
        m = np.array([uvs[1] - uvs[0], uvs[2] - uvs[0]]).T
        coeffs = np.linalg.solve(m, -uvs[0])
        if coeffs.min() >= -1e-6 and coeffs.sum() <= 1 + 1e-6:
            du = np.linalg.solve(m, np.array([32.0, 0.0]))
            dv = np.linalg.solve(m, np.array([0.0, 32.0]))
            edge1, edge2 = pts[1] - pts[0], pts[2] - pts[0]
            origin = pts[0] + coeffs[0] * edge1 + coeffs[1] * edge2
            return s, b, origin, du[0] * edge1 + du[1] * edge2, dv[0] * edge1 + dv[1] * edge2
    raise SystemExit("the Nintendo logo is not in the title screen's level model")


def sand_height(model, x, z, guess):
    """The level's surface height under (x, z) nearest to guess."""
    best = None
    for _, _, _, pts, _ in triangles(model):
        a, b, c = pts
        d = (b[2] - c[2]) * (a[0] - c[0]) + (c[0] - b[0]) * (a[2] - c[2])
        if abs(d) < 1e-9:
            continue
        l1 = ((b[2] - c[2]) * (x - c[0]) + (c[0] - b[0]) * (z - c[2])) / d
        l2 = ((c[2] - a[2]) * (x - c[0]) + (a[0] - c[0]) * (z - c[2])) / d
        l3 = 1 - l1 - l2
        if min(l1, l2, l3) < -1e-6:
            continue
        y = l1 * a[1] + l2 * b[1] + l3 * c[1]
        if best is None or abs(y - guess) < abs(best - guess):
            best = y
    return guess if best is None else best


def strokes(rng):
    """ "n't" as finger strokes in logo texels (x along, y down from the top of
    the logo), at the logo's letter size: x-height from row 10 to 23."""
    def wobble(points, amount=0.6, steps=6):
        out = []
        for (x0, y0), (x1, y1) in zip(points, points[1:]):
            for i in range(steps):
                t = i / steps
                out.append((x0 + (x1 - x0) * t + rng.uniform(-amount, amount),
                            y0 + (y1 - y0) * t + rng.uniform(-amount, amount)))
        out.append(points[-1])
        return out

    def arc(cx, cy, r, a0, a1, steps=10):
        return [(cx + r * math.cos(a), cy + r * math.sin(a))
                for a in (a0 + (a1 - a0) * i / steps for i in range(steps + 1))]

    # The logo's letters fill rows 8 to 25; these are drawn as big.
    top, base = 8.5, 25.0
    k = (base - top) / 12.5  # the stroke layout was drawn for a 12.5-row x-height
    def at(x, y):
        return (2.0 + (x - 2.0) * k, top + (y - 10.5) * k)
    # n: down stroke, back up, over the hump and down again, one movement.
    n = wobble([at(3.0, 11.0), at(3.4, 23.0)], 0.4) + \
        wobble([at(3.4, 23.0), at(3.2, 14.5)], 0.3) + \
        wobble([at(*p) for p in arc(8.0, 14.7, 4.6, math.pi * 1.05, math.pi * 1.95)], 0.35) + \
        wobble([at(12.7, 14.5), at(13.0, 23.3)], 0.4)
    apostrophe = wobble([at(17.6, 7.0), at(16.9, 11.0)], 0.3, 4)
    t_stem = wobble([at(25.4, 6.5), at(24.9, 21.0)], 0.4) + \
        wobble([at(*p) for p in arc(27.4, 20.6, 2.6, math.pi, math.pi * 0.25)], 0.3)
    t_bar = wobble([at(21.8, 12.1), at(29.3, 11.4)], 0.35)
    return [n, apostrophe, t_stem, t_bar]


def heightfield(rng):
    """Height of the sand over the scrawl, in logo texels: a round groove along
    each stroke, a little ridge of pushed sand beside it, finger pressure varying."""
    xs = (np.arange(TEX) + 0.5) / TEX * WIDTH
    ys = (np.arange(TEX) + 0.5) / TEX * LOGO_H
    gx, gy = np.meshgrid(xs, ys)
    height = np.zeros((TEX, TEX))
    for stroke in strokes(rng):
        pts = np.array(stroke)
        depth = np.full(len(pts), 1.0)
        depth *= 0.85 + 0.3 * np.sin(np.linspace(0, math.pi * rng.uniform(1, 3), len(pts)))  # pressure
        dist = np.full(gx.shape, 1e9)
        weight = np.zeros(gx.shape)
        for (x0, y0), (x1, y1), w0, w1 in zip(pts, pts[1:], depth, depth[1:]):
            seg = np.array([x1 - x0, y1 - y0])
            length2 = seg @ seg or 1e-9
            t = np.clip(((gx - x0) * seg[0] + (gy - y0) * seg[1]) / length2, 0, 1)
            d = np.hypot(gx - (x0 + t * seg[0]), gy - (y0 + t * seg[1]))
            closer = d < dist
            dist = np.where(closer, d, dist)
            weight = np.where(closer, w0 + (w1 - w0) * t, weight)
        radius = 1.7   # texels: a fingertip at the logo's scale
        groove = -np.clip(1 - (dist / radius) ** 2, 0, None) * weight
        ridge = 0.28 * np.exp(-((dist - radius * 1.35) / (radius * 0.45)) ** 2) * weight
        height += groove + ridge
    height = np.maximum(height, -1.2)  # crossing strokes dig no deeper
    return height + rng_noise(rng, TEX, TEX) * 0.03


def rng_noise(rng, width, height):
    field = np.array([[rng.uniform(-1, 1) for _ in range(width)] for _ in range(height)])
    kernel = np.array([1, 4, 6, 4, 1], float) / 16
    for axis in (0, 1):
        field = np.apply_along_axis(lambda r: np.convolve(r, kernel, mode="same"), axis, field)
    return field


def normal_map(height, depth_world, texel_u, texel_v):
    """Normals in texture space (x along u, y along the image rows), from heights."""
    scale = depth_world
    hy, hx = np.gradient(height * scale)
    dx = WIDTH * texel_u / TEX   # world units per normal-map texel, along
    dy = LOGO_H * texel_v / TEX  # and across
    nx, ny, nz = -hx / dx, -hy / dy, np.ones_like(height)
    length = np.sqrt(nx * nx + ny * ny + nz * nz)
    rgb = np.stack([nx / length, ny / length, nz / length], axis=-1)
    return ((rgb * 0.5 + 0.5) * 255).round().clip(0, 255).astype(np.uint8)


def write_png(path, rgb):
    """An 8-bit RGB PNG, without an imaging library."""
    h, w, _ = rgb.shape
    raw = b"".join(b"\0" + rgb[y].tobytes() for y in range(h))

    def chunk(tag, data):
        return struct.pack(">I", len(data)) + tag + data + struct.pack(">I", zlib.crc32(tag + data) & 0xFFFFFFFF)

    Path(path).write_bytes(b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0)) +
                          chunk(b"IDAT", zlib.compress(raw, 9)) + chunk(b"IEND", b""))


def main():
    assets = Path(sys.argv[1])
    model = level_model(assets)
    segment, batch, origin, du, dv = logo_frame(model)
    texel_u, texel_v = np.linalg.norm(du), np.linalg.norm(dv)
    rng = random.Random(1997)
    height = heightfield(rng)
    write_png(sys.argv[2], normal_map(height, 3.0, texel_u, texel_v))
    # Vertices: the logo's rows, from GAP texels past its right edge.
    rows = []
    for j in range(GRID_V):
        for i in range(GRID_U):
            s, t = i / (GRID_U - 1), j / (GRID_V - 1)
            u = LOGO_W + GAP + s * WIDTH
            v = LOGO_H * (1 - t)          # the logo's image is stored flipped
            p = origin + u * du + v * dv
            p[1] = sand_height(model, p[0], p[2], p[1]) + LIFT
            rows.append((p, s, t))
    out = [
        "// Generated by dreamcast/scrawl/make_scrawl.py: where \"n't\" lies on the title beach.",
        "#define SCRAWL_LEVEL %d" % LEVEL,
        "#define SCRAWL_SEGMENT %d" % segment,
        "#define SCRAWL_BATCH %d" % batch,
        "#define SCRAWL_GRID_U %d" % GRID_U,
        "#define SCRAWL_GRID_V %d" % GRID_V,
        "static const float sScrawlGrid[SCRAWL_GRID_U * SCRAWL_GRID_V][5] = {",
    ]
    out += ["    { %.2ff, %.2ff, %.2ff, %.4ff, %.4ff }," % (p[0], p[1], p[2], s, t) for p, s, t in rows]
    out.append("};")
    Path(sys.argv[3]).write_text("\n".join(out) + "\n")
    print(f"{sys.argv[2]}: n't at segment {segment} batch {batch}, {texel_u:.2f} x {texel_v:.2f} world units per logo texel")
    if len(sys.argv) > 4:
        # Lit from the upper left, as the hardware will with the default light.
        nm = normal_map(height, 3.0, texel_u, texel_v).astype(float) / 127.5 - 1
        light = np.array([-0.5, -0.5, 0.7])
        light /= np.linalg.norm(light)
        shade = np.clip(nm @ light, 0, 1)
        flat = light[2]
        image = np.clip(np.array([210, 180, 90])[None, None, :] * (shade / flat)[..., None], 0, 255).astype(np.uint8)
        write_png(sys.argv[4], image.repeat(2, axis=0).repeat(2, axis=1))


if __name__ == "__main__":
    main()
