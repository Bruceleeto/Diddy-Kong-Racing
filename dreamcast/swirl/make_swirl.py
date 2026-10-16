#!/usr/bin/env python3
"""Make the Dreamcast swirl that replaces the N64 logo on the boot screen.

usage: make_swirl.py OUT.model|OUT.gltf [PREVIEW.png]

The outline is the orange path of dreamcast_logo_orange.svg (Wikimedia Commons,
"Dreamcast logo (orange).svg", public domain; the swirl itself is a Sega
trademark). It is flattened, simplified, triangulated and extruded into a slab
the size of the N64 logo model (about 96 units across), facing +Z with +Y up,
coloured through its vertex colours: the logo's orange on the faces, darker on
the edge. OUT.model is a DKR object model, uncompressed, written directly
(dkr_assets_tool's own glTF import crashes on it); the port embeds it and
serves it in place of the N64 logo model (dreamcast/swirl.c). OUT.gltf is the
same mesh for viewing.
"""
import base64
import json
import math
import re
import struct
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
SVG = HERE / "dreamcast_logo_orange.svg"
SWIRL_FILL = "#ED8432"
SIZE = 112.0         # model units across the larger side; the N64 logo is 96, but solid
DEPTH = 14.0         # model units front to back
FLATNESS = 0.2       # SVG units: how far the outline may stray from the curves
FACE_RGB = (0xED, 0x84, 0x32)
EDGE_RGB = (0xB0, 0x5A, 0x18)


def swirl_path():
    svg = SVG.read_text()
    for attrs in re.findall(r"<path\b(.*?)/>", svg, re.S):
        if SWIRL_FILL in attrs:
            return re.search(r'\bd="([^"]+)"', attrs).group(1)
    raise SystemExit(f"{SVG}: no {SWIRL_FILL} path")


def tokens(d):
    for match in re.finditer(r"[MmLlHhVvCcSsQqTtZz]|-?(?:\d+\.?\d*|\.\d+)(?:[eE][-+]?\d+)?", d):
        yield match.group(0)


def cubic(p0, p1, p2, p3, steps):
    out = []
    for i in range(1, steps + 1):
        t = i / steps
        u = 1 - t
        out.append((u**3 * p0[0] + 3 * u * u * t * p1[0] + 3 * u * t * t * p2[0] + t**3 * p3[0],
                    u**3 * p0[1] + 3 * u * u * t * p1[1] + 3 * u * t * t * p2[1] + t**3 * p3[1]))
    return out


def outlines(d):
    """Closed polygons of an SVG path; curves become 16 segments each."""
    items = list(tokens(d))
    polygons, points = [], []
    pos = start = (0.0, 0.0)
    last_control = None
    command = None
    i = 0

    def number():
        nonlocal i
        value = float(items[i])
        i += 1
        return value

    while i < len(items):
        if items[i].isalpha():
            command = items[i]
            i += 1
        relative = command.islower()
        op = command.upper()
        base = pos if relative else (0.0, 0.0)
        if op == "Z":
            if points:
                polygons.append(points)
            points = []
            pos = start
            last_control = None
            command = None
            continue
        if op == "M":
            if points:
                polygons.append(points)
            pos = start = (base[0] + number(), base[1] + number())
            points = [pos]
            command = "l" if relative else "L"  # further pairs are lines
            last_control = None
        elif op == "L":
            pos = (base[0] + number(), base[1] + number())
            points.append(pos)
            last_control = None
        elif op == "H":
            pos = ((pos[0] if relative else 0.0) + number(), pos[1])
            points.append(pos)
            last_control = None
        elif op == "V":
            pos = (pos[0], (pos[1] if relative else 0.0) + number())
            points.append(pos)
            last_control = None
        elif op in "CS":
            if op == "C":
                c1 = (base[0] + number(), base[1] + number())
            else:
                c1 = (2 * pos[0] - last_control[0], 2 * pos[1] - last_control[1]) if last_control else pos
            c2 = (base[0] + number(), base[1] + number())
            end = (base[0] + number(), base[1] + number())
            points += cubic(pos, c1, c2, end, 16)
            pos, last_control = end, c2
        else:
            raise SystemExit(f"unsupported SVG path command {command}")
    if points:
        polygons.append(points)
    return polygons


def simplify(points, tolerance):
    """Douglas-Peucker on a closed polygon."""
    def run(pts):
        if len(pts) < 3:
            return pts
        (ax, ay), (bx, by) = pts[0], pts[-1]
        length = math.hypot(bx - ax, by - ay) or 1e-9
        worst, index = 0.0, 0
        for k in range(1, len(pts) - 1):
            dist = abs((bx - ax) * (ay - pts[k][1]) - (ax - pts[k][0]) * (by - ay)) / length
            if dist > worst:
                worst, index = dist, k
        if worst <= tolerance:
            return [pts[0], pts[-1]]
        return run(pts[:index + 1])[:-1] + run(pts[index:])
    # Split at the point farthest from the first, so both halves are open chains.
    far = max(range(len(points)), key=lambda k: math.dist(points[0], points[k]))
    first = run(points[:far + 1])
    second = run(points[far:] + [points[0]])
    return first[:-1] + second[:-1]


