#!/usr/bin/env python3
"""Stage the already-authored DKR music assets for the optional player."""

import argparse
import json
import shutil
import struct
from pathlib import Path


AFX_HEADER_BYTES = 80
SHORT_CUE_TICKS = 10_000


def write_if_changed(path: Path, data: bytes) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    if not path.is_file() or path.read_bytes() != data:
        path.write_bytes(data)


def flow_stats(data: bytes) -> dict[str, int]:
    if len(data) < AFX_HEADER_BYTES:
        raise ValueError("truncated AFX header")
    h = struct.unpack_from("<20I", data)
    image_at, image_bytes, stream_at, stream_bytes = h[4:8]
    if h[1] != 7 or h[2] != len(data) or image_at + image_bytes != len(data) or stream_at + stream_bytes > image_bytes:
        raise ValueError("invalid AFX image")
    stream = data[image_at + stream_at:image_at + stream_at + stream_bytes]
    cursor = duration = notes = 0
    while cursor < len(stream):
        opcode = stream[cursor]
        if opcode in (0, 0x13):
            size, wait = 1, 0
        elif opcode == 1:
            size, wait = 2, stream[cursor + 1]
        elif opcode == 2:
            size, wait = 3, int.from_bytes(stream[cursor + 1:cursor + 3], "little")
        elif opcode == 3:
            size, wait = 5, int.from_bytes(stream[cursor + 1:cursor + 5], "little")
        elif opcode == 0x12:
            size, wait = 2, 0
        elif opcode == 0x14:
            size, wait, notes = 8, 0, notes + 1
        elif opcode == 0x15:
            size, wait = 4, 0
        elif opcode in (0x10, 0x11):
            prefix = 8 if opcode == 0x10 else 6
            if cursor + prefix > len(stream):
                raise ValueError("truncated AFX event")
            mask = int.from_bytes(stream[cursor + prefix - 4:cursor + prefix], "little")
            size, wait = prefix + 2 * mask.bit_count(), 0
            notes += opcode == 0x10
        else:
            raise ValueError(f"unknown AFX opcode {opcode:#x}")
        if cursor + size > len(stream):
            raise ValueError("truncated AFX event")
        cursor += size
        duration += wait
    if cursor != len(stream):
        raise ValueError("truncated AFX stream")
    setup_bytes = h[9] * 36
    if setup_bytes > stream_at:
        raise ValueError("invalid AFX setups")
    return {"bytes": image_bytes, "control_bytes": 0, "sample_bytes": 0,
            "stream_bytes": stream_bytes, "setup_bytes": setup_bytes,
            "padding_bytes": image_bytes - setup_bytes - stream_bytes,
            "command_baseline_bytes": stream_bytes + notes * 36,
            "note_count": notes, "sample_count": 0, "setup_count": h[9],
            "channel_count": h[16], "duration_ticks": duration}


def stage(root: Path, disc: Path, verify: bool) -> bytes:
    music = root / "build/dc/aicaflow"
    manifest = json.loads((music / "music_visuals.json").read_text())
    tracks = manifest.get("tracks", [])
    if manifest.get("version") != 1 or len(tracks) != 64 or len({track["sequence"] for track in tracks}) != 64:
        raise ValueError("expected 64 unique non-silent DKR tracks")
    bank = music / "music.afb"
    properties = (root / "assets/.vanilla/us.v77/audio/unknown/asset_audio_6.bin").read_bytes()
    if not bank.is_file():
        raise ValueError("missing shared DKR music bank")
    songs = []
    sources = [(bank, disc / bank.name), (music / "music_visuals.json", disc / "manifest.json")]
    for track in tracks:
        sequence = track["sequence"]
        source = music / "music_controls" / f"sequence_{sequence}.afx"
        seek = source.with_suffix(".afc")
        visual = music / track["visual"]
        if not source.is_file() or not seek.is_file() or not visual.is_file() or sequence * 3 + 2 >= len(properties):
            raise ValueError(f"incomplete DKR music track {sequence}")
        stats = flow_stats(source.read_bytes())
        volume, _, reverb = properties[sequence * 3:sequence * 3 + 3]
        songs.append({"sequence": sequence, "title": track["title"], "file": source.name,
                      "visual": track["visual"], "dsp": bool(reverb), "gain": volume * 96 // 127, **stats})
        sources += [(source, disc / source.name), (seek, disc / seek.name), (visual, disc / track["visual"])]
    songs.sort(key=lambda song: (song["duration_ticks"] < SHORT_CUE_TICKS, "ambient" in song["title"].casefold()))
    header = ["static const struct song { const char *title, *file, *visual, *dsp; bool wraps; uint32_t bytes, control_bytes, sample_bytes, stream_bytes, setup_bytes, padding_bytes, command_baseline_bytes, note_count; uint16_t sample_count, setup_count; uint8_t sequence, channel_count, gain; } songs[] = {"]
    keys = ("bytes", "control_bytes", "sample_bytes", "stream_bytes", "setup_bytes", "padding_bytes", "command_baseline_bytes", "note_count", "sample_count", "setup_count", "sequence", "channel_count", "gain")
    for song in songs:
        header.append("{" + ",".join((json.dumps(song["title"]), json.dumps(song["file"]), json.dumps(song["visual"]),
            '"room"' if song["dsp"] else "NULL", "false", *(str(song[key]) for key in keys))) + "},")
    header.append("};")
    generated = ("\n".join(header) + "\n").encode()
    include = Path(__file__).resolve().parent / "include/songs.h"
    if verify:
        if not include.is_file() or include.read_bytes() != generated:
            raise ValueError("generated playlist header differs")
        for source, target in sources:
            if not target.is_file() or target.read_bytes() != source.read_bytes():
                raise ValueError(f"staged asset differs: {target}")
    else:
        for source, target in sources:
            write_if_changed(target, source.read_bytes())
        write_if_changed(include, generated)
    return generated


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("root", type=Path)
    parser.add_argument("disc", type=Path)
    parser.add_argument("--verify", action="store_true")
    args = parser.parse_args()
    stage(args.root.resolve(), args.disc.resolve(), args.verify)


if __name__ == "__main__":
    main()
