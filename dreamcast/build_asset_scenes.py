#!/usr/bin/env python3
"""Build the scene asset packs: the parts of assets.bin each scene reads.

usage: build_asset_scenes.py ASSETS_BIN ASSETS_LUT RECORDED OUTDIR

For every level, OUTDIR/LEVEL.dsp holds byte ranges of assets.bin that the
scene gate reads into RAM in one pass when the scene starts loading, so the
game's asset reads for that scene come from RAM instead of the disc:

- found from the level itself: its header, its level model, its object map
  and the 3D textures the level model lists;
- recorded on the console (RECORDED, dreamcast/assets.scenes): whatever else
  the scene read, such as the objects it spawned.

Sections the port keeps in RAM from boot (every one under 128 KB, and the 2D
textures; see dreamcast/reimpl.c) are left out.

File: "DSPK", version, range count, reserved (all little-endian u32), then per
range, sorted, its offset in assets.bin, its size and where its bytes start in
the file, on a 32-byte boundary.
"""
import struct
import sys
import zlib
from pathlib import Path

# include/asset_enums.h, AssetSectionsEnum
TEXTURES_3D, TEXTURES_3D_TABLE, TEXTURES_2D = 2, 3, 4
LEVEL_OBJECT_MAPS_TABLE, LEVEL_OBJECT_MAPS = 20, 21
LEVEL_HEADERS_TABLE, LEVEL_HEADERS = 22, 23
LEVEL_MODELS_TABLE, LEVEL_MODELS = 26, 27
RESIDENT_SMALL_MAX = 128 * 1024  # dreamcast/reimpl.c
RANGE_GAP = 2048  # ranges closer than this are stored as one


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


def entry(sec, tab, section, index):
    """The range of entry `index` of a table-indexed section, or None."""
    if index < 0 or index + 1 >= len(tab):
        return None
    lo, hi = sec[section][0] + tab[index], sec[section][0] + tab[index + 1]
    return (lo, hi) if hi > lo else None


def level_ranges(data, sec, level, headers, models, maps, tex3d):
    ranges = []
    header = entry(sec, headers, LEVEL_HEADERS, level)
    if header is None:
        return ranges
    ranges.append(header)
    raw = data[header[0]:header[1]]
    geometry, collectables = struct.unpack_from(">hh", raw, 0x34)
    # The game falls back to entry 0 for an index past the table's end.
    model = entry(sec, models, LEVEL_MODELS, geometry if 0 <= geometry < len(models) - 1 else 0)
    if model:
        ranges.append(model)
        packed = data[model[0]:model[1]]
        try:
            unpacked = zlib.decompress(packed[5:], -15)  # DKR's gzip: five header bytes, then deflate
            offset, count = struct.unpack_from(">I", unpacked, 0)[0], struct.unpack_from(">h", unpacked, 0x18)[0]
            for i in range(count):
                tex = entry(sec, tex3d, TEXTURES_3D, struct.unpack_from(">I", unpacked, offset + 8 * i)[0] & 0x7FFF)
                if tex:
                    ranges.append(tex)
        except (zlib.error, struct.error):
            pass
    if collectables >= 0:
        objmap = entry(sec, maps, LEVEL_OBJECT_MAPS, collectables if collectables < len(maps) - 1 else 0)
        if objmap:
            ranges.append(objmap)
    return ranges


def recorded(path):
    """dreamcast/assets.scenes: 'scene LEVEL OFFSET:SIZE ...' in hex."""
    scenes = {}
    if path.exists():
        for line in path.read_text().splitlines():
            fields = line.split("#", 1)[0].split()
            if not fields:
                continue
            if fields[0] != "scene" or len(fields) < 2:
                raise ValueError(f"{path}: invalid line: {line}")
            for field in fields[2:]:
                off, size = (int(v, 16) for v in field.split(":"))
                scenes.setdefault(int(fields[1]), []).append((off, off + size))
    return scenes


def without_resident(ranges, sec):
    """Clip away the parts that lie in sections kept in RAM from boot."""
    resident = [(lo, hi) for i, (lo, hi) in enumerate(sec) if hi > lo and (hi - lo <= RESIDENT_SMALL_MAX or i == TEXTURES_2D)]
    out = []
    for lo, hi in ranges:
        pieces = [(lo, hi)]
        for rlo, rhi in resident:
            pieces = [p for a, b in pieces for p in ((a, min(b, rlo)), (max(a, rhi), b)) if p[1] > p[0]]
        out.extend(pieces)
    return out


def joined(ranges):
    out = []
    for lo, hi in sorted(ranges):
        if out and lo <= out[-1][1] + RANGE_GAP:
            out[-1][1] = max(out[-1][1], hi)
        else:
            out.append([lo, hi])
    return out


def write_pack(path, data, ranges):
    at = (16 + 12 * len(ranges) + 31) & ~31
    table_bytes = bytearray()
    body = bytearray()
    for lo, hi in ranges:
        table_bytes += struct.pack("<3I", lo, hi - lo, at + len(body))
        body += data[lo:hi]
        body += bytes(-len(body) % 32)
    head = b"DSPK" + struct.pack("<3I", 1, len(ranges), 0) + table_bytes
    path.write_bytes(head + bytes(at - len(head)) + body)
    return path.stat().st_size


def main():
    data = Path(sys.argv[1]).read_bytes()
    sec = sections(Path(sys.argv[2]).read_bytes())
    scenes = recorded(Path(sys.argv[3]))
    out = Path(sys.argv[4])
    out.mkdir(parents=True, exist_ok=True)
    for old in out.glob("*.dsp"):
        old.unlink()

    headers = table(data, sec, LEVEL_HEADERS_TABLE)
    models = table(data, sec, LEVEL_MODELS_TABLE)
    maps = table(data, sec, LEVEL_OBJECT_MAPS_TABLE)
    tex3d = table(data, sec, TEXTURES_3D_TABLE)
    total = largest = count = 0
    for level in range(len(headers) - 1):
        ranges = joined(without_resident(level_ranges(data, sec, level, headers, models, maps, tex3d) +
                                         scenes.get(level, []), sec))
        if not ranges:
            continue
        size = write_pack(out / f"{level}.dsp", data, ranges)
        total += size
        largest = max(largest, size)
        count += 1
    print(f"{out}: {count} scene packs, {total // 1024} KB, largest {largest // 1024} KB")


if __name__ == "__main__":
    main()
