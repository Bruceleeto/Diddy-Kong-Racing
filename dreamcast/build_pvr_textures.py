#!/usr/bin/env python3
"""Convert every DKR texture asset to a ready PVR texture, offline.

The game used to decode each N64 texture on the console the first time it was
drawn. This builds them all ahead of time from the extracted PNGs, with
pvrtex, into one pack the game loads with each texture asset, so a scene's
textures are in video memory before it starts and nothing is decoded while it
runs.

Each frame is first brought to exactly the colours the runtime decoder would
have produced from the cartridge data (5-bit channels widened the same way,
4-bit alpha, the image the right way up), then given the same treatment
gfx_create_texture applies: the format follows the alpha (RGB565 when opaque,
ARGB1555 for a cutout, ARGB4444 otherwise), invisible texels take the average
colour of their visible neighbours, and the image is padded to a power of two
by repeating its edge. pvrtex then quantises and twiddles it.

Pack layout, little endian:
  header   "DTPK", u32 version, u32 count2d, u32 count3d
  index    u32 offset per texture, 2D table then 3D table; 0 means absent
  record   u16 width, u16 height, u16 padded_w, u16 padded_h,
           u16 frames, u16 kind (0 opaque, 1 cutout, 2 translucent),
           u32 pvr format bits (PVR_TXRFMT_*), u32 bytes per frame,
           then each frame's texel data, 32-byte aligned
"""
import argparse
import json
import struct
import subprocess
import tempfile
import zlib
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path

VERSION = 1
KIND_OPAQUE, KIND_CUTOUT, KIND_TRANSLUCENT = 0, 1, 2
# PVR_TXRFMT_* texture control word bits for each kind; twiddled.
PVR_FORMAT = {KIND_OPAQUE: 1 << 27, KIND_CUTOUT: 0 << 27, KIND_TRANSLUCENT: 2 << 27}
PVRTEX_FORMAT = {KIND_OPAQUE: "RGB565", KIND_CUTOUT: "ARGB1555", KIND_TRANSLUCENT: "ARGB4444"}


def widen5(value):
    v = value >> 3
    return (v << 3) | (v >> 2)


def widen4(value):
    v = value >> 4
    return (v << 4) | v


def read_png(path):
    """8-bit, non-interlaced PNGs, which is all the extractor writes: (w, h, channels, rows)."""
    data = path.read_bytes()
    if data[:8] != b"\x89PNG\r\n\x1a\n":
        raise ValueError(f"{path}: not a PNG")
    pos, idat, w = 8, bytearray(), 0
    while pos < len(data):
        length, kind = struct.unpack(">I4s", data[pos:pos + 8])
        body = data[pos + 8:pos + 8 + length]
        if kind == b"IHDR":
            w, h, depth, colour, _, _, interlace = struct.unpack(">2I5B", body)
            if depth != 8 or interlace or colour not in (0, 2, 4, 6):
                raise ValueError(f"{path}: unsupported PNG layout")
            channels = {0: 1, 2: 3, 4: 2, 6: 4}[colour]
        elif kind == b"IDAT":
            idat += body
        pos += 12 + length
    raw = zlib.decompress(bytes(idat))
    stride = w * channels
    rows, prev = [], bytearray(stride)
    for y in range(h):
        kind = raw[y * (stride + 1)]
        line = bytearray(raw[y * (stride + 1) + 1:(y + 1) * (stride + 1)])
        for i in range(stride):
            left = line[i - channels] if i >= channels else 0
            up = prev[i]
            corner = prev[i - channels] if i >= channels else 0
            if kind == 1:
                line[i] = (line[i] + left) & 255
            elif kind == 2:
                line[i] = (line[i] + up) & 255
            elif kind == 3:
                line[i] = (line[i] + ((left + up) >> 1)) & 255
            elif kind == 4:
                pa, pb, pc = abs(up - corner), abs(left - corner), abs(left + up - 2 * corner)
                line[i] = (line[i] + (left if pa <= pb and pa <= pc else up if pb <= pc else corner)) & 255
        rows.append(line)
        prev = line
    return w, h, channels, rows


