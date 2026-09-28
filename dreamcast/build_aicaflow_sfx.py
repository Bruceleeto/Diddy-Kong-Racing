#!/usr/bin/env python3
"""Build the resident, vehicle, and level-local DKR AICAFLOW SFX banks."""

import argparse
import json
import re
import struct
import sys
from pathlib import Path


TITLE_IDS = {17, 22, 261, 262}
RACE_IDS = {1, 4, 5, 10, 19, 21, 22, 24, 25, 35, 41, 47, 55, 74, 76, 77, 83,
            114, 122, 128, 139, 155, 180, 189, 200, 218, 224, 225, 226, 227,
            228, 231, 232, 233, 234, 235, 236, 237, 243, 257, 266, 269, 286,
            289, 304, 505, 677}
ITEM_IDS = {563, 565, 567}
ALWAYS_IDS = {7}  # SOUND_BALLOON_POP after DKR's gSoundTable translation.
EXTRA_BY_LEVEL = {
    36: {3, 4, 66, 68, 69, 77, 269, 270},
    # The boot logo runs in Frontend. These are the raw sound bites
    # behind SOUND_INTRO_PLANE and SOUND_INTRO_KIDS.
    21: {89, 91},
}
VEHICLE_NAMES = ("VEHICLE_CAR", "VEHICLE_HOVERCRAFT", "VEHICLE_PLANE")


def sound_ids_header(path):
    return {match.group(2): int(match.group(1), 16)
            for match in re.finditer(r"/\* 0x([0-9A-Fa-f]+) \*/\s*(SOUND_[A-Z0-9_]+)",
                                     path.read_text())}


def sound_table(path):
    data = path.read_bytes()
    if len(data) % 10:
        raise ValueError(f"{path}: invalid SoundData table")
    return [struct.unpack_from(">H", data, offset)[0] for offset in range(0, len(data), 10)]


def static_ids(root, table):
    names = sound_ids_header(root / "include/sound_ids.h")
    ids = {25, 76}  # sound_play_direct and audspat_play_sound_direct constants.
    pattern = re.compile(r"\b(?:sound_play|sound_play_spatial|audspat_play_sound_at_position)\(\s*(SOUND_[A-Z0-9_]+)")
    for path in (root / "src").glob("*.c"):
        for name in pattern.findall(path.read_text()):
            index = names[name]
            if index < len(table) and table[index]:
                ids.add(table[index])
    return ids


def vehicle_ids(path):
    data = path.read_bytes()
    if len(data) != 30 * 76:
        raise ValueError(f"{path}: invalid VehicleSoundAsset table")
    ids = set()
    for offset in range(0, len(data), 76):
        ids.update(struct.unpack_from(">HH", data, offset))
        ids.add(data[offset + 54])
    return ids - {0}


def level_ids(root, header, table):
    direct, logical = set(), set()
    for key in ("map-2", "map-collectables"):
        index = int(header[key].rsplit("_", 1)[1])
        path = root / f"assets/.vanilla/us.v77/levels/objectMaps/unknown/asset_level_object_maps_{index}.gltf"
        for node in json.loads(path.read_text()).get("nodes", []):
            extra = node.get("extras", {})
            if extra.get("id") == "ASSET_OBJECT_AUDIO":
                direct.add(extra["soundId"])
            if extra.get("id") == "ASSET_OBJECT_AUDIOLINE":
                if extra["unk8"] == 0 and extra["unkD"] == 0:
                    direct.add(extra["soundID"])
            if extra.get("id") == "ASSET_OBJECT_AUDIOSEQLINE":
                raw = extra["unk8"]
                # Same AudioLine layout; only type SOUND and vertex zero own SFX.
                # Type JINGLE refers to a music sequence, not a sample-bank sound.
                if raw[0] == 0 and raw[5] == 0:
                    direct.add(raw[2] << 8 | raw[3])
            if extra.get("id") == "ASSET_OBJECT_ANIMATION":
                # obj64->soundID comes from unk1E; 0 is idle, 255 stops the sound.
                # The misleading soundEffect field selects a fade colour.
                sound = extra.get("unk1E", 0) & 255
                if sound not in (0, 255):
                    logical.add(sound)
    return (direct - {0}) | {table[index] for index in logical if 0 <= index < len(table) and table[index]}


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


