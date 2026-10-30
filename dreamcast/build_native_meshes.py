#!/usr/bin/env python3
"""Compile DKR's level geometry and object models to ready PVR data, offline.

usage: build_native_meshes.py ASSETS_BIN ASSETS_LUT MATERIAL_TABLES_JSON OUT

The game hands the RDP a material for every batch of a level: a texture, some
render flags, a vertex load and sixteen triangles at most. The display-list walk
turned that back into a PVR header and colours on every frame. This does it once:
it follows src/textures_sprites.c material_set, the walk's render-mode and
combiner code (dreamcast/main.c) and dreamcast/gfx.c's header compile, and
writes what they would all have come to. A frame then only culls, transforms and
sends (dreamcast/native.c).

What cannot be decided here stays with the walk, batch by batch: a batch whose
material the game varies per frame (Spaceport Alpha's pulsing lights, the level
header's colour cycles), or one drawn from a paletted texture, which the texture
pack does not hold. A batch's list and depth modes must also not depend on the
game's anti-aliasing setting.

Triangles become strips within a batch: 1.9 TA vertices a triangle instead of 3
over all the levels. Back faces are left to the PVR, which is what makes
strips possible at all: a corner written is one more the strip can reuse, and a
triangle a CPU cull took out would break the strip around it. Two-sided triangles
go in a second part of the batch, under a header without culling. Opaque and
punch-through batches may reorder their triangles to find longer strips. A
translucent one keeps its order, as what it blends depends on it. Every batch is
decoded again here and compared with the triangles it came from.

Object models keep what only a frame knows: vertices that animate, colours from
the lights, a material that turns translucent as the object fades. So a model's
block holds the strips and the texture coordinates, and each batch's material
four times over, for opaque or fading and for plain or directional lighting.
Its colours and positions come from the object each frame.

Pack, little endian: "DNPK", u32 version, u32 level count, u32 model count, then
per level model, then per object model, u32 offset, u32 bytes (0 for none). Each is
a block the game loads whole and relocates; offsets inside it count from its
start, which is 32-byte aligned. A level:
  header  "DNML", u32 model size, counts and table offsets (HEADER below)
  mats    32 bytes each: the PVR polygon header, without the texture's size,
          format and address, which the console fills in at load from the
          texture as it put it in video memory (a texture's place there is not
          known here)
  infos   20 bytes per mat: route (0 opaque, 1 punch-through, 2 translucent),
          decal flag, level texture index or -1, texture frame, then the combiner
          (two words and the cycle type) for what a frame's colours decide
  segs    8 bytes each: u32 offset of its u16 per batch (chunk index, 0xFFFF the
          walk draws it, 0xFFFE drawn by nobody), u16 batch count, u16 opaque batch count
  chunks  48 bytes each: f32 box centre and half size, u32 offsets of its
          positions and corners, u16 positions, corners one-sided, corners
          two-sided, mat, flags (1 water, 2 animated texture, 4 translucent pass)
          and batch, u32 reserved
  xyz     f32 x, y, z per position
  corners 16 bytes each: f32 u, v, u32 ARGB, then PVR command in the top nibble
          and the position index in the low 16 bits
An object model, the same way:
  header  "DNMO", u32 model file size, then as MODEL_HEADER below
  mats, infos  as a level's; a mat is always followed by its two-sided twin
  chunks  32 bytes per batch: u16 first vertex, vertex count, corners one-sided,
          corners two-sided, flags (8 environment-mapped), u16 reserved, u32 offset of
          its corners, u16 mat per variant (bit 0 fading, bit 1 directional
          lighting; 0xFFFF the walk draws it), u32 reserved
  corners 16 bytes each: f32 u, v, then in place of the colour the triangle (from
          the model's first) times 4 plus the corner, as above otherwise
"""
import collections
import json
import struct
import sys
import zlib
from pathlib import Path

VERSION = 2

# include/asset_enums.h, AssetSectionsEnum
TEXTURES_3D, TEXTURES_3D_TABLE = 2, 3
LEVEL_MODELS_TABLE, LEVEL_MODELS = 26, 27
OBJECT_MODELS_TABLE, OBJECT_MODELS = 28, 29

# src/textures_sprites.h, RenderFlags
RENDER_ANTI_ALIASING = 1 << 0
RENDER_Z_COMPARE = 1 << 1
RENDER_SEMI_TRANSPARENT = 1 << 2
RENDER_FOG_ACTIVE = 1 << 3
RENDER_CUTOUT = 1 << 4
RENDER_COLOUR_INDEX = 1 << 5
RENDER_CLAMP_X = 1 << 6
RENDER_CLAMP_Y = 1 << 7
RENDER_Z_UPDATE = RENDER_HIDDEN = 1 << 8
RENDER_DECAL = 1 << 11
RENDER_WATER = 1 << 13
RENDER_TEX_ANIM = 1 << 16
RENDER_ENVMAP = 1 << 15
RENDER_PULSING_LIGHTS = 1 << 18
RENDER_VTX_ALPHA = 1 << 27
BACKFACE_DRAW = 0x40

