#!/usr/bin/env python3
"""Build DKR SFX banks from the tracked .afsfx residency map.

This script owns only the DKR-specific grouping/header policy. Each sound is
lowered by the native afx_n64 tool and each final pack by native afx_bank.
"""
import argparse
import json
import struct
import subprocess
from pathlib import Path


def read_map(path):
    count = None
    masks = None
    banks = {}
    scenes = {}
    voices = {}
    for raw in path.read_text().splitlines():
        fields = raw.split("#", 1)[0].split()
        if not fields:
            continue
        kind, values = fields[0], fields[1:]
        if kind == "scene-count" and len(values) == 1:
            count = int(values[0])
        elif kind == "vehicle-masks":
            masks = [int(value) for value in values]
        elif kind == "bank" and len(values) > 1:
            name, *ids = values
            if name in banks:
                raise ValueError(f"duplicate bank {name}")
            banks[name] = sorted({int(value) for value in ids})
        elif kind == "scene" and len(values) > 1:
            scene, *ids = values
            scene = int(scene)
            if scene in scenes:
                raise ValueError(f"duplicate scene {scene}")
            scenes[scene] = sorted({int(value) for value in ids})
        elif kind == "voice" and len(values) > 1:
            character, *ids = values
            character = int(character)
            if character in voices:
                raise ValueError(f"duplicate voice pack {character}")
            voices[character] = sorted({int(value) for value in ids})
        else:
            raise ValueError(f"{path}: invalid mapping line: {raw}")
    if count is None or count < 1 or masks is None or len(masks) != count or set(banks) != {"core", "vehicle"}:
        raise ValueError(f"{path}: incomplete SFX map")
    if any(mask < 0 or mask > 7 for mask in masks) or any(scene < 0 or scene >= count for scene in scenes):
        raise ValueError(f"{path}: scene or vehicle mask outside range")
    if any(sound < 1 for ids in [*banks.values(), *scenes.values(), *voices.values()] for sound in ids):
        raise ValueError(f"{path}: sound IDs are one-based")
    if set(banks["core"]) & set(banks["vehicle"]):
        raise ValueError(f"{path}: core and vehicle banks overlap")
    if any(character < 0 or character >= VOICE_CHARACTERS for character in voices):
        raise ValueError(f"{path}: voice character outside range")
    resident = set(banks["core"]) | set(banks["vehicle"])
    if any(resident & set(ids) for ids in voices.values()):
        raise ValueError(f"{path}: a voice pack repeats a core or vehicle sound")
    return count, masks, banks, scenes, voices


VOICE_CHARACTERS = 10  # Krunch .. Diddy, the game's racer character IDs.


def align(value):
    return (value + 31) & ~31


def aica_bytes(image, controls):
    magic, version, low, high, data_at, data_bytes, total, reserved = struct.unpack_from("<8I", image)
    if (magic, version, total, reserved) != (0x00424641, 1, len(image), 0) or not (low or high):
        raise ValueError("invalid AFB header or file size")
    if data_at != 32 or data_bytes != len(image) - data_at:
        raise ValueError("invalid AFB payload range")
    result = align(data_bytes)
    for flow in controls.values():
        header = struct.unpack_from("<20I", flow)
        if header[:3] != (0x32584641, 7, len(flow)):
            raise ValueError("invalid AFX control flow")
        result += align(header[5])
    return result


def write_pack(path, ids, control, samples, author, merger):
    raw = path.parent / f".{path.stem}.raw"
    raw.mkdir(parents=True, exist_ok=True)
    flows = []
    for sound_id in ids:
        flow = raw / f"{sound_id}.afx"
        subprocess.run((str(author), "--sfx", str(control), str(samples), str(sound_id), str(flow)),
                       check=True, stdout=subprocess.DEVNULL)
        flows.append(flow)
    path.parent.mkdir(parents=True, exist_ok=True)
    controls_dir = path.with_suffix("")
    controls_dir.mkdir(exist_ok=True)
    subprocess.run((str(merger), "--merge", str(path), str(controls_dir), *(str(flow) for flow in flows)),
                   check=True, stdout=subprocess.DEVNULL)
    controls = {sound_id: (controls_dir / f"{sound_id}.afx").read_bytes() for sound_id in ids}
    return aica_bytes(path.read_bytes(), controls)


