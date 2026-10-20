#!/usr/bin/env python3
"""Merge what a TEX_TRACE=1 run loaded into the scene maps.

usage: record_scenes.py LOG [LOG ...]

TEXUSE lines go into dreamcast/textures.scenes (the textures each scene's
bundle holds). MODELUSE and SONGUSE lines that come after the scene's SCENE
LOAD line, so models and songs loaded while the scene ran, go into
dreamcast/models.scenes and dreamcast/songs.scenes, which the scene gate loads
beforehand; ASSETUSE lines, the parts of assets.bin a scene read that its pack
did not hold, go into dreamcast/assets.scenes, from which the scene packs are
built; those the load itself reads are resident anyway. Songs keep the
order they started in: the gate makes the first one resident. Each texture or model is added to its scene's line, keeping
what the file already lists, so runs through different scenes add up.
"""
import re
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
MAPS = {"TEXUSE": HERE / "textures.scenes", "MODELUSE": HERE / "models.scenes", "SONGUSE": HERE / "songs.scenes",
        "ASSETUSE": HERE / "assets.scenes"}
AFTER_START = {"MODELUSE", "SONGUSE"}
# assets.scenes ranges closer than this are kept as one.
RANGE_GAP = 2048


def uses(tag, text):
    """(level, id) of each tag line; MODELUSE only once the scene has started."""
    started = tag not in AFTER_START
    for match in re.finditer(r"(SCENE LOAD )|(level_load enter)|" + tag + r" level=(-?\d+) id=([0-9a-f:]+)", text):
        if match.group(1):
            started = True
        elif match.group(2):
            started = tag not in AFTER_START
        elif started:
            ident = match.group(4)
            yield int(match.group(3)), (tuple(int(v, 16) for v in ident.split(":")) if ":" in ident else int(ident, 16))


def merge_ranges(path, logs):
    """assets.scenes: OFFSET:SIZE ranges of assets.bin, joined where they overlap or nearly touch."""
    header, scenes = [], {}
    if path.exists():
        for line in path.read_text().splitlines():
            fields = line.split("#", 1)[0].split()
            if not fields:
                if not scenes:
                    header.append(line)
                continue
            ranges = scenes.setdefault(int(fields[1]), [])
            for field in fields[2:]:
                off, size = (int(v, 16) for v in field.split(":"))
                ranges.append((off, off + size))
    for text in logs:
        for level, (off, size) in uses("ASSETUSE", text):
            if level >= 0:
                scenes.setdefault(level, []).append((off, off + size))
    lines = list(header)
    for level, ranges in sorted(scenes.items()):
        joined = []
        for lo, hi in sorted(ranges):
            if joined and lo <= joined[-1][1] + RANGE_GAP:
                joined[-1][1] = max(joined[-1][1], hi)
            else:
                joined.append([lo, hi])
        lines.append(f"scene {level} " + " ".join(f"{lo:x}:{hi - lo:x}" for lo, hi in joined))
    path.write_text("\n".join(lines) + "\n")
    print(f"{path.name}: {len(scenes)} scenes")


def merge(path, tag, logs):
    header, scenes = [], {}
    for line in path.read_text().splitlines():
        fields = line.split("#", 1)[0].split()
        if not fields:
            if not scenes:
                header.append(line)
            continue
        ids = scenes.setdefault(int(fields[1]), [])
        ids.extend(i for i in (int(v, 16) for v in fields[2:]) if i not in ids)
    for text in logs:
        for level, ident in uses(tag, text):
            ids = scenes.setdefault(level, []) if level >= 0 else None
            if ids is not None and ident not in ids:
                ids.append(ident)
    ordered = (lambda ids: ids) if tag == "SONGUSE" else sorted
    lines = header + [f"scene {level} " + " ".join(f"{i:04x}" for i in ordered(ids)) for level, ids in sorted(scenes.items())]
    path.write_text("\n".join(lines) + "\n")
    print(f"{path.name}: {len(scenes)} scenes, {sum(len(v) for v in scenes.values())} entries")


def main():
    logs = [Path(log).read_text(errors="replace") for log in sys.argv[1:]]
    for tag, path in MAPS.items():
        if tag == "ASSETUSE":
            merge_ranges(path, logs)
        else:
            merge(path, tag, logs)


if __name__ == "__main__":
    main()