# include/PR/gbi.h
G_FOG, G_ZBUFFER = 0x00010000, 0x00000001
G_MDSFT_CYCLETYPE, G_MDSFT_TEXTFILT = 20, 12
G_CYC_2CYCLE = 1 << G_MDSFT_CYCLETYPE
G_TF_POINT = 0
Z_CMP, Z_UPD, ZMODE_DEC, ZMODE_MASK = 0x10, 0x20, 0xC00, 0xC00
FORCE_BL, CVG_X_ALPHA = 0x4000, 0x1000
G_BL_CLR_MEM = 1
G_AC_THRESHOLD = 1

# KOS's dc/pvr.h, as dreamcast/gfx.c compile_header_uncached uses it.
LIST_OP, LIST_TR, LIST_PT = 0, 2, 4
ROUTE_OP, ROUTE_PT, ROUTE_TR = 0, 1, 2
CULL_NONE, CULL_CW = 0, 3
CMD_VERTEX, CMD_VERTEX_EOL = 0xE0000000, 0xF0000000

# dreamcast/main.c, the slots of a compiled combiner.
SRC_SHADE, SRC_COMB, SRC_ZERO, SRC_ONE, SRC_TEXEL, SRC_PRIM, SRC_ENV = 0, 4, 8, 9, 10, 11, 15

CHUNK_WATER, CHUNK_TEX_ANIM, CHUNK_TRANSLUCENT, CHUNK_ENVMAP = 1, 2, 4, 8
BATCH_WALK, BATCH_NONE = 0xFFFF, 0xFFFE


def sections(lut):
    """(start, end) in assets.bin of each section, from the LUT: a count, then the starts."""
    words = struct.unpack(f">{len(lut) // 4}I", lut)
    starts = words[1:]
    return [(starts[i], starts[i + 1]) for i in range(len(starts) - 1)]


def table(data, sec, index):
    """A section's offset table, as big-endian u32s up to the -1 that ends it."""
    lo, hi = sec[index]
    words = struct.unpack(f">{(hi - lo) // 4}I", data[lo:hi - (hi - lo) % 4])
    return list(words[: words.index(0xFFFFFFFF)] if 0xFFFFFFFF in words else words)


def next_pot(value):
    """dreamcast/gfx.c: the smallest power of two >= v, within the PVR's [8, 1024]."""
    pot = 8
    while pot < value and pot < 1024:
        pot *= 2
    return pot


def texture_header(data, sec, offsets, index):
    """What a 3D texture asset's header says, from the head of its record."""
    start = sec[TEXTURES_3D][0] + offsets[index]
    width, height, fmt = data[start], data[start + 1], data[start + 2]
    flags = struct.unpack_from(">h", data, start + 6)[0] & 0xFFFF
    frames = struct.unpack_from(">H", data, start + 0x12)[0]
    return {"width": width, "height": height, "format": fmt & 0xF, "flags": flags, "frames": frames}


def wrap_modes(tex):
    """src/textures_sprites.c material_init: (cms, cmt), the RDP's clamp bit 2 set when clamped."""
    u_clamp = v_clamp = True
    size = 1
    for _ in range(7):
        if size == tex["width"]:
            u_clamp = False
        if size == tex["height"]:
            v_clamp = False
        size *= 2
    return (2 if u_clamp or tex["flags"] & RENDER_CLAMP_X else 0, 2 if v_clamp or tex["flags"] & RENDER_CLAMP_Y else 0)


def material(tables, flags, tex, directional=False):
    """src/textures_sprites.c material_set: (w0, w1, othermode H, L, geometry)."""
    if tex is not None:
        flags |= tex["flags"]
    if directional:
        flags &= RENDER_DECAL | RENDER_COLOUR_INDEX | RENDER_ANTI_ALIASING | RENDER_Z_COMPARE | RENDER_SEMI_TRANSPARENT
    else:
        flags &= (RENDER_VTX_ALPHA | RENDER_DECAL | RENDER_Z_UPDATE | RENDER_COLOUR_INDEX | RENDER_CUTOUT |
                  RENDER_FOG_ACTIVE | RENDER_SEMI_TRANSPARENT | RENDER_Z_COMPARE | RENDER_ANTI_ALIASING)
    flags = flags & ~RENDER_FOG_ACTIVE if flags & RENDER_VTX_ALPHA else flags & ~RENDER_Z_UPDATE
    # Directional lighting goes without fog, as the walk draws it.
    geometry = 0 if flags & RENDER_VTX_ALPHA or directional else G_FOG
    if flags & RENDER_Z_COMPARE:
        geometry |= G_ZBUFFER
    if tex is None:
        if flags & RENDER_VTX_ALPHA:
            entry = tables["dRenderSettingsSolidColourVtxAlpha"][flags & (RENDER_ANTI_ALIASING | RENDER_Z_COMPARE)]
        else:
            entry = tables["dRenderSettingsSolidColour"][flags & (RENDER_FOG_ACTIVE | RENDER_SEMI_TRANSPARENT |
                                                                  RENDER_Z_COMPARE | RENDER_ANTI_ALIASING)]
    elif directional and flags & RENDER_DECAL and flags & RENDER_Z_COMPARE:
        index = flags & RENDER_ANTI_ALIASING
        index |= 2 if flags & RENDER_SEMI_TRANSPARENT else 0
        index |= 4 if flags & RENDER_COLOUR_INDEX else 0
        entry = tables["dRenderSettingsPrimOverlay"][index]
    elif directional:
        if flags & RENDER_COLOUR_INDEX:
            flags = (flags ^ RENDER_COLOUR_INDEX) | 8
        entry = tables["dRenderSettingsDirectionalLighting"][flags]
    elif flags & RENDER_DECAL and flags & RENDER_Z_COMPARE:
        index = flags & RENDER_ANTI_ALIASING
        index |= 2 if flags & RENDER_SEMI_TRANSPARENT else 0
        index |= 4 if flags & RENDER_FOG_ACTIVE else 0
        index |= 8 if flags & RENDER_COLOUR_INDEX else 0
        entry = tables["dRenderSettingsDecal"][index]
    elif flags & RENDER_CUTOUT:
        index = flags & (RENDER_ANTI_ALIASING | RENDER_Z_COMPARE | RENDER_SEMI_TRANSPARENT)
        index |= 8 if flags & RENDER_FOG_ACTIVE else 0
        entry = tables["dRenderSettingsCutout"][index]
    elif flags & RENDER_VTX_ALPHA:
        flags &= ~RENDER_DECAL
        index = flags & (RENDER_ANTI_ALIASING | RENDER_Z_COMPARE)
        if flags & RENDER_Z_UPDATE:
            index |= 4
        else:
            geometry |= G_ZBUFFER
        entry = tables["dRenderSettingsVtxAlpha"][index]
    else:
        flags &= ~RENDER_DECAL
        entry = tables["dRenderSettingsCommon"][flags]
    return entry[0] & 0xFFFFFFFF, entry[1], entry[2], entry[3], geometry


