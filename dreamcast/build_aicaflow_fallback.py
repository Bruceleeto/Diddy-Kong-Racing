#!/usr/bin/env python3
"""Export one independently loadable AFB per N64 sound for runtime fallback."""
import argparse
import hashlib
import json
import sys
import struct
from concurrent.futures import ProcessPoolExecutor
from pathlib import Path

# Keep fallback semantics identical to the resident bank.  563..568 are the
# three player “get item” composites and their chained components.
def initialize_worker(tools, control, samples):
    global _control, _samples
    sys.path.insert(0, tools)
    _control, _samples = control, samples


def compile_sound(sound):
    from afx_n64_sfx import compile_pack
    return compile_pack(_control, _samples, [sound])


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('root', type=Path)
    parser.add_argument('tools', type=Path)
    parser.add_argument('output', type=Path)
    parser.add_argument('--verify', action='store_true')
    args = parser.parse_args()
    sys.path.insert(0, str(args.tools.resolve()))
    from afx_n64 import ALBank
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
            magic, version, sounds, _, _, sound_at, _, _, _, total = struct.unpack_from('<4sHH7I', image)
            if ((magic, version, sounds, total) != (b'AFB1', 1, 1, len(image)) or
                    struct.unpack_from('<H', image, sound_at)[0] != record['id'] or
                    len(image) != record['bytes'] or
                    hashlib.sha256(image).hexdigest() != record['sha256']):
                raise ValueError(f"invalid fallback bank {record['id']}")
        print(f'Fallback banks verified: {count} independently loadable sounds')
        return
    args.output.mkdir(parents=True, exist_ok=True)
    records = []
    with ProcessPoolExecutor(max_workers=min(4, count), initializer=initialize_worker,
                             initargs=(str(args.tools.resolve()), control, samples)) as pool:
        for sound, (image, info) in enumerate(pool.map(compile_sound, range(1, count + 1)), 1):
            path = args.output/f'{sound}.afb'
            temporary = path.with_suffix('.tmp')
            temporary.write_bytes(image)
            temporary.replace(path)
            records.append({'id': sound, 'bytes': len(image),
                            'sample_bytes': info['sample_bytes'],
                            'sha256': hashlib.sha256(image).hexdigest()})
            print(f'Fallback {sound}/{count}: {len(image)} bytes', flush=True)
    # Only publish the manifest once every valid ID has been exported.
    (args.output/'manifest.json').write_text(json.dumps(records, indent=2) + '\n')


if __name__ == '__main__':
    main()
