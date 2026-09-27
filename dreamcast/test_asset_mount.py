#!/usr/bin/env python3
"""Mount selection updates its header only when the selected path changes."""
from pathlib import Path
import subprocess
import tempfile

source = Path(__file__).resolve().parents[1].joinpath('Makefile.dc').read_text()
config = source[source.index('DKR_ASSET_MOUNT ?='):source.index('DEFINES :=')]
rules = source[source.index('.PHONY: force-asset-mount'):source.index('$(BUILD)/dreamcast/reimpl.o $(BUILD)/dreamcast/audio_aicaflow.o:')]
with tempfile.TemporaryDirectory() as directory:
    root = Path(directory)
    (root / 'Makefile').write_text('BUILD := build\n' + config + rules)
    header = root / 'build/dkr_asset_mount.h'
    def build(mount):
        return subprocess.run(['make', 'build/dkr_asset_mount.h', 'DKR_ASSET_MOUNT=' + mount], cwd=root, capture_output=True)
    for mount in ('/pc', '/cd', '/pc'):
        assert build(mount).returncode == 0
        assert header.read_text() == f'#define DKR_ASSET_MOUNT "{mount}"\n'
        timestamp = header.stat().st_mtime_ns
        assert build(mount).returncode == 0
        assert header.stat().st_mtime_ns == timestamp
    assert build('/invalid').returncode != 0
print('Asset mount: pc/cd/pc switching, unchanged timestamps and invalid mount passed')