def write_header(path, present, masks, sizes, core, vehicle, scene_ids, voices, voice_sizes):
    flat, first, counts = [], [], []
    voice_flat, voice_first, voice_counts = [], [], []
    for character in range(VOICE_CHARACTERS):
        voice_first.append(len(voice_flat))
        voice_flat.extend(voices.get(character, []))
        voice_counts.append(len(voices.get(character, [])))
    for level in range(len(present)):
        first.append(len(flat))
        ids = scene_ids.get(level, [])
        flat.extend(ids)
        counts.append(len(ids))
    path.write_text("/* Generated from dreamcast/aicaflow_tools/dkr.afsfx. */\n"
                    "#ifndef DKR_AICAFLOW_SFX_MANIFEST_H\n#define DKR_AICAFLOW_SFX_MANIFEST_H\n"
                    f"#define DKR_AFX_SCENE_COUNT {len(present)}u\n"
                    f"#define DKR_AFX_RESIDENT_SOUND_COUNT {len(core)}u\n"
                    f"#define DKR_AFX_VEHICLE_SOUND_COUNT {len(vehicle)}u\n"
                    "static const unsigned short dkr_afx_resident_sound_ids[] = {\n    " +
                    ", ".join(map(str, core)) + "\n};\n"
                    "static const unsigned short dkr_afx_vehicle_sound_ids[] = {\n    " +
                    ", ".join(map(str, vehicle)) + "\n};\n"
                    "static const unsigned char dkr_afx_scene_has_sfx[DKR_AFX_SCENE_COUNT] = {\n    " +
                    ", ".join(map(str, present)) + "\n};\n"
                    "static const unsigned char dkr_afx_scene_vehicle_mask[DKR_AFX_SCENE_COUNT] = {\n    " +
                    ", ".join(map(str, masks)) + "\n};\n"
                    "static const unsigned int dkr_afx_scene_bytes[DKR_AFX_SCENE_COUNT] = {\n    " +
                    ", ".join(map(str, sizes)) + "\n};\n"
                    "static const unsigned short dkr_afx_scene_sound_ids[] = {\n    " +
                    ", ".join(map(str, flat)) + "\n};\n"
                    "static const unsigned short dkr_afx_scene_sound_first[DKR_AFX_SCENE_COUNT] = {\n    " +
                    ", ".join(map(str, first)) + "\n};\n"
                    "static const unsigned short dkr_afx_scene_sound_count[DKR_AFX_SCENE_COUNT] = {\n    " +
                    ", ".join(map(str, counts)) + "\n};\n"
                    f"#define DKR_AFX_VOICE_PACK_COUNT {VOICE_CHARACTERS}u\n"
                    "static const unsigned short dkr_afx_voice_sound_ids[] = {\n    " +
                    ", ".join(map(str, voice_flat)) + "\n};\n"
                    "static const unsigned short dkr_afx_voice_sound_first[DKR_AFX_VOICE_PACK_COUNT] = {\n    " +
                    ", ".join(map(str, voice_first)) + "\n};\n"
                    "static const unsigned short dkr_afx_voice_sound_count[DKR_AFX_VOICE_PACK_COUNT] = {\n    " +
                    ", ".join(map(str, voice_counts)) + "\n};\n"
                    "static const unsigned int dkr_afx_voice_bytes[DKR_AFX_VOICE_PACK_COUNT] = {\n    " +
                    ", ".join(map(str, voice_sizes)) + "\n};\n"
                    "#endif\n")


