#!/usr/bin/env python3
"""Scene-bank extraction follows AudioLine type, endianness and owner vertex."""
import json
import tempfile
import sys
from pathlib import Path
from build_aicaflow_sfx import level_ids
sys.path.insert(0, str(Path(__file__).resolve().parents[1]/'third_party/aicaflow/tools'))

with tempfile.TemporaryDirectory() as directory:
    root = Path(directory)
    maps = root/'assets/.vanilla/us.v77/levels/objectMaps/unknown'
    maps.mkdir(parents=True)
    objects = [
        dict(id='ASSET_OBJECT_AUDIO', soundId=91),
        dict(id='ASSET_OBJECT_AUDIOLINE', unk8=0, unkD=0, soundID=92),
        dict(id='ASSET_OBJECT_AUDIOLINE', unk8=0, unkD=1, soundID=600),
        dict(id='ASSET_OBJECT_AUDIOLINE', unk8=1, unkD=0, soundID=41),
        dict(id='ASSET_OBJECT_AUDIOSEQLINE', unk8=[1,100,0,41,0,0,0,0,0,0,0,0]),
        dict(id='ASSET_OBJECT_AUDIOSEQLINE', unk8=[0,100,1,2,0,0,0,0,0,0,0,0]),
        dict(id='ASSET_OBJECT_AUDIOSEQLINE', unk8=[0,100,1,3,0,1,0,0,0,0,0,0]),
        dict(id='ASSET_OBJECT_AUDIO', soundId=0),
        dict(id='ASSET_OBJECT_ANIMATION', unk1E=1, soundEffect=2),
        dict(id='ASSET_OBJECT_ANIMATION', unk1E=255, soundEffect=2),
        dict(id='ASSET_OBJECT_ANIMATION', unk1E=0, soundEffect=2),
    ]
    (maps/'asset_level_object_maps_0.gltf').write_text(json.dumps({'nodes': [{'extras': e} for e in objects]}))
    assert level_ids(root, {'map-2':'MAP_0','map-collectables':'MAP_0'}, [0, 89, 99]) == {89,91,92,258}
print('Scene IDs: SFX, jingles, first vertex and big-endian IDs passed')