def render_state(w0, w1, omh, oml, geometry):
    """dreamcast/main.c apply_render_mode, and the list as gfx_blast_begin picks it."""
    z_on = bool(geometry & G_ZBUFFER)
    depth_test = z_on and bool(oml & Z_CMP)
    depth_write = z_on and bool(oml & Z_UPD)
    decal = z_on and (oml & ZMODE_MASK) == ZMODE_DEC
    two_cycle = (omh & (3 << G_MDSFT_CYCLETYPE)) == G_CYC_2CYCLE
    p = (oml >> (28 if two_cycle else 30)) & 3
    m = (oml >> (20 if two_cycle else 22)) & 3
    blend = bool(oml & FORCE_BL) and (p == G_BL_CLR_MEM or m == G_BL_CLR_MEM)
    alpha_test = bool(oml & CVG_X_ALPHA) or ((oml & 3) == G_AC_THRESHOLD and BLEND_ALPHA > 0)
    point = ((omh >> G_MDSFT_TEXTFILT) & 3) == G_TF_POINT
    fog = bool(geometry & G_FOG)
    if alpha_test:
        route = ROUTE_PT
    elif depth_test:
        route = ROUTE_TR if blend else ROUTE_OP
    else:
        return None  # a level batch always tests depth; nothing here for what would not
    return {"route": route, "depth_test": depth_test, "depth_write": depth_write, "decal": decal,
            "point": point, "fog": fog}


BLEND_ALPHA = 100  # src/tracks.c render_scene: gDPSetBlendColor(0, 0, 0, 100)


def pvr_header(state, textured, wrap, culled):
    """dreamcast/gfx.c compile_header_uncached and KOS's pvr_poly_compile."""
    pvr_list = {ROUTE_OP: LIST_OP, ROUTE_PT: LIST_PT, ROUTE_TR: LIST_TR}[state["route"]]
    opaque = state["route"] == ROUTE_OP
    cmd = 0x80840000 | (pvr_list << 24) | (1 << 2) | (1 << 1)  # specular, gouraud, packed colour, 32-bit UV
    if textured:
        cmd |= 1 << 3
    mode1 = (6 << 29) | ((CULL_CW if culled else CULL_NONE) << 27)
    if not state["depth_write"]:
        mode1 |= 1 << 26
    if textured:
        mode1 |= 1 << 25
    src, dst = (1, 0) if opaque else (4, 5)
    mode2 = (src << 29) | (dst << 26) | ((0 if state["fog"] else 2) << 22) | ((0 if opaque else 1) << 20)
    if textured:
        cms, cmt = wrap
        clamp = (2 if cms & 2 else 0) | (1 if cmt & 2 else 0)
        env = 1 if opaque else 3
        mode2 |= (1 if opaque else 0) << 19
        mode2 |= clamp << 15
        mode2 |= (0 if state["point"] else 1) << 13
        mode2 |= env << 6
        mode2 |= 4 << 8  # PVR_MIPBIAS_NORMAL
    return [cmd, mode1, mode2, 0, 0xFFFFFFFF, 0xFFFFFFFF, 0, 0]


def cc_rgb(mux, ch, comb):
    if mux == 0:
        return comb + ch
    if mux in (1, 2):
        return SRC_TEXEL
    if mux == 6:
        return SRC_ONE
    if mux == 3:
        return SRC_PRIM + ch
    if mux == 4:
        return SRC_SHADE + ch
    if mux == 5:
        return SRC_ENV + ch
    return SRC_ZERO


