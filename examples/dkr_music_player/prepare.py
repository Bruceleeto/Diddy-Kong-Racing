#!/usr/bin/env python3
"""Stage DKR's shared bank, compact flows, seek indexes, visuals, and playlist."""
import argparse
import hashlib
import json
import struct
import sys
from pathlib import Path


AFX_HEADER_BYTES = 80
SHORT_CUE_TICKS = 10_000


def flow_duration_ticks(data):
    """Return the AFX control-stream duration in the file's 1 kHz ticks."""
    if len(data) < AFX_HEADER_BYTES:
        raise ValueError("truncated AFX header")
    fields = struct.unpack_from("<20I", data)
    image_at, image_size, stream_at, stream_size = fields[4:8]
    if image_at > len(data) or image_size > len(data) - image_at or stream_at > image_size or stream_size > image_size - stream_at:
        raise ValueError("invalid AFX control-stream range")
    stream = data[image_at + stream_at:image_at + stream_at + stream_size]
    cursor = duration = 0
    while cursor < len(stream):
        opcode = stream[cursor]
        if opcode in (0, 0x13):
            size, wait = 1, 0
        elif opcode == 1:
            size, wait = 2, 0
        elif opcode == 2:
            size, wait = 3, int.from_bytes(stream[cursor + 1:cursor + 3], "little")
        elif opcode == 3:
            size, wait = 5, int.from_bytes(stream[cursor + 1:cursor + 5], "little")
        elif opcode == 0x12:
            size, wait = 2, 0
        elif opcode == 0x14:
            size, wait = 8, 0
        elif opcode == 0x15:
            size, wait = 4, 0
        elif opcode in (0x10, 0x11):
            prefix = 8 if opcode == 0x10 else 6
            if cursor + prefix > len(stream):
                raise ValueError("truncated AFX event")
            mask = int.from_bytes(stream[cursor + prefix - 4:cursor + prefix], "little")
            size, wait = prefix + 2 * bin(mask).count("1"), 0
        else:
            raise ValueError(f"unknown AFX opcode {opcode}")
        if cursor + size > len(stream):
            raise ValueError("truncated AFX event")
        if opcode == 1:
            wait = stream[cursor + 1]
        elif opcode == 2:
            wait = int.from_bytes(stream[cursor + 1:cursor + 3], "little")
        elif opcode == 3:
            wait = int.from_bytes(stream[cursor + 1:cursor + 5], "little")
        cursor += size
        duration += wait
    return duration


def write_if_changed(path, data):
    if not path.is_file() or path.read_bytes() != data:
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(data)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("root", type=Path)
    parser.add_argument("aicaflow_tools", type=Path)
    parser.add_argument("disc", type=Path)
    parser.add_argument("--verify", action="store_true")
    args = parser.parse_args()
    root, disc = args.root.resolve(), args.disc.resolve()
    music = root / "build/dc/aicaflow"
    manifest = json.loads((music / "music_visuals.json").read_text())
    if manifest.get("version") != 1:
        raise ValueError("unsupported DKR music manifest")
    sys.path.insert(0, str(args.aicaflow_tools.resolve()))
    import afx_metadata

    properties = (root / "assets/.vanilla/us.v77/audio/unknown/asset_audio_6.bin").read_bytes()
    tracks = manifest["tracks"]
    if len(tracks) != 64 or len({track["sequence"] for track in tracks}) != len(tracks):
        raise ValueError("expected 64 unique non-silent DKR tracks")
    songs = []
    bank = music / "music.afb"
    if not bank.is_file():
        raise ValueError("missing shared DKR music bank")
    if not args.verify:
        write_if_changed(disc / bank.name, bank.read_bytes())
    for track in tracks:
        sequence = track["sequence"]
        visual = music / track["visual"]
        source = music / "music_controls" / f"sequence_{sequence}.afx"
        control = music / "music_controls" / f"sequence_{sequence}.afc"
        if not visual.is_file() or not source.is_file() or not control.is_file() or sequence * 3 + 2 >= len(properties):
            raise ValueError(f"incomplete DKR music track {sequence}")
        source_data = source.read_bytes()
        layout = afx_metadata.resident_layout(source_data)
        volume, _, reverb = properties[sequence * 3:sequence * 3 + 3]
        song = {"sequence": sequence, "title": track["title"], "file": source.name,
                "visual": track["visual"], "dsp": bool(reverb),
                "gain": volume * 96 // 127, "bytes": layout["image_bytes"],
                "control_bytes": control.stat().st_size,
                "duration_ticks": flow_duration_ticks(source_data), **layout}
        songs.append(song)
        if not args.verify:
            write_if_changed(disc / source.name, source_data)
            write_if_changed(disc / control.name, control.read_bytes())
            write_if_changed(disc / track["visual"], visual.read_bytes())
    # Keep the established music order; ambient beds follow regular songs and
    # brief cues remain at the very end.
    def playlist_group(song):
        return song["duration_ticks"] < SHORT_CUE_TICKS, "ambient" in song["title"].casefold()
    songs.sort(key=playlist_group)
    assert [playlist_group(song) for song in songs] == sorted(playlist_group(song) for song in songs)
    if not args.verify:
        write_if_changed(disc / "manifest.json", (json.dumps(songs, indent=2) + "\n").encode())
    else:
        for song in songs:
            source, visual = disc / song["file"], disc / song["visual"]
            control = disc / song["file"].replace(".afx", ".afc")
            if (not source.is_file() or not visual.is_file() or not control.is_file() or
                    source.read_bytes() != (music / "music_controls" / song["file"]).read_bytes() or
                    control.read_bytes() != (music / "music_controls" / control.name).read_bytes() or
                    visual.read_bytes() != (music / song["visual"]).read_bytes()):
                raise ValueError(f"staged track differs: {song['title']}")

    header = ["static const struct song { const char *title, *file, *visual, *dsp; bool wraps; uint32_t bytes, control_bytes, sample_bytes, stream_bytes, setup_bytes, padding_bytes, command_baseline_bytes, note_count; uint16_t sample_count, setup_count; uint8_t sequence, channel_count, gain; } songs[] = {"]
    for song in songs:
        fields = ",".join(str(song[key]) for key in ("bytes", "control_bytes", "sample_bytes", "stream_bytes", "setup_bytes", "padding_bytes", "command_baseline_bytes", "note_count", "sample_count", "setup_count", "sequence", "channel_count", "gain"))
        header.append("{" + ",".join((json.dumps(song["title"]), json.dumps(song["file"]), json.dumps(song["visual"]), '"room"' if song["dsp"] else "NULL", "false", fields)) + "},")
    header.append("};")
    contents = ("\n".join(header) + "\n").encode()
    output = Path(__file__).resolve().parent / "include/songs.h"
    if args.verify:
        if not output.is_file() or hashlib.sha256(output.read_bytes()).digest() != hashlib.sha256(contents).digest():
            raise ValueError("generated playlist header differs")
    else:
        write_if_changed(output, contents)
    print(f"DKR music player: {len(songs)} tracks, {sum((disc / song['file']).stat().st_size for song in songs) if not args.verify else sum((music / song['file']).stat().st_size for song in songs)} byte flows")


if __name__ == "__main__":
    main()
