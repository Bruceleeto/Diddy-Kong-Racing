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
by repeating its edge. pvrtex then quantises and twiddles it, and can also compress it or add
mipmaps (--pvrtex-flags). Each frame is kept as the complete DcTx file pvrtex
writes, which enDjinn's enj_texture_load_blob loads as it is.

It also writes one bundle per scene (--scenes): every texture the scene is
known to use, laid out to be read in one sequential pass while the scene loads,
so nothing is read from disc while it runs. A scene's set is the textures of
its level model, read from the model itself, plus those listed for it in
dreamcast/textures.scenes, which records what the game loaded at run time.

Bundle layout, little endian: "DTSB", u32 version, u32 count, then per texture
u32 id (bit 15 set for the 3D table), u32 bytes, and that many bytes laid out as
a pack record whose frame offsets count from the record's start.

Pack layout, little endian:
  header   "DTPK", u32 version, u32 count2d, u32 count3d
  index    u32 offset per texture, 2D table then 3D table; 0 means absent
  record   u16 width, u16 height, u16 frames, u16 kind (0 opaque, 1 cutout,
           2 translucent), then per frame u32 offset, u32 bytes of its DcTx
           file; each file starts 32-byte aligned
"""
import argparse
import json
import struct
import subprocess
import tempfile
import zlib
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path

VERSION = 2
KIND_OPAQUE, KIND_CUTOUT, KIND_TRANSLUCENT = 0, 1, 2
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


def encode(workdir, tag, w, h, pixels, kind, pvrtex, flags):
    pw, ph, padded = prepare(w, h, pixels, kind)
    src = workdir / f"{tag}.png"
    dst = workdir / f"{tag}.dt"
    write_png(src, pw, ph, padded)
    if pw != ph:
        flags = [f for f in flags if f not in ("-m", "--mipmap")]  # the PVR mipmaps square textures only
    subprocess.run((str(pvrtex), "-i", str(src), "-o", str(dst), "-f", PVRTEX_FORMAT[kind], *flags),
                   check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    data = dst.read_bytes()
    if data[:4] != b"DcTx" or struct.unpack_from("<I", data, 4)[0] != len(data):
        raise ValueError(f"{tag}: pvrtex wrote no usable DT file")
    return data


def level_model_textures(assets):
    """For each level ID, the 3D texture IDs its level model lists."""
    headers = json.loads((assets / "asset_level_headers.meta.json").read_text())["files"]
    models = json.loads((assets / "asset_level_models.meta.json").read_text())["files"]
    result = []
    for name in headers["order"]:
        section = headers["sections"].get(name)
        ids = []
        if section:
            header = json.loads((assets / "levels/headers" / section["filename"]).read_text())
            model = models["sections"].get(header.get("model", ""))
            if model:
                desc = assets / "levels/models" / model["filename"]
                raw = (desc.parent / json.loads(desc.read_text())["raw"]).read_bytes()
                data = zlib.decompress(raw[5:], -15)  # DKR's gzip: five header bytes, then deflate
                table, count = struct.unpack_from(">I", data, 0)[0], struct.unpack_from(">h", data, 0x18)[0]
                ids = sorted({struct.unpack_from(">I", data, table + 8 * i)[0] | 0x8000 for i in range(count)})
        result.append(ids)
    return result


def recorded_textures(path):
    """dreamcast/textures.scenes: lines 'scene LEVEL ID ...', IDs in hex."""
    scenes = {}
    if path and path.exists():
        for raw in path.read_text().splitlines():
            fields = raw.split("#", 1)[0].split()
            if not fields:
                continue
            if fields[0] != "scene" or len(fields) < 2:
                raise ValueError(f"{path}: invalid line: {raw}")
            scenes.setdefault(int(fields[1]), set()).update(int(v, 16) for v in fields[2:])
    return scenes


def write_bundle(path, ids, blocks):
    """blocks: id -> (w, h, kind, [DcTx bytes, ...])"""
    body = bytearray()
    count = 0
    for tex_id in ids:
        block = blocks.get(tex_id)
        if block is None:
            continue
        w, h, kind, frames = block
        record = bytearray(struct.pack("<4H", w, h, len(frames), kind) + bytes(8 * len(frames)))
        for i, dt in enumerate(frames):
            while len(record) % 32:
                record.append(0)
            struct.pack_into("<2I", record, 8 + 8 * i, len(record), len(dt))
            record += dt
        # Keep each record 32-byte aligned within the file: 16-byte header, 8 bytes per entry.
        while (16 + len(body) + 8) % 32:
            body.append(0)
        body += struct.pack("<2I", tex_id, len(record)) + record
        count += 1
    path.write_bytes(b"DTSB" + struct.pack("<2I", 1, count) + bytes(4) + bytes(body))
    return count, path.stat().st_size


def build(root, pvrtex, out, flags, scenes_dir=None, recorded=None):
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
            encoded = [encode(workdir, f"{tag}_{i}", w, h, d[3], kind, pvrtex, flags) for i, d in enumerate(decoded)]
            return w, h, kind, encoded

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
            w, h, kind, frames = result
            while (data_start + len(blob)) % 32:
                blob.append(0)
            record = data_start + len(blob)
            index.append(record)
            table_at = len(blob) + 8
            blob += struct.pack("<4H", w, h, len(frames), kind) + bytes(8 * len(frames))
            for i, dt in enumerate(frames):
                while (data_start + len(blob)) % 32:
                    blob.append(0)
                struct.pack_into("<2I", blob, table_at + 8 * i, data_start + len(blob), len(dt))
                blob += dt
    out.parent.mkdir(parents=True, exist_ok=True)
    out.write_bytes(b"DTPK" + struct.pack("<3I", VERSION, count2d, count3d) +
                    struct.pack(f"<{len(index)}I", *index) + bytes(blob))
    if scenes_dir:
        blocks = {}
        for table_index, table in enumerate(results):
            for index, result in enumerate(table):
                if result:
                    blocks[(0x8000 if table_index else 0) | index] = result
        models = level_model_textures(root / "assets/.vanilla/us.v77")
        recorded_ids = recorded_textures(recorded)
        scenes_dir.mkdir(parents=True, exist_ok=True)
        total = 0
        for level, model_ids in enumerate(models):
            ids = sorted(set(model_ids) | recorded_ids.get(level, set()))
            if ids:
                count, size = write_bundle(scenes_dir / f"{level}.dtb", ids, blocks)
                total += size
        print(f"DKR PVR textures: {len(models)} scene bundles, {total} bytes")
    kinds = [r[2] for t in results for r in t if r]
    print(f"DKR PVR textures: {len(kinds)} textures, {kinds.count(KIND_OPAQUE)} RGB565, "
          f"{kinds.count(KIND_CUTOUT)} ARGB1555, {kinds.count(KIND_TRANSLUCENT)} ARGB4444, "
          f"{out.stat().st_size} bytes")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("root", type=Path)
    parser.add_argument("pvrtex", type=Path)
    parser.add_argument("output", type=Path)
    parser.add_argument("--pvrtex-flags", default="", help="extra pvrtex options, e.g. '-c' or '-m'")
    parser.add_argument("--scenes", type=Path, help="directory for the per-scene bundles")
    parser.add_argument("--recorded", type=Path, help="dreamcast/textures.scenes")
    args = parser.parse_args()
    build(args.root.resolve(), args.pvrtex, args.output.resolve(), args.pvrtex_flags.split(),
          args.scenes.resolve() if args.scenes else None, args.recorded.resolve() if args.recorded else None)


if __name__ == "__main__":
    main()
