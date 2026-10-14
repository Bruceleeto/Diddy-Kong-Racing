#!/usr/bin/env python3
"""Merge the textures a TEX_TRACE=1 run loaded into dreamcast/textures.scenes.

usage: record_texture_scenes.py LOG [LOG ...]

Reads the TEXUSE lines of each log and adds every texture to its scene's line,
keeping what the file already lists, so runs through different scenes add up.
"""
import re
import sys
from pathlib import Path

MAP = Path(__file__).resolve().parent / "textures.scenes"


def main():
    header, scenes = [], {}
    for line in MAP.read_text().splitlines():
        fields = line.split("#", 1)[0].split()
        if not fields:
            if not scenes:
                header.append(line)
            continue
        scenes.setdefault(int(fields[1]), set()).update(int(v, 16) for v in fields[2:])
    for log in sys.argv[1:]:
        for match in re.finditer(r"TEXUSE level=(-?\d+) id=([0-9a-f]+)", Path(log).read_text(errors="replace")):
            level = int(match.group(1))
            if level >= 0:
                scenes.setdefault(level, set()).add(int(match.group(2), 16))
    lines = header + [f"scene {level} " + " ".join(f"{i:04x}" for i in sorted(ids)) for level, ids in sorted(scenes.items())]
    MAP.write_text("\n".join(lines) + "\n")
    print(f"textures.scenes: {len(scenes)} scenes, {sum(len(v) for v in scenes.values())} textures")


if __name__ == "__main__":
    main()
