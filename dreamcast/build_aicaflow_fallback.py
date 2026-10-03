#!/usr/bin/env python3
"""Export one native AFX/AFB pair per N64 sound for on-demand fallback."""
import argparse
import hashlib
import json
import os
import struct
import subprocess
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path


def sound_count(control):
    data = control.read_bytes()
    if len(data) < 8 or data[:2] != b"B1":
        raise ValueError("not an ALBank B1 file")
    banks = struct.unpack_from(">H", data, 2)[0]
    if not banks:
        raise ValueError("empty ALBank")
    bank = struct.unpack_from(">I", data, 4)[0]
    if bank + 16 > len(data):
        raise ValueError("truncated ALBank")
    instruments = struct.unpack_from(">h", data, bank)[0]
    if instruments < 1 or bank + 12 + 4 * instruments > len(data):
        raise ValueError("invalid ALBank instruments")
    instrument = next((struct.unpack_from(">I", data, bank + 12 + 4 * index)[0]
                       for index in range(instruments)
                       if struct.unpack_from(">I", data, bank + 12 + 4 * index)[0]), 0)
    if not instrument or instrument + 16 > len(data):
        raise ValueError("missing ALBank instrument")
    count = struct.unpack_from(">h", data, instrument + 14)[0]
    if count < 1 or instrument + 16 + 4 * count > len(data):
        raise ValueError("invalid ALInstrument sound list")
    return count


def compile_sound(task):
    author, control, samples, output, sound = task
    controls = output / "controls"
    controls.mkdir(exist_ok=True)
    temporary = output / f".{sound}.afx"
    subprocess.run((str(author), "--sfx", str(control), str(samples), str(sound), str(temporary)),
                   check=True, stdout=subprocess.DEVNULL)
    temporary.with_suffix(".afb").replace(output / f"{sound}.afb")
    temporary.replace(controls / f"{sound}.afx")
    image = (output / f"{sound}.afb").read_bytes()
    flow = (controls / f"{sound}.afx").read_bytes()
    return {"id": sound, "bytes": len(image), "sha256": hashlib.sha256(image).hexdigest(),
            "flow_bytes": len(flow), "flow_sha256": hashlib.sha256(flow).hexdigest()}


def verify(output, count):
    records = json.loads((output / "manifest.json").read_text())
    if [record["id"] for record in records] != list(range(1, count + 1)):
        raise ValueError("fallback manifest does not cover the source bank")
    for record in records:
        image = (output / f"{record['id']}.afb").read_bytes()
        flow = (output / "controls" / f"{record['id']}.afx").read_bytes()
        bank = struct.unpack_from("<8I", image)
        header = struct.unpack_from("<3I", flow)
        if ((bank[0], bank[1], bank[6], bank[7]) != (0x00424641, 1, len(image), 0) or
                bank[4] != 32 or bank[5] != len(image) - 32 or not (bank[2] or bank[3]) or
                header != (0x32584641, 7, len(flow)) or
                len(image) != record["bytes"] or hashlib.sha256(image).hexdigest() != record["sha256"] or
                len(flow) != record["flow_bytes"] or hashlib.sha256(flow).hexdigest() != record["flow_sha256"]):
            raise ValueError(f"invalid fallback bank {record['id']}")
    print(f"Fallback banks verified: {count} independently loadable C-authored sounds")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("root", type=Path)
    parser.add_argument("author", type=Path)
    parser.add_argument("output", type=Path)
    parser.add_argument("--verify", action="store_true")
    args = parser.parse_args()
    assets = args.root.resolve() / "assets/.vanilla/us.v77/audio/unknown"
    control, samples, output = assets / "asset_audio_2.bin", assets / "asset_audio_3.bin", args.output.resolve()
    count = sound_count(control)
    if args.verify:
        verify(output, count)
        return
    output.mkdir(parents=True, exist_ok=True)
    workers = min(8, os.cpu_count() or 1, count)
    tasks = [(args.author.resolve(), control, samples, output, sound) for sound in range(1, count + 1)]
    with ThreadPoolExecutor(max_workers=workers) as pool:
        records = list(pool.map(compile_sound, tasks))
    (output / "manifest.json").write_text(json.dumps(records, indent=2) + "\n")
    verify(output, count)


if __name__ == "__main__":
    main()
