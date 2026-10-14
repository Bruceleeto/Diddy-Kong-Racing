#!/usr/bin/env python3
"""Merge what a TEX_TRACE=1 run loaded into the scene maps.

usage: record_scenes.py LOG [LOG ...]

TEXUSE lines go into dreamcast/textures.scenes (the textures each scene's
bundle holds). MODELUSE and SONGUSE lines that come after the scene's SCENE
LOAD line, so models and songs loaded while the scene ran, go into
dreamcast/models.scenes and dreamcast/songs.scenes, which the scene gate loads
beforehand; those the load itself reads are resident anyway. Songs keep the
order they started in: the gate makes the first one resident. Each texture or model is added to its scene's line, keeping
what the file already lists, so runs through different scenes add up.
"""
import re
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
MAPS = {"TEXUSE": HERE / "textures.scenes", "MODELUSE": HERE / "models.scenes", "SONGUSE": HERE / "songs.scenes"}
AFTER_START = {"MODELUSE", "SONGUSE"}


def uses(tag, text):
    """(level, id) of each tag line; MODELUSE only once the scene has started."""
    started = tag not in AFTER_START
    for match in re.finditer(r"(SCENE LOAD )|(level_load enter)|" + tag + r" level=(-?\d+) id=([0-9a-f]+)", text):
        if match.group(1):
            started = True
        elif match.group(2):
            started = tag not in AFTER_START
        elif started:
            yield int(match.group(3)), int(match.group(4), 16)


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
        merge(path, tag, logs)


if __name__ == "__main__":
    main()