def cc_mul(mux, ch, comb):
    if mux == 7:
        return comb + 3
    if mux in (8, 9):
        return SRC_TEXEL
    if mux == 10:
        return SRC_PRIM + 3
    if mux == 11:
        return SRC_SHADE + 3
    if mux == 12:
        return SRC_ENV + 3
    return cc_rgb(mux, ch, comb)


def cc_alpha(mux, comb):
    return {0: comb + 3, 1: SRC_TEXEL, 2: SRC_TEXEL, 3: SRC_PRIM + 3, 4: SRC_SHADE + 3, 5: SRC_ENV + 3,
            6: SRC_ONE}.get(mux, SRC_ZERO)


def cc_cycle(slots, comb):
    a, b, c, d, aa, ba, ca, da = slots
    prog = [[cc_rgb(a, ch, comb), cc_rgb(b, ch, comb), cc_mul(c, ch, comb), cc_rgb(d, ch, comb)] for ch in range(3)]
    prog.append([cc_alpha(aa, comb), cc_alpha(ba, comb), cc_alpha(ca, comb), cc_alpha(da, comb)])
    return prog


def combiner(w0, w1, omh):
    """dreamcast/main.c cc_prog_sync: one program per cycle."""
    w0 &= 0xFFFFFFFF
    progs = [cc_cycle(((w0 >> 20) & 0xF, (w1 >> 28) & 0xF, (w0 >> 15) & 0x1F, (w1 >> 15) & 7, (w0 >> 12) & 7,
                       (w1 >> 12) & 7, (w0 >> 9) & 7, (w1 >> 9) & 7), SRC_SHADE)]
    if (omh & (3 << G_MDSFT_CYCLETYPE)) == G_CYC_2CYCLE:
        progs.append(cc_cycle(((w0 >> 5) & 0xF, (w1 >> 24) & 0xF, w0 & 0x1F, (w1 >> 6) & 7, (w1 >> 21) & 7,
                               (w1 >> 3) & 7, (w1 >> 18) & 7, w1 & 7), SRC_COMB))
    return progs


def shade_color(progs, rgba, prim=(255, 255, 255, 255), env=(255, 255, 255, 0)):
    """The colour a corner goes out with: the combiner on the shade, with the texel white."""
    src = [0.0] * 19
    src[SRC_ONE] = 1.0
    src[SRC_TEXEL] = 1.0
    for i in range(4):
        src[SRC_SHADE + i] = rgba[i] / 255.0
        src[SRC_PRIM + i] = prim[i] / 255.0
        src[SRC_ENV + i] = env[i] / 255.0
    for prog in progs:
        out = [min(1.0, max(0.0, (src[p[0]] - src[p[1]]) * src[p[2]] + src[p[3]])) for p in prog]
        src[SRC_COMB:SRC_COMB + 4] = out
    # What the walk's classification comes to: a channel equal to the shade is
    # the shade's own byte, so a full-range lerp cannot round it differently.
    result = []
    for i in range(4):
        value = src[SRC_COMB + i]
        if abs(value - rgba[i] / 255.0) < 1e-6:
            result.append(rgba[i])
        else:
            result.append(max(0, min(255, int(value * 255.0 + 0.5))))
    return (result[3] << 24) | (result[0] << 16) | (result[1] << 8) | result[2]


def strips_ordered(tris):
    """Strips that keep the triangles' order: a new one wherever the next does not continue it."""
    out = []
    for tri in tris:
        if out:
            seq, flip = out[-1]
            x, y = seq[-2], seq[-1]
            first, second = (y, x) if not flip else (x, y)
            order = None
            for rot in range(3):
                t = (tri[rot], tri[(rot + 1) % 3], tri[(rot + 2) % 3])
                if t[0] == first and t[1] == second:
                    order = t
            if order is not None:
                seq.append(order[2])
                out[-1] = (seq, not flip)
                continue
        out.append(([tri[0], tri[1], tri[2]], False))
    return [seq for seq, _ in out]


def strips_greedy(tris):
    """Strips over a batch's triangles in any order, longest first from the least connected."""
    edges = collections.defaultdict(list)
    for i, tri in enumerate(tris):
        for k in range(3):
            edges[(tri[k], tri[(k + 1) % 3])].append(i)
    used = [False] * len(tris)

    def walk(start, rot):
        tri = tris[start]
        seq = [tri[rot], tri[(rot + 1) % 3], tri[(rot + 2) % 3]]
        taken = {start}
        flip = False
        while True:
            x, y = seq[-2], seq[-1]
            key = (y, x) if not flip else (x, y)
            nxt = None
            for j in edges.get(key, ()):
                if not used[j] and j not in taken:
                    nxt = j
                    break
            if nxt is None:
                return seq, taken
            third = [v for v in tris[nxt] if v != x and v != y]
            if len(third) != 1:
                return seq, taken
            seq.append(third[0])
            taken.add(nxt)
            flip = not flip

    def neighbours(i):
        tri = tris[i]
        return sum(1 for k in range(3) for j in edges.get((tri[(k + 1) % 3], tri[k]), ()) if not used[j])

    out = []
    while True:
        remaining = [i for i in range(len(tris)) if not used[i]]
        if not remaining:
            return out
        best = None
        for start in sorted(remaining, key=neighbours)[:4]:
            for rot in range(3):
                seq, taken = walk(start, rot)
                if best is None or len(taken) > len(best[1]):
                    best = (seq, taken)
        for i in best[1]:
            used[i] = True
        out.append(best[0])