def write_png(path, w, h, pixels):
    raw = b"".join(b"\0" + bytes(c for px in pixels[y * w:(y + 1) * w] for c in px) for y in range(h))

    def chunk(kind, body):
        return struct.pack(">I", len(body)) + kind + body + struct.pack(">I", zlib.crc32(kind + body))

    path.write_bytes(b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">2I5B", w, h, 8, 6, 0, 0, 0)) +
                     chunk(b"IDAT", zlib.compress(raw, 1)) + chunk(b"IEND", b""))


def runtime_rgba(path, fmt, flipped):
    """The RGBA8888 pixels main.c's decode_texture would produce for this asset."""
    w, h, channels, rows = read_png(path)
    if flipped:
        rows.reverse()
    src = []
    for row in rows:
        for x in range(w):
            p = row[x * channels:(x + 1) * channels]
            if channels == 1:
                l = p[0]
                src.append((l, l, l, l if fmt in ("I4", "I8") else 255))
            elif channels == 2:
                l, a = p
                src.append((l, l, l, l) if fmt in ("I4", "I8") else (l, l, l, a))
            elif channels == 3:
                src.append((p[0], p[1], p[2], 255))
            else:
                src.append(tuple(p))
    out = []
    for r, g, b, a in src:
        if fmt == "RGBA16":
            out.append((widen5(r), widen5(g), widen5(b), 255 if a >= 128 else 0))
        elif fmt in ("IA8", "I4"):
            out.append((widen4(r), widen4(g), widen4(b), widen4(a)))
        elif fmt == "IA4":
            i = r >> 5
            i = (i << 5) | (i << 2) | (i >> 1)
            out.append((i, i, i, 255 if a >= 128 else 0))
        else:  # RGBA32, IA16, I8: eight bits a channel already
            out.append((r, g, b, a))
    return w, h, out


def classify(pixels):
    opaque = all((a >> 4) == 0xF for _, _, _, a in pixels)
    if opaque:
        return KIND_OPAQUE
    cutout = all((a >> 4) in (0, 0xF) for _, _, _, a in pixels)
    return KIND_CUTOUT if cutout else KIND_TRANSLUCENT


def next_pot(v):
    p = 8
    while p < v:
        p <<= 1
    return p


