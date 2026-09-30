#!/usr/bin/env python3
"""Export one independently loadable AFB per N64 sound for runtime fallback."""
import argparse
import hashlib
import json
import os
import sys
import struct
from concurrent.futures import ProcessPoolExecutor
from pathlib import Path

# Keep fallback semantics identical to the resident bank.  563..568 are the
# three player “get item” composites and their chained components.
def initialize_worker(tools, control, samples):
    global _control, _samples, _bank, _component_cache
    root = Path(tools).resolve().parents[1]
    sys.path.insert(0, str(root / 'third_party/aicaflow/tools/research'))
    sys.path.insert(0, tools)
    from afx_n64_cseq import ALBank
    _control, _samples = control, samples
    _bank = ALBank(control, samples)
    _component_cache = {}


def compile_sound(sound):
    from afx_n64_sfx import compile_pack
    return compile_pack(_control, _samples, [sound], bank=_bank,
                        component_cache=_component_cache)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('root', type=Path)
    parser.add_argument('tools', type=Path)
    parser.add_argument('output', type=Path)
    parser.add_argument('--verify', action='store_true')
    args = parser.parse_args()
    sys.path.insert(0, str(args.root.resolve() / 'third_party/aicaflow/tools/research'))
    sys.path.insert(0, str(args.tools.resolve()))
    from afx_n64_cseq import ALBank
    assets = args.root/'assets/.vanilla/us.v77/audio/unknown'
    control = (assets/'asset_audio_2.bin').read_bytes()
    samples = (assets/'asset_audio_3.bin').read_bytes()
    count = len(ALBank(control, samples).instrument(0)['sounds'])
    if args.verify:
        records = json.loads((args.output/'manifest.json').read_text())
        if [record['id'] for record in records] != list(range(1, count + 1)):
            raise ValueError('fallback manifest does not cover the source bank')
        for record in records:
            image = (args.output/f"{record['id']}.afb").read_bytes()
            flow = (args.output/'controls'/f"{record['id']}.afx").read_bytes()
            magic, version, low, high, data_at, data_bytes, total, reserved = struct.unpack_from('<8I', image)
            flow_magic, flow_version, flow_total = struct.unpack_from('<3I', flow)
            if ((magic, version, total, reserved) != (0x00424641, 1, len(image), 0) or
                    not (low or high) or data_at != 32 or data_bytes != len(image) - data_at or
                    (flow_magic, flow_version, flow_total) != (0x32584641, 7, len(flow)) or
                    len(image) != record['bytes'] or
                    hashlib.sha256(image).hexdigest() != record['sha256'] or
                    len(flow) != record['flow_bytes'] or
                    hashlib.sha256(flow).hexdigest() != record['flow_sha256']):
                raise ValueError(f"invalid fallback bank {record['id']}")
        print(f'Fallback banks verified: {count} independently loadable sounds')
        return
    args.output.mkdir(parents=True, exist_ok=True)
    records = []
    workers = min(8, os.cpu_count() or 1, count)
    with ProcessPoolExecutor(max_workers=workers, initializer=initialize_worker,
                             initargs=(str(args.tools.resolve()), control, samples)) as pool:
        # Keep neighbouring IDs in each process. Sound chains frequently point
        # nearby in this bank, so this lets the worker retain native-encoded
        # components instead of starting one cold compiler per output file.
        chunk = max(1, count // (workers * 4))
        for sound, (image, controls, info) in enumerate(pool.map(compile_sound, range(1, count + 1), chunksize=chunk), 1):
            path = args.output/f'{sound}.afb'
            temporary = path.with_suffix('.tmp')
            temporary.write_bytes(image)
            temporary.replace(path)
            flow = controls[sound]
            controls_dir = args.output/'controls'
            controls_dir.mkdir(exist_ok=True)
            flow_path = controls_dir/f'{sound}.afx'
            flow_temporary = flow_path.with_suffix('.tmp')
            flow_temporary.write_bytes(flow)
            flow_temporary.replace(flow_path)
            records.append({'id': sound, 'bytes': len(image),
                            'sample_bytes': info['sample_bytes'],
                            'sha256': hashlib.sha256(image).hexdigest(),
                            'flow_bytes': len(flow),
                            'flow_sha256': hashlib.sha256(flow).hexdigest()})
            print(f'Fallback {sound}/{count}: {len(image)} bytes', flush=True)
    # Only publish the manifest once every valid ID has been exported.
    (args.output/'manifest.json').write_text(json.dumps(records, indent=2) + '\n')


if __name__ == '__main__':
    main()
