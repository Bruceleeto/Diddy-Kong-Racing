#!/usr/bin/env python3
"""Build VIZ1 spectrum sidecars and a readable DKR music playlist."""

import argparse
import json
import re
import struct
import subprocess
from pathlib import Path


VIZ_HEADER = struct.Struct("<4sBBBBI")
WORDS = {"TTS": "T.T.'s", "TAJS": "Taj's", "WIZPIGS": "Wizpig's", "FIMAL": "Final",
         "UNK": "Unknown"}


def sequence_names(path):
    names = []
    for line in path.read_text().splitlines():
        match = re.match(r"\s*(SEQUENCE_[A-Z0-9_]+)\s*,", line)
        if match:
            names.append(match.group(1))
    return names


def title(name):
    words = re.findall(r"[A-Z]+|\d+", name.removeprefix("SEQUENCE_"))
    return " ".join(WORDS.get(word, word.capitalize()) for word in words)


def write_if_changed(path, data):
    if not path.is_file() or path.read_bytes() != data:
        path.write_bytes(data)


def tracks(root, music, compiler):
    names = sequence_names(root / "include/sequence_ids.h")
    controls = music / "music_controls"
    for control in sorted(controls.glob("sequence_*.afx"), key=lambda path: int(path.stem[9:])):
        sequence = int(control.stem[9:])
        if sequence >= len(names):
            raise ValueError(f"{control}: unknown sequence")
        visual = music / "music_visuals" / f"sequence_{sequence}.viz"
        visual.parent.mkdir(exist_ok=True)
        subprocess.run((str(compiler), "--visual", str(control), str(visual)), check=True)
        data = visual.read_bytes()
        # Sequence 1 is an empty source slot, not a playable DKR track.
        if not any(data[VIZ_HEADER.size:]):
            continue
        yield {"sequence": sequence, "title": title(names[sequence]), "control": control.relative_to(music).as_posix(),
               "visual": visual.relative_to(music).as_posix()}


def verify(music):
    manifest = json.loads((music / "music_visuals.json").read_text())
    assert manifest["version"] == 1 and manifest["bank"] == "music.afb"
    assert manifest["tracks"] and len({track["sequence"] for track in manifest["tracks"]}) == len(manifest["tracks"])
    for track in manifest["tracks"]:
        assert track["title"] and not track["title"].startswith("Sequence")
        control, visual = music / track["control"], music / track["visual"]
        assert control.is_file() and visual.is_file()
        magic, version, bands, rate, reserved, frames = VIZ_HEADER.unpack_from(visual.read_bytes())
        assert (magic, version, bands, rate, reserved) == (b"VIZ1", 1, 32, 60, 0) and frames
        assert visual.stat().st_size == VIZ_HEADER.size + frames * bands
    print(f"DKR music visuals: {len(manifest['tracks'])} VIZ1 sidecars")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("root", type=Path)
    parser.add_argument("compiler", type=Path)
    parser.add_argument("music", type=Path)
    parser.add_argument("--verify", action="store_true")
    args = parser.parse_args()
    root, music, compiler = args.root.resolve(), args.music.resolve(), args.compiler.resolve()
    if args.verify:
        verify(music)
        return
    playlist = {"version": 1, "bank": "music.afb", "tracks": list(tracks(root, music, compiler))}
    write_if_changed(music / "music_visuals.json", (json.dumps(playlist, indent=2) + "\n").encode())
    verify(music)


if __name__ == "__main__":
    main()