def strips_decode(seqs):
    """The triangles a run of strips draws, each rotated to its lowest corner."""
    out = []
    for seq in seqs:
        for i in range(len(seq) - 2):
            a, b, c = seq[i], seq[i + 1], seq[i + 2]
            tri = (a, b, c) if i % 2 == 0 else (b, a, c)
            low = tri.index(min(tri))
            out.append(tri[low:] + tri[:low])
    return sorted(out)


def rotated(tri):
    low = tri.index(min(tri))
    return tri[low:] + tri[:low]


class Level:
    """Reads one level model the way generate_track leaves it."""

    def __init__(self, raw):
        self.raw = raw
        self.tex_offset = struct.unpack_from(">I", raw, 0)[0]
        self.seg_offset = struct.unpack_from(">I", raw, 4)[0]
        self.num_textures = struct.unpack_from(">h", raw, 0x18)[0]
        self.num_segments = struct.unpack_from(">h", raw, 0x1A)[0]
        self.model_size = struct.unpack_from(">i", raw, 0x48)[0]

    def texture_id(self, index):
        return struct.unpack_from(">I", self.raw, self.tex_offset + 8 * index)[0] & 0x7FFF

    def segments(self):
        raw = self.raw
        for s in range(self.num_segments):
            base = self.seg_offset + s * 0x44
            vtx, tri = struct.unpack_from(">II", raw, base)
            bat = struct.unpack_from(">I", raw, base + 12)[0]
            nv, nt, nb = struct.unpack_from(">hhh", raw, base + 0x1C)
            opaque = raw[base + 0x40]
            verts = [list(struct.unpack_from(">hhhBBBB", raw, vtx + i * 10)) for i in range(nv)]
            batches = []
            for i in range(nb + 1):
                e = bat + i * 12
                tex, _, vo, fo, misc, off, flags = struct.unpack_from(">BbhhBBI", raw, e)
                batches.append({"tex": tex, "v": vo, "f": fo, "misc": misc, "off": off, "flags": flags})
            # generate_track: a vertex marked r = g = 1 keeps its alpha in b.
            for i in range(nb):
                for k in range(batches[i]["v"], batches[i + 1]["v"]):
                    v = verts[k]
                    if v[3] == 1 and v[4] == 1:
                        v[6] = v[5]
                        v[3] = v[4] = v[5] = 0x80
                        batches[i]["flags"] |= 0x08000000
            tris = []
            for i in range(nt):
                f, a, b, c, u0, v0, u1, v1, u2, v2 = struct.unpack_from(">BBBB6h", raw, tri + i * 16)
                tris.append((f, a, b, c, u0, v0, u1, v1, u2, v2))
            yield {"verts": verts, "tris": tris, "batches": batches[:nb], "sentinel": batches[nb], "opaque": opaque}


def mat_info_bytes(info):
    return struct.pack("<BBhHHIII", *info[:4], 0, *info[4:])