def write_pack(path, ids, control, samples, bank, component_cache):
    image, controls, _ = compile_pack(control, samples, sorted(ids), bank=bank,
                                      component_cache=component_cache)
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(image)
    controls_dir = path.with_suffix("")
    controls_dir.mkdir(exist_ok=True)
    for sound_id, flow in controls.items():
        (controls_dir / f"{sound_id}.afx").write_bytes(flow)
    return aica_bytes(image, controls)


def write_header(path, present, vehicles, sizes, core, vehicle, scene_ids):
    flat, first, counts = [], [], []
    for level in range(len(present)):
        first.append(len(flat))
        ids = sorted(scene_ids[level])
        flat.extend(ids)
        counts.append(len(ids))
    path.write_text("/* Generated by dreamcast/build_aicaflow_sfx.py. */\n"
                    "#ifndef DKR_AICAFLOW_SFX_MANIFEST_H\n#define DKR_AICAFLOW_SFX_MANIFEST_H\n"
                    f"#define DKR_AFX_SCENE_COUNT {len(present)}u\n"
                    f"#define DKR_AFX_RESIDENT_SOUND_COUNT {len(core)}u\n"
                    f"#define DKR_AFX_VEHICLE_SOUND_COUNT {len(vehicle)}u\n"
                    "static const unsigned short dkr_afx_resident_sound_ids[] = {\n    " +
                    ", ".join(map(str, sorted(core))) + "\n};\n"
                    "static const unsigned short dkr_afx_vehicle_sound_ids[] = {\n    " +
                    ", ".join(map(str, sorted(vehicle))) + "\n};\n"
                    "static const unsigned char dkr_afx_scene_has_sfx[DKR_AFX_SCENE_COUNT] = {\n    " +
                    ", ".join(map(str, present)) + "\n};\n"
                    "static const unsigned char dkr_afx_scene_vehicle_mask[DKR_AFX_SCENE_COUNT] = {\n    " +
                    ", ".join(map(str, vehicles)) + "\n};\n"
                    "static const unsigned int dkr_afx_scene_bytes[DKR_AFX_SCENE_COUNT] = {\n    " +
                    ", ".join(map(str, sizes)) + "\n};\n"
                    "static const unsigned short dkr_afx_scene_sound_ids[] = {\n    " +
                    ", ".join(map(str, flat)) + "\n};\n"
                    "static const unsigned short dkr_afx_scene_sound_first[DKR_AFX_SCENE_COUNT] = {\n    " +
                    ", ".join(map(str, first)) + "\n};\n"
                    "static const unsigned short dkr_afx_scene_sound_count[DKR_AFX_SCENE_COUNT] = {\n    " +
                    ", ".join(map(str, counts)) + "\n};\n"
                    "#endif\n")