def prepare(w, h, pixels, kind):
    """Bleed invisible texels and pad to a power of two, as gfx_create_texture does."""
    if kind != KIND_OPAQUE:
        bled = list(pixels)
        for y in range(h):
            for x in range(w):
                r, g, b, a = pixels[y * w + x]
                if (a >> 4) != 0:
                    continue
                nr = ng = nb = n = 0
                for dy in (-1, 0, 1):
                    for dx in (-1, 0, 1):
                        qx, qy = x + dx, y + dy
                        if 0 <= qx < w and 0 <= qy < h:
                            q = pixels[qy * w + qx]
                            if (q[3] >> 4) != 0:
                                nr += q[0]
                                ng += q[1]
                                nb += q[2]
                                n += 1
                if n:
                    bled[y * w + x] = (nr // n, ng // n, nb // n, a)
        pixels = bled
    pw, ph = next_pot(w), next_pot(h)
    padded = [pixels[min(y, h - 1) * w + min(x, w - 1)] for y in range(ph) for x in range(pw)]
    return pw, ph, padded


def convert_frame(job):
    path, fmt, flipped, workdir, tag = job
    w, h, pixels = runtime_rgba(path, fmt, flipped)
    kind = classify(pixels)
    return w, h, kind, pixels


def encode(workdir, tag, w, h, pixels, kind, pvrtex):
    pw, ph, padded = prepare(w, h, pixels, kind)
    src = workdir / f"{tag}.png"
    dst = workdir / f"{tag}.dt"
    write_png(src, pw, ph, padded)
    subprocess.run((str(pvrtex), "-i", str(src), "-o", str(dst), "-f", PVRTEX_FORMAT[kind]),
                   check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    data = dst.read_bytes()
    if data[:4] != b"DcTx":
        raise ValueError(f"{tag}: pvrtex wrote no DT file")
    header = 32 * (data[9] + 1)
    texels = data[header:header + pw * ph * 2]
    if len(texels) != pw * ph * 2:
        raise ValueError(f"{tag}: unexpected DT size")
    return pw, ph, texels


def build(root, pvrtex, out):
    assets = root / "assets/.vanilla/us.v77"
    tables = []
    for table in ("2d", "3d"):
        meta = json.loads((assets / f"asset_textures_{table}.meta.json").read_text())
        textures = []
        for name in meta["files"]["order"]:
            section = meta["files"]["sections"].get(name)
            if not section:
                textures.append(None)
                continue
            desc = assets / "textures" / table / section["filename"]
            info = json.loads(desc.read_text())
            frames = [desc.parent / image for image in info["images"]]
            textures.append((f"{table}_{len(textures)}", frames, info["format"], bool(info.get("flipped-image"))))
        tables.append(textures)

    with tempfile.TemporaryDirectory(prefix="dkr-pvrtex-") as tmp:
        workdir = Path(tmp)

        def one(entry):
            if entry is None:
                return None
            tag, frames, fmt, flipped = entry
            decoded = [convert_frame((frame, fmt, flipped, workdir, tag)) for frame in frames]
            w, h = decoded[0][0], decoded[0][1]
            if any((d[0], d[1]) != (w, h) for d in decoded):
                return None  # frames of different sizes: leave it to the runtime decoder
            # One format for all frames, the most capable any frame needs.
            kind = max(d[2] for d in decoded)
            encoded = [encode(workdir, f"{tag}_{i}", w, h, d[3], kind, pvrtex) for i, d in enumerate(decoded)]
            pw, ph = encoded[0][0], encoded[0][1]
            return w, h, pw, ph, kind, [e[2] for e in encoded]

        with ThreadPoolExecutor() as pool:
            results = [list(pool.map(one, table)) for table in tables]

    count2d, count3d = len(results[0]), len(results[1])
    blob = bytearray()
    index = []
    data_start = 16 + 4 * (count2d + count3d)
    for table in results:
        for result in table:
            if result is None:
                index.append(0)
                continue
            w, h, pw, ph, kind, frames = result
            while (data_start + len(blob)) % 32:
                blob.append(0)
            index.append(data_start + len(blob))
            frame_bytes = pw * ph * 2
            blob += struct.pack("<6H2I", w, h, pw, ph, len(frames), kind, PVR_FORMAT[kind], frame_bytes)
            for texels in frames:
                while (data_start + len(blob)) % 32:
                    blob.append(0)
                blob += texels
    out.parent.mkdir(parents=True, exist_ok=True)
    out.write_bytes(b"DTPK" + struct.pack("<3I", VERSION, count2d, count3d) +
                    struct.pack(f"<{len(index)}I", *index) + bytes(blob))
    kinds = [r[4] for t in results for r in t if r]
    print(f"DKR PVR textures: {len(kinds)} textures, {kinds.count(KIND_OPAQUE)} RGB565, "
          f"{kinds.count(KIND_CUTOUT)} ARGB1555, {kinds.count(KIND_TRANSLUCENT)} ARGB4444, "
          f"{out.stat().st_size} bytes")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("root", type=Path)
    parser.add_argument("pvrtex", type=Path)
    parser.add_argument("output", type=Path)
    args = parser.parse_args()
    build(args.root.resolve(), args.pvrtex, args.output.resolve())


if __name__ == "__main__":
    main()