def verify(output, count, masks, banks, scene_ids, voices):
    manifest = json.loads((output / "manifest.json").read_text())
    core, vehicle = banks["core"], banks["vehicle"]
    if (manifest["core"], manifest["vehicles"], manifest["scene_ids"], manifest["vehicle_masks"],
            manifest["voice_ids"]) != (
            core, vehicle, {str(level): scene_ids.get(level, []) for level in range(count)}, masks,
            {str(character): ids for character, ids in sorted(voices.items())}):
        raise ValueError("SFX manifest differs from .afsfx map")
    for name, ids in (("core.afb", core), ("vehicle.afb", vehicle)):
        controls = {sound_id: (output / name).with_suffix("").joinpath(f"{sound_id}.afx").read_bytes()
                    for sound_id in ids}
        aica_bytes((output / name).read_bytes(), controls)
    sizes = [aica_bytes((output / "scenes" / f"{level}.afb").read_bytes(),
                        {sound_id: (output / "scenes" / str(level) / f"{sound_id}.afx").read_bytes()
                         for sound_id in scene_ids[level]}) if level in scene_ids else 0
             for level in range(count)]
    if manifest["scene_bytes"] != sizes:
        raise ValueError("SFX scene sizes differ")
    voice_sizes = [aica_bytes((output / "voices" / f"{character}.afb").read_bytes(),
                              {sound_id: (output / "voices" / str(character) / f"{sound_id}.afx").read_bytes()
                               for sound_id in voices[character]}) if character in voices else 0
                   for character in range(VOICE_CHARACTERS)]
    if manifest["voice_bytes"] != voice_sizes:
        raise ValueError("SFX voice pack sizes differ")
    print(f"DKR AICAFLOW SFX map: {len(core)} resident, {len(vehicle)} vehicle, {len(scene_ids)} local scene banks, "
          f"{len(voices)} voice packs")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("root", type=Path)
    parser.add_argument("author", type=Path)
    parser.add_argument("merger", type=Path)
    parser.add_argument("mapping", type=Path)
    parser.add_argument("output", type=Path)
    parser.add_argument("--verify", action="store_true")
    args = parser.parse_args()
    root, output = args.root.resolve(), args.output.resolve()
    count, masks, banks, scene_ids, voices = read_map(args.mapping.resolve())
    if args.verify:
        verify(output, count, masks, banks, scene_ids, voices)
        return
    assets = root / "assets/.vanilla/us.v77/audio/unknown"
    control, samples = assets / "asset_audio_2.bin", assets / "asset_audio_3.bin"
    output.mkdir(parents=True, exist_ok=True)
    write_pack(output / "core.afb", banks["core"], control, samples, args.author, args.merger)
    write_pack(output / "vehicle.afb", banks["vehicle"], control, samples, args.author, args.merger)
    sizes = []
    for level in range(count):
        ids = scene_ids.get(level, [])
        sizes.append(write_pack(output / "scenes" / f"{level}.afb", ids, control, samples, args.author, args.merger) if ids else 0)
    voice_sizes = [write_pack(output / "voices" / f"{character}.afb", voices[character], control, samples,
                              args.author, args.merger) if character in voices else 0
                   for character in range(VOICE_CHARACTERS)]
    present = [int(level in scene_ids) for level in range(count)]
    write_header(output / "sfx_manifest.h", present, masks, sizes, banks["core"], banks["vehicle"], scene_ids,
                 voices, voice_sizes)
    (output / "manifest.json").write_text(json.dumps({"core": banks["core"], "vehicles": banks["vehicle"],
        "scene_ids": {str(level): scene_ids.get(level, []) for level in range(count)},
        "vehicle_masks": masks, "scene_bytes": sizes,
        "voice_ids": {str(character): ids for character, ids in sorted(voices.items())},
        "voice_bytes": voice_sizes}, indent=2) + "\n")
    verify(output, count, masks, banks, scene_ids, voices)


if __name__ == "__main__":
    main()