def compile_level(raw, tables, data, sec, tex_offsets, stats):
    level = Level(raw)
    mats, mat_keys = [], {}
    segs, chunks = [], []
    xyz = bytearray()
    corner_bytes = bytearray()

    def mat_index(key, words, info):
        if key not in mat_keys:
            mat_keys[key] = len(mats)
            mats.append((words, info))
        return mat_keys[key]

    for segment in level.segments():
        table_entries = []
        batches = segment["batches"] + [segment["sentinel"]]
        for i in range(len(segment["batches"])):
            batch = batches[i]
            stats["batches"] += 1
            if batch["flags"] & RENDER_HIDDEN:
                table_entries.append(BATCH_NONE)
                continue
            tex = None
            if batch["tex"] != 0xFF:
                ident = level.texture_id(batch["tex"])
                tex = texture_header(data, sec, tex_offsets, ident)
            flags = batch["flags"] | RENDER_FOG_ACTIVE | RENDER_Z_COMPARE
            tex_flags = tex["flags"] if tex else 0
            solid = (not (tex_flags & RENDER_SEMI_TRANSPARENT) and not (flags & RENDER_WATER)) or bool(flags & RENDER_DECAL)
            in_opaque_range = i < segment["opaque"]
            if solid != in_opaque_range:
                # A pass never reaches a batch on the wrong side of the split,
                # and the other pass skips it: the game draws it in neither.
                table_entries.append(BATCH_NONE)
                continue
            why = None
            if flags & RENDER_PULSING_LIGHTS:
                why = "pulsing"
            elif (flags >> 28) & 7:
                why = "level colour"
            elif tex and tex["format"] in (7, 8):
                why = "palette"
            elif flags & RENDER_DECAL and not (flags & RENDER_Z_COMPARE):
                why = "decal"
            state = None
            if why is None:
                states = []
                for aa in (0, RENDER_ANTI_ALIASING):
                    f = flags
                    if not (f & RENDER_CUTOUT) and not (f & RENDER_DECAL):
                        f |= aa
                    states.append(render_state(*material(tables, f, tex)))
                if states[0] is None or states[0] != states[1]:
                    why = "state varies with anti-aliasing"
                state = states[0]
            first_tri, last_tri = batch["f"], batches[i + 1]["f"]
            first_vtx, count_vtx = batch["v"], batches[i + 1]["v"] - batch["v"]
            tris = segment["tris"][first_tri:last_tri]
            stats["tris"] += len(tris)
            if why is None and state["route"] == ROUTE_TR and len({t[0] & BACKFACE_DRAW for t in tris}) > 1:
                why = "translucent with both kinds of triangle"
            if why is not None:
                stats["walk"][why] += 1
                table_entries.append(BATCH_WALK)
                continue

            w0, w1, omh, _, _ = material(tables, flags, tex)
            progs = combiner(w0, w1, omh)
            frame = batch["off"] >> 2 if tex and not (flags & RENDER_TEX_ANIM) else 0
            if tex and frame >= tex["frames"] >> 8:
                frame = 0
            u_mul = v_mul = 0.0
            if tex:
                u_mul = (tex["width"] / next_pot(tex["width"])) / (32.0 * tex["width"])
                v_mul = (tex["height"] / next_pot(tex["height"])) / (32.0 * tex["height"])

            corner_ids = {}
            corner_list = []
            positions = {}
            one, two = [], []

            def corner(index, u, v):
                key = (index, u, v)
                if key not in corner_ids:
                    vtx = segment["verts"][first_vtx + index]
                    if index not in positions:
                        positions[index] = len(positions)
                    corner_ids[key] = len(corner_list)
                    corner_list.append((positions[index], struct.unpack("<f", struct.pack("<f", u * u_mul))[0],
                                        struct.unpack("<f", struct.pack("<f", v * v_mul))[0],
                                        shade_color(progs, vtx[3:7])))
                return corner_ids[key]

            for t in tris:
                if t[1] >= count_vtx or t[2] >= count_vtx or t[3] >= count_vtx:
                    continue  # the walk skips these too
                face = (corner(t[1], t[4], t[5]), corner(t[2], t[6], t[7]), corner(t[3], t[8], t[9]))
                (two if t[0] & BACKFACE_DRAW else one).append(face)

            parts = []
            for faces in (one, two):
                # Corners shared twice in a triangle would make a degenerate strip step.
                seqs = strips_ordered(faces) if state["route"] == ROUTE_TR else strips_greedy(faces)
                if strips_decode(seqs) != sorted(rotated(f) for f in faces):
                    raise ValueError("strips do not decode to the batch's triangles")
                parts.append(seqs)
            stats["corners"] += sum(len(s) for p in parts for s in p)

            texture_index = batch["tex"] if tex else -1
            wrap = wrap_modes(tex) if tex else (0, 0)
            base = pvr_header(state, tex is not None, wrap, True)
            info = (state["route"], state["decal"], texture_index, frame, w0, w1, omh & (3 << G_MDSFT_CYCLETYPE))
            mat_one = mat_index((tuple(base), texture_index, frame, state["decal"], state["route"]), base, info)
            twosided = pvr_header(state, tex is not None, wrap, False)
            mat_two = mat_index((tuple(twosided), texture_index, frame, state["decal"], state["route"]), twosided, info)

            order = sorted(positions, key=positions.get)
            box_lo = [min(segment["verts"][first_vtx + k][a] for k in order) for a in range(3)]
            box_hi = [max(segment["verts"][first_vtx + k][a] for k in order) for a in range(3)]
            xyz_at = len(xyz)
            for k in order:
                xyz += struct.pack("<3f", *segment["verts"][first_vtx + k][:3])
            corners_at = len(corner_bytes)
            counts = []
            for seqs in parts:
                n = 0
                for seq in seqs:
                    for j, c in enumerate(seq):
                        pos, u, v, argb = corner_list[c]
                        cmd = CMD_VERTEX_EOL if j == len(seq) - 1 else CMD_VERTEX
                        corner_bytes += struct.pack("<ffII", u, v, argb, cmd | pos)
                        n += 1
                counts.append(n)
            chunk_flags = 0
            if flags & RENDER_WATER:
                chunk_flags |= CHUNK_WATER
            if flags & RENDER_TEX_ANIM and tex:
                chunk_flags |= CHUNK_TEX_ANIM
            if state["route"] == ROUTE_TR:
                chunk_flags |= CHUNK_TRANSLUCENT
            chunks.append({"lo": box_lo, "hi": box_hi, "xyz": xyz_at, "corners": corners_at, "npos": len(order),
                           "one": counts[0], "two": counts[1], "mat_one": mat_one, "mat_two": mat_two,
                           "flags": chunk_flags, "batch": i})
            table_entries.append(len(chunks) - 1)
        segs.append((table_entries, len(segment["batches"]), segment["opaque"]))

    head_size = 64
    mats_at = head_size
    infos_at = mats_at + 32 * len(mats)
    segs_at = infos_at + 20 * len(mats)
    tables_at = segs_at + 8 * len(segs)
    body = bytearray()
    seg_rows = bytearray()
    for entries, batch_count, opaque in segs:
        seg_rows += struct.pack("<IHH", tables_at + len(body), batch_count, opaque)
        body += struct.pack(f"<{len(entries)}H", *entries)
    chunks_at = (tables_at + len(body) + 31) & ~31
    xyz_at = chunks_at + 48 * len(chunks)
    corners_at = (xyz_at + len(xyz) + 31) & ~31
    out = bytearray(b"DNML")
    out += struct.pack("<7I", level.model_size, len(mats), len(segs), len(chunks), mats_at, infos_at, segs_at)
    out += struct.pack("<3I", chunks_at, xyz_at, corners_at)
    out += bytes(head_size - len(out))
    for words, _ in mats:
        out += struct.pack("<8I", *words)
    for _, info in mats:
        out += mat_info_bytes(info)
    out += seg_rows + body
    out += bytes(chunks_at - len(out))
    for c in chunks:
        centre = [(c["lo"][a] + c["hi"][a]) * 0.5 for a in range(3)]
        half = [(c["hi"][a] - c["lo"][a]) * 0.5 for a in range(3)]
        out += struct.pack("<6f", *centre, *half)
        out += struct.pack("<II", xyz_at + c["xyz"], corners_at + c["corners"])
        out += struct.pack("<8H", c["npos"], c["one"], c["two"], c["mat_one"], c["mat_two"], c["flags"], c["batch"], 0)
    out += xyz
    out += bytes(corners_at - len(out))
    out += corner_bytes
    return bytes(out)