def area(poly):
    return sum(poly[k][0] * poly[(k + 1) % len(poly)][1] - poly[(k + 1) % len(poly)][0] * poly[k][1]
               for k in range(len(poly))) / 2


def triangulate(poly):
    """Ear clipping of a simple counter-clockwise polygon; indices into poly."""
    def cross(o, a, b):
        return (a[0] - o[0]) * (b[1] - o[1]) - (a[1] - o[1]) * (b[0] - o[0])

    def inside(p, a, b, c):
        return cross(a, b, p) >= 0 and cross(b, c, p) >= 0 and cross(c, a, p) >= 0

    remaining = list(range(len(poly)))
    triangles = []
    guard = 0
    while len(remaining) > 3 and guard < 100000:
        guard += 1
        n = len(remaining)
        for k in range(n):
            i0, i1, i2 = remaining[(k - 1) % n], remaining[k], remaining[(k + 1) % n]
            a, b, c = poly[i0], poly[i1], poly[i2]
            if cross(a, b, c) <= 1e-12:
                continue  # reflex or flat corner
            if any(inside(poly[j], a, b, c) for j in remaining if j not in (i0, i1, i2)):
                continue
            triangles.append((i0, i1, i2))
            del remaining[k]
            break
        else:
            raise SystemExit("triangulation failed: the outline is not simple")
    triangles.append(tuple(remaining))
    return triangles


def build_mesh():
    polygons = outlines(swirl_path())
    poly = max(polygons, key=lambda p: abs(area(p)))
    poly = simplify(poly, FLATNESS)
    # SVG y grows down: flip, centre and scale to SIZE.
    xs = [p[0] for p in poly]
    ys = [p[1] for p in poly]
    cx, cy = (min(xs) + max(xs)) / 2, (min(ys) + max(ys)) / 2
    scale = SIZE / max(max(xs) - min(xs), max(ys) - min(ys))
    poly = [((x - cx) * scale, -(y - cy) * scale) for x, y in poly]
    if area(poly) < 0:
        poly.reverse()
    caps = triangulate(poly)
    half = DEPTH / 2
    positions, colours, indices = [], [], []
    n = len(poly)
    # Front (+Z) and back cap.
    for z in (half, -half):
        for x, y in poly:
            positions.append((x, y, z))
            colours.append(FACE_RGB)
    for a, b, c in caps:
        indices += [a, b, c]                 # front, counter-clockwise from +Z
        indices += [n + a, n + c, n + b]     # back, reversed
    # Edge: a ring of vertices at each face, shared by neighbouring quads, in
    # the darker colour.
    base = 2 * n
    for z in (half, -half):
        for x, y in poly:
            positions.append((x, y, z))
            colours.append(EDGE_RGB)
    for k in range(n):
        f0, f1 = base + k, base + (k + 1) % n
        b0, b1 = f0 + n, f1 + n
        indices += [f0, b0, b1, f0, b1, f1]
    return poly, positions, colours, indices


def write_gltf(path, positions, colours, indices):
    pos = b"".join(struct.pack("<3f", *p) for p in positions)
    col = b"".join(struct.pack("<4B", *c, 255) for c in colours)
    idx = b"".join(struct.pack("<H", i) for i in indices)
    idx += bytes(-len(idx) % 4)
    blob = pos + col + idx
    mins = [min(p[k] for p in positions) for k in range(3)]
    maxs = [max(p[k] for p in positions) for k in range(3)]
    gltf = {
        "asset": {"version": "2.0", "generator": "dreamcast/swirl/make_swirl.py"},
        "scene": 0,
        "scenes": [{"nodes": [0]}],
        "nodes": [{"name": "swirl", "mesh": 0}],
        "materials": [{"name": "swirl", "pbrMetallicRoughness": {"baseColorFactor": [1, 1, 1, 1]}}],
        "meshes": [{"primitives": [{"attributes": {"POSITION": 0, "COLOR_0": 1}, "indices": 2, "material": 0}]}],
        "buffers": [{"byteLength": len(blob),
                     "uri": "data:application/octet-stream;base64," + base64.b64encode(blob).decode()}],
        "bufferViews": [
            {"buffer": 0, "byteOffset": 0, "byteLength": len(pos), "target": 34962},
            {"buffer": 0, "byteOffset": len(pos), "byteLength": len(col), "target": 34962},
            {"buffer": 0, "byteOffset": len(pos) + len(col), "byteLength": len(indices) * 2, "target": 34963},
        ],
        "accessors": [
            {"bufferView": 0, "componentType": 5126, "count": len(positions), "type": "VEC3", "min": mins, "max": maxs},
            {"bufferView": 1, "componentType": 5121, "normalized": True, "count": len(colours), "type": "VEC4"},
            {"bufferView": 2, "componentType": 5123, "count": len(indices), "type": "SCALAR"},
        ],
    }
    Path(path).write_text(json.dumps(gltf, indent=1))