def main():
    global compile_pack
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("root", type=Path)
    parser.add_argument("aicaflow_tools", type=Path)
    parser.add_argument("output", type=Path)
    parser.add_argument("--verify", action="store_true")
    args = parser.parse_args()
    root, output = args.root.resolve(), args.output.resolve()
    sys.path.insert(0, str(args.aicaflow_tools.resolve()))
    from afx_n64 import ALBank
    from afx_n64_sfx import compile_pack

    assets = root / "assets/.vanilla/us.v77"
    control = (assets / "audio/unknown/asset_audio_2.bin").read_bytes()
    samples = (assets / "audio/unknown/asset_audio_3.bin").read_bytes()
    # Every output bank is independent at runtime, but many resolve the same
    # N64 wavetable. Keep one decoded bank and native-encoded component cache
    # for this one offline export pass.
    bank, component_cache = ALBank(control, samples), {}
    table = sound_table(assets / "audio/unknown/asset_audio_7.bin")
    # These sound-table targets are selected arithmetically in gameplay code,
    # so static_ids() cannot discover all of them. They must be ready before a
    # pickup can be heard; the remaining one-shots stay scene-local/fallback
    # to preserve AICA RAM for the music bank.
    core = RACE_IDS | TITLE_IDS | ITEM_IDS | ALWAYS_IDS
    vehicles = vehicle_ids(assets / "audio/unknown/asset_audio_8.bin") - core
    level_metadata = json.loads((assets / "asset_level_headers.meta.json").read_text())["files"]
    order = level_metadata["order"]
    header_filenames = level_metadata["sections"]
    scene_ids, present, masks = {}, [], []
    for level, enum in enumerate(order):
        header = json.loads((assets / "levels/headers" /
                             header_filenames[enum]["filename"]).read_text())
        ids = level_ids(root, header, table) | EXTRA_BY_LEVEL.get(level, set())
        ids -= core | vehicles
        scene_ids[level] = ids
        present.append(int(bool(ids)))
        masks.append(sum(1 << index for index, name in enumerate(VEHICLE_NAMES)
                         if name in (header.get("avaliable-vehicles") or [])))

    if args.verify:
        manifest = json.loads((output / "manifest.json").read_text())
        header = (output / "sfx_manifest.h").read_text()
        assert manifest["core"] == sorted(core)
        assert manifest["vehicles"] == sorted(vehicles)
        assert manifest["scene_ids"] == {str(level): sorted(ids) for level, ids in scene_ids.items()}
        assert manifest["vehicle_masks"] == masks
        for name, ids in (("core.afb", core), ("vehicle.afb", vehicles)):
            controls = {sound_id: (output / name).with_suffix("").joinpath(f"{sound_id}.afx").read_bytes()
                        for sound_id in ids}
            aica_bytes((output / name).read_bytes(), controls)
        sizes = [aica_bytes((output / "scenes" / f"{level}.afb").read_bytes(),
                            {sound_id: (output / "scenes" / str(level) / f"{sound_id}.afx").read_bytes()
                             for sound_id in ids}) if ids else 0
                 for level, ids in scene_ids.items()]
        assert manifest["scene_bytes"] == sizes
        assert ", ".join(map(str, sizes)) in header
        assert not core & vehicles
        assert "True" not in header and "False" not in header
        print(f"DKR AICAFLOW SFX manifest: {len(core)} resident, {len(vehicles)} vehicle, "
              f"{sum(present)} local scene banks")
        return

    output.mkdir(parents=True, exist_ok=True)
    scenes = output / "scenes"
    scenes.mkdir(exist_ok=True)
    for name, ids in (("core.afb", core), ("vehicle.afb", vehicles)):
        write_pack(output / name, ids, control, samples, bank, component_cache)
    sizes = []
    for level, ids in scene_ids.items():
        size = 0
        if ids:
            size = write_pack(scenes / f"{level}.afb", ids, control, samples, bank, component_cache)
        sizes.append(size)
    write_header(output / "sfx_manifest.h", present, masks, sizes, core, vehicles, scene_ids)
    (output / "manifest.json").write_text(json.dumps({
        "core": sorted(core), "static_ids": sorted(static_ids(root, table)),
        "vehicles": sorted(vehicles),
        "scene_ids": {str(level): sorted(ids) for level, ids in scene_ids.items()},
        "vehicle_masks": masks, "scene_bytes": sizes,
    }, indent=2) + "\n")
    print(f"DKR AICAFLOW SFX banks: {len(core)} resident, {len(vehicles)} vehicle, "
          f"{sum(present)} local scene banks")


if __name__ == "__main__":
    main()