MODEL_HEADER = 48


def compile_model(raw, tables, data, sec, tex_offsets, stats):
    """An object model's block, or nothing for a model with no batches to speak of."""
    textures_at, vertices_at, triangles_at = struct.unpack_from(">III", raw, 0)
    num_textures, num_vertices, num_triangles, num_batches = struct.unpack_from(">hhhh", raw, 0x22)
    file_size = struct.unpack_from(">i", raw, 0x2C)[0]
    batches_at = struct.unpack_from(">I", raw, 0x38)[0]
    if num_batches <= 0:
        return b""
    verts = [struct.unpack_from(">hhhBBBB", raw, vertices_at + i * 10) for i in range(num_vertices)]
    batches = []
    for i in range(num_batches + 1):
        tex, _, vo, fo, misc, off, flags = struct.unpack_from(">BbhhBBI", raw, batches_at + i * 12)
        batches.append({"tex": tex, "v": vo, "f": fo, "off": off, "flags": flags})
    texture_ids = [struct.unpack_from(">I", raw, textures_at + i * 8)[0] & 0x7FFF for i in range(num_textures)]
    mats, mat_keys = [], {}
    chunks = []
    corner_bytes = bytearray()

    def pair(state, tex, index, combine):
        """The one-sided mat and, right after it, its two-sided twin."""
        wrap = wrap_modes(tex) if tex else (0, 0)
        base = pvr_header(state, tex is not None, wrap, True)
        twin = pvr_header(state, tex is not None, wrap, False)
        key = (tuple(base), index, state["decal"], state["route"], combine)
        if key not in mat_keys:
            mat_keys[key] = len(mats)
            info = (state["route"], state["decal"], index, 0, combine[0], combine[1], combine[2])
            mats.append((base, info))
            mats.append((twin, info))
        return mat_keys[key]

    for i in range(num_batches):
        batch = batches[i]
        first_vtx, count_vtx = batch["v"], batches[i + 1]["v"] - batch["v"]
        tex = None
        if batch["tex"] != 0xFF and batch["tex"] < num_textures:
            tex = texture_header(data, sec, tex_offsets, texture_ids[batch["tex"]])
        stats["model_batches"] += 1
        variants = []
        for variant in range(4):
            flags = batch["flags"] | RENDER_FOG_ACTIVE
            if variant & 1:
                flags |= RENDER_SEMI_TRANSPARENT
            directional = bool(variant & 2)
            if (tex and tex["format"] in (7, 8)) or count_vtx > 32:
                variants.append(0xFFFF)
                continue
            w0, w1, omh, oml, geometry = material(tables, flags, tex, directional)
            state = render_state(w0, w1, omh, oml, geometry)
            if state is None:
                variants.append(0xFFFF)
                continue
            variants.append(pair(state, tex, batch["tex"] if tex else -1, (w0, w1, omh & (3 << G_MDSFT_CYCLETYPE))))
        if variants[0] == 0xFFFF:
            stats["walk"]["model batch"] += 1
        tris = [struct.unpack_from(">BBBB6h", raw, triangles_at + t * 16) for t in range(batch["f"], batches[i + 1]["f"])]
        u_mul = v_mul = 0.0
        if tex:
            u_mul = (tex["width"] / next_pot(tex["width"])) / (32.0 * tex["width"])
            v_mul = (tex["height"] / next_pot(tex["height"])) / (32.0 * tex["height"])
        envmap = bool(batch["flags"] & RENDER_ENVMAP)
        ids, corners = {}, []
        one, two = [], []

        def corner(tri_index, which, index, u, v):
            # An environment-mapped corner's coordinates change, so it is its own.
            key = (tri_index, which) if envmap else (index, u, v)
            if key not in ids:
                ids[key] = len(corners)
                # Its u and v are then the factors for what the frame's lighting left in the model.
                corners.append((index, u_mul if envmap else u * u_mul, v_mul if envmap else v * v_mul,
                                (batch["f"] + tri_index) * 4 + which))
            return ids[key]

        for k, t in enumerate(tris):
            if t[1] >= count_vtx or t[2] >= count_vtx or t[3] >= count_vtx:
                continue
            face = (corner(k, 0, t[1], t[4], t[5]), corner(k, 1, t[2], t[6], t[7]), corner(k, 2, t[3], t[8], t[9]))
            (two if t[0] & BACKFACE_DRAW else one).append(face)
        keep_order = bool(batch["flags"] & RENDER_SEMI_TRANSPARENT)
        counts = []
        offset = len(corner_bytes)
        for faces in (one, two):
            seqs = strips_ordered(faces) if keep_order else strips_greedy(faces)
            if strips_decode(seqs) != sorted(rotated(f) for f in faces):
                raise ValueError("strips do not decode to the batch's triangles")
            n = 0
            for seq in seqs:
                for j, c in enumerate(seq):
                    index, u, v, ref = corners[c]
                    cmd = CMD_VERTEX_EOL if j == len(seq) - 1 else CMD_VERTEX
                    corner_bytes += struct.pack("<ffII", u, v, ref, cmd | index)
                    n += 1
            counts.append(n)
        stats["model_tris"] += len(tris)
        stats["model_corners"] += sum(counts)
        chunks.append((first_vtx, count_vtx, counts[0], counts[1], CHUNK_ENVMAP if envmap else 0, offset, variants))

    if num_vertices:
        lo = [min(v[a] for v in verts) for a in range(3)]
        hi = [max(v[a] for v in verts) for a in range(3)]
    else:
        lo = hi = [0, 0, 0]
    mats_at = MODEL_HEADER
    infos_at = mats_at + 32 * len(mats)
    chunks_at = (infos_at + 20 * len(mats) + 31) & ~31
    corners_at = chunks_at + 32 * len(chunks)
    out = bytearray(b"DNMO")
    out += struct.pack("<7I", file_size, len(mats), len(chunks), mats_at, infos_at, chunks_at, corners_at)
    out += struct.pack("<3f", *[(lo[a] + hi[a]) * 0.5 for a in range(3)])
    out += struct.pack("<f", max((hi[a] - lo[a]) * 0.5 for a in range(3)))
    out += bytes(MODEL_HEADER - len(out))
    for words, _ in mats:
        out += struct.pack("<8I", *words)
    for _, info in mats:
        out += mat_info_bytes(info)
    out += bytes(chunks_at - len(out))
    for first_vtx, count_vtx, one, two, flags, offset, variants in chunks:
        out += struct.pack("<6HI4H2I", first_vtx, count_vtx, one, two, flags, 0, corners_at + offset, *variants, 0, 0)
    out += corner_bytes
    return bytes(out)