def preview(path, poly, caps_tris):
    import matplotlib
    matplotlib.use("Agg")
    import matplotlib.pyplot as plt
    fig, ax = plt.subplots(figsize=(4, 4), facecolor="black")
    for a, b, c in caps_tris:
        tri = [poly[a], poly[b], poly[c]]
        ax.fill([p[0] for p in tri], [p[1] for p in tri], color="#ED8432", linewidth=0.2, edgecolor="#a05010")
    ax.set_aspect("equal")
    ax.axis("off")
    fig.savefig(path, dpi=100, facecolor="black")


# DKR object model (include/structs.h ObjectModel, Vertex, Triangle,
# TriangleBatchInfo), big-endian, in the order the game's own models use:
# header, batches, triangles, vertices, then a per-vertex animation slot map,
# unused here but present as in every model the game loads.
HEADER_BYTES = 0x58
BATCH_VERTICES = 24       # as the largest batches in the game's models
BATCH_TRIANGLES = 16      # one G_TRIN draws at most 16 (include/f3ddkr.h)
TRI_DRAW_BACKFACE = 0x40
BATCH_VERTEX_COLOURS = 0xFF
RENDER_FLAGS = 0x3        # as the N64 logo's batches


def batches(positions, colours, indices):
    """Triangles grouped into batches of at most BATCH_VERTICES vertices and
    BATCH_TRIANGLES triangles each."""
    out, current, local = [], [], {}
    for k in range(0, len(indices), 3):
        tri = indices[k:k + 3]
        new = [v for v in dict.fromkeys(tri) if v not in local]
        if len(local) + len(new) > BATCH_VERTICES or len(current) == BATCH_TRIANGLES:
            out.append((list(local), current))
            current, local = [], {}
            new = list(dict.fromkeys(tri))
        for v in new:
            local[v] = len(local)
        current.append(tuple(local[v] for v in tri))
    if current:
        out.append((list(local), current))
    return out


def write_object_model(path, positions, colours, indices):
    groups = batches(positions, colours, indices)
    n_verts = sum(len(v) for v, _ in groups)
    n_tris = sum(len(t) for _, t in groups)
    batches_at = HEADER_BYTES
    tris_at = batches_at + 12 * (len(groups) + 1)
    verts_at = tris_at + 16 * n_tris
    anim_at = (verts_at + 10 * n_verts + 15) & ~15
    end = (anim_at + 2 * n_verts + 15) & ~15
    model = bytearray(end)
    radius = max(math.sqrt(x * x + y * y + z * z) for x, y, z in positions)
    struct.pack_into(">IIIIII", model, 0x00, 0, verts_at, tris_at, 0, 0, anim_at)
    struct.pack_into(">hhIhhhhh", model, 0x18, 0, 0, anim_at, 0, 0, n_verts, n_tris, len(groups))
    struct.pack_into(">hIhhI", model, 0x2A, -1, end, 0, 0, 0)
    struct.pack_into(">IfIIhhI", model, 0x38, batches_at, radius, 0, 0, 0, 0, anim_at)
    vo = to = 0
    for b, (verts, tris) in enumerate(groups):
        struct.pack_into(">BbhhBBI", model, batches_at + 12 * b, 0xFF, 0, vo, to, BATCH_VERTEX_COLOURS, 0, RENDER_FLAGS)
        for v in verts:
            x, y, z = positions[v]
            r, g, bl = colours[v]
            struct.pack_into(">hhhBBBB", model, verts_at + 10 * vo, round(x), round(y), round(z), r, g, bl, 255)
            vo += 1
        for a, c, d in tris:
            struct.pack_into(">BBBB", model, tris_at + 16 * to, TRI_DRAW_BACKFACE, a, c, d)
            to += 1
    struct.pack_into(">BbhhBBI", model, batches_at + 12 * len(groups), 0xFF, 0, vo, to, BATCH_VERTEX_COLOURS, 0, 0)
    Path(path).write_bytes(bytes(model))
    return len(groups), n_verts, n_tris, len(model)


def main():
    poly, positions, colours, indices = build_mesh()
    if sys.argv[1].endswith(".model"):
        groups, verts, tris, size = write_object_model(sys.argv[1], positions, colours, indices)
        print(f"{sys.argv[1]}: {groups} batches, {verts} vertices, {tris} triangles, {size} bytes")
        if len(sys.argv) > 2:
            preview(sys.argv[2], poly, triangulate(poly))
        return
    write_gltf(sys.argv[1], positions, colours, indices)
    print(f"{sys.argv[1]}: {len(poly)} outline points, {len(positions)} vertices, {len(indices) // 3} triangles")
    if len(sys.argv) > 2:
        preview(sys.argv[2], poly, triangulate(poly))


if __name__ == "__main__":
    main()
