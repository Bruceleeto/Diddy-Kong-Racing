#!/usr/bin/env python3
"""Make the Dreamcast swirl that replaces the N64 logo on the boot screen.

usage: make_swirl.py OUT.model
       make_swirl.py OUT.gltf [PREVIEW.png]

The outline is the orange path of dreamcast_logo_orange.svg (Wikimedia Commons,
"Dreamcast logo (orange).svg", public domain; the swirl itself is a Sega
trademark). It is flattened, simplified, triangulated and extruded into a slab
the size of the N64 logo model (about 96 units across), facing +Z with +Y up.

The faces are flat; a narrow bevel at each face and the side between them
share their normals, so the edge is rounded (two smoothing groups). The port
lights every vertex itself as the swirl spins (dreamcast/swirl.c), from the
surface written next to the model.

OUT.model is a DKR object model, uncompressed, written directly; the port
embeds it and serves it in place of the N64 logo model (dreamcast/swirl.c).
It is only a stand-in: the port lights and draws the swirl itself, as
triangle strips, from OUT.surface (OUT.model with that suffix),
little-endian: three u32 counts (distinct vertices, strips, strip indices);
each distinct vertex (by position and normal) as its position and unit
normal, six floats; each strip's length, a u16; and the strips' indices of
distinct vertices, u16s. OUT.gltf is the same mesh for viewing.
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
BEVEL = 1.5          # model units the edge is rounded off at each cap (the band is ~7.6 wide)
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


def inset(poly, d):
    """The counter-clockwise outline moved inwards by d, each corner along its
    mitre, no further than 2d (the swirl's tips are sharp)."""
    out = []
    n = len(poly)
    for k in range(n):
        (px, py), (x, y), (nx, ny) = poly[k - 1], poly[k], poly[(k + 1) % n]
        normals = []
        for dx, dy in ((x - px, y - py), (nx - x, ny - y)):
            length = math.hypot(dx, dy) or 1
            normals.append((-dy / length, dx / length))   # left of the edge: inside
        mx, my = normals[0][0] + normals[1][0], normals[0][1] + normals[1][1]
        length = math.hypot(mx, my) or 1
        mx, my = mx / length, my / length
        cos = mx * normals[0][0] + my * normals[0][1]
        reach = min(d / max(cos, 1e-6), 2 * d)
        out.append((x + mx * reach, y + my * reach))
    return out


def build_mesh():
    """The outline, the vertices, their colours, and the triangles in two
    smoothing groups: the flat caps, and the bevels and side that round the
    edge."""
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
    inner = inset(poly, BEVEL)
    caps = triangulate(inner)
    half = DEPTH / 2
    positions, colours = [], []
    faces, edge = [], []
    n = len(poly)
    # Front (+Z) and back cap, the outline inset by the bevel.
    for z in (half, -half):
        for x, y in inner:
            positions.append((x, y, z))
            colours.append(FACE_RGB)
    for a, b, c in caps:
        faces += [a, b, c]                   # front, counter-clockwise from +Z
        faces += [n + a, n + c, n + b]       # back, reversed
    # Edge: rings at the caps, where the bevel starts and ends and at the back,
    # shared by neighbouring quads, in the darker colour.
    base = 2 * n
    rings = ((inner, half), (poly, half - BEVEL), (poly, BEVEL - half), (inner, -half))
    for outline, z in rings:
        for x, y in outline:
            positions.append((x, y, z))
            colours.append(EDGE_RGB)
    for r in range(len(rings) - 1):
        for k in range(n):
            f0, f1 = base + r * n + k, base + r * n + (k + 1) % n
            b0, b1 = f0 + n, f1 + n
            edge += [f0, b0, b1, f0, b1, f1]
    # The edge's rings, as strips: between each ring and the next, all round.
    strips = []
    for r in range(len(rings) - 1):
        strip = []
        for k in range(n + 1):
            strip += [base + r * n + k % n, base + (r + 1) * n + k % n]
        strips.append(strip)
    strips += stripify(faces)
    return poly, positions, colours, [faces, edge], strips


def stripify(indices):
    """Triangles as strips, greedily: each grows across the edge it ends on
    for as long as an unused triangle shares it. Drawn without culling, so the
    strips' alternating winding doesn't matter."""
    tris = [tuple(indices[k:k + 3]) for k in range(0, len(indices), 3)]
    by_edge = {}
    for t, tri in enumerate(tris):
        for e in ((tri[0], tri[1]), (tri[1], tri[2]), (tri[2], tri[0])):
            by_edge.setdefault(frozenset(e), []).append(t)
    used = [False] * len(tris)

    def grow(strip):
        taken = []
        while True:
            edge = frozenset(strip[-2:])
            nxt = next((u for u in by_edge.get(edge, ()) if not used[u] and u not in taken), None)
            if nxt is None:
                return strip, taken
            taken.append(nxt)
            strip.append(next(v for v in tris[nxt] if v not in edge))

    strips = []
    for t, tri in enumerate(tris):
        if used[t]:
            continue
        used[t] = True
        # Whichever way round the first triangle goes furthest.
        strip, taken = max((grow(list(tri[k:] + tri[:k])) for k in range(3)), key=lambda st: len(st[1]))
        for u in taken:
            used[u] = True
        strips.append(strip)
    return strips


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
# header, batches, triangles, vertices, then a per-vertex animation slot map.
# The port draws the swirl itself, so the game's model is a stand-in: one
# degenerate triangle, and the swirl's radius for the game's visibility test.
HEADER_BYTES = 0x58
BATCH_VERTEX_COLOURS = 0xFF
RENDER_FLAGS = 0x3        # as the N64 logo's batches


def vertex_normals(positions, groups):
    """A unit normal per vertex: the area-weighted face normals of its smoothing
    group at its position as rounded in the model. The faces are those before
    rounding, which leaves some slivers of the caps flat or flipped."""
    at = [tuple(round(c) for c in p) for p in positions]
    sums = {}
    owner = {}
    for group, indices in enumerate(groups):
        for k in range(0, len(indices), 3):
            a, b, c = (positions[v] for v in indices[k:k + 3])
            u = [b[i] - a[i] for i in range(3)]
            w = [c[i] - a[i] for i in range(3)]
            n = (u[1] * w[2] - u[2] * w[1], u[2] * w[0] - u[0] * w[2], u[0] * w[1] - u[1] * w[0])
            for v in indices[k:k + 3]:
                key = (at[v], group)
                owner[v] = key
                acc = sums.setdefault(key, [0.0, 0.0, 0.0])
                for i in range(3):
                    acc[i] += n[i]
    normals = {}
    for v, key in owner.items():
        x, y, z = sums[key]
        length = math.sqrt(x * x + y * y + z * z) or 1
        normals[v] = (x / length, y / length, z / length)
    return normals


def write_object_model(path, positions, groups, strips):
    """The stand-in model at path, and the surface the port draws next to it."""
    n_verts, n_tris = 3, 1
    batches_at = HEADER_BYTES
    tris_at = batches_at + 12 * 2
    verts_at = tris_at + 16 * n_tris
    anim_at = (verts_at + 10 * n_verts + 15) & ~15
    end = (anim_at + 2 * n_verts + 15) & ~15
    model = bytearray(end)
    radius = max(math.sqrt(x * x + y * y + z * z) for x, y, z in positions)
    struct.pack_into(">IIIIII", model, 0x00, 0, verts_at, tris_at, 0, 0, anim_at)
    struct.pack_into(">hhIhhhhh", model, 0x18, 0, 0, anim_at, 0, 0, n_verts, n_tris, 1)
    struct.pack_into(">hIhhI", model, 0x2A, -1, end, 0, 0, 0)
    struct.pack_into(">IfIIhhI", model, 0x38, batches_at, radius, 0, 0, 0, 0, anim_at)
    struct.pack_into(">BbhhBBI", model, batches_at, 0xFF, 0, 0, 0, BATCH_VERTEX_COLOURS, 0, RENDER_FLAGS)
    struct.pack_into(">BbhhBBI", model, batches_at + 12, 0xFF, 0, n_verts, n_tris, BATCH_VERTEX_COLOURS, 0, 0)
    struct.pack_into(">BBBB", model, tris_at, 0, 0, 1, 2)
    for v in range(n_verts):
        struct.pack_into(">hhhBBBB", model, verts_at + 10 * v, 0, 0, 0, 255, 255, 255, 255)
    Path(path).write_bytes(bytes(model))

    # The surface: distinct vertices by position (as rounded) and normal.
    normals = vertex_normals(positions, groups)
    distinct = {}

    def point(v):
        x, y, z = positions[v]
        return distinct.setdefault(((round(x), round(y), round(z)), normals[v]), len(distinct))

    strips = [[point(v) for v in strip] for strip in strips]
    flat = [i for strip in strips for i in strip]
    surface = struct.pack("<3I", len(distinct), len(strips), len(flat))
    surface += b"".join(struct.pack("<6f", *p, *n) for p, n in distinct)
    surface += struct.pack(f"<{len(strips)}H", *(len(strip) for strip in strips))
    surface += struct.pack(f"<{len(flat)}H", *flat)
    Path(path).with_suffix(".surface").write_bytes(surface)
    return len(distinct), len(strips), len(flat), sum(len(t) - 2 for t in strips)


def main():
    poly, positions, colours, groups, strips = build_mesh()
    if sys.argv[1].endswith(".model"):
        points, count, verts, tris = write_object_model(sys.argv[1], positions, groups, strips)
        print(f"{sys.argv[1]}: {points} distinct vertices, {count} strips of {verts} vertices, {tris} triangles")
        return
    indices = groups[0] + groups[1]
    write_gltf(sys.argv[1], positions, colours, indices)
    print(f"{sys.argv[1]}: {len(poly)} outline points, {len(positions)} vertices, {len(indices) // 3} triangles")
    if len(sys.argv) > 2:
        preview(sys.argv[2], poly, triangulate(poly))


if __name__ == "__main__":
    main()