def main():
    data = Path(sys.argv[1]).read_bytes()
    sec = sections(Path(sys.argv[2]).read_bytes())
    tables = json.loads(Path(sys.argv[3]).read_text())
    out_path = Path(sys.argv[4])
    models = table(data, sec, LEVEL_MODELS_TABLE)
    tex_offsets = table(data, sec, TEXTURES_3D_TABLE)
    stats = {"batches": 0, "tris": 0, "corners": 0, "model_batches": 0, "model_tris": 0, "model_corners": 0,
             "walk": collections.Counter()}
    blocks = []
    for i in range(len(models) - 1):
        lo, hi = sec[LEVEL_MODELS][0] + models[i], sec[LEVEL_MODELS][0] + models[i + 1]
        if hi <= lo:
            blocks.append(b"")
            continue
        raw = zlib.decompress(data[lo + 5:hi], -15)
        blocks.append(compile_level(raw, tables, data, sec, tex_offsets, stats))
    level_count = len(blocks)
    object_models = table(data, sec, OBJECT_MODELS_TABLE)
    for i in range(len(object_models) - 1):
        lo, hi = sec[OBJECT_MODELS][0] + object_models[i], sec[OBJECT_MODELS][0] + object_models[i + 1]
        if hi <= lo:
            blocks.append(b"")
            continue
        blocks.append(compile_model(zlib.decompress(data[lo + 5:hi], -15), tables, data, sec, tex_offsets, stats))
    head = bytearray(b"DNPK" + struct.pack("<3I", VERSION, level_count, len(blocks) - level_count))
    at = (len(head) + 8 * len(blocks) + 31) & ~31
    body = bytearray()
    for block in blocks:
        head += struct.pack("<II", at + len(body) if block else 0, len(block))
        body += block
        body += bytes(-len(body) % 32)
    head += bytes(at - len(head))
    out_path.write_bytes(bytes(head) + bytes(body))
    print(f"native meshes: {level_count} levels, {stats['batches']} batches, {stats['tris']} triangles, "
          f"{stats['corners']} TA vertices; {len(blocks) - level_count} models, {stats['model_batches']} batches, "
          f"{stats['model_tris']} triangles, {stats['model_corners']} TA vertices; {len(body)} bytes; "
          f"left to the walk: {dict(stats['walk'])}")


if __name__ == "__main__":
    main()
