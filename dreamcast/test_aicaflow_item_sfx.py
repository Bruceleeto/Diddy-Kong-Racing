#!/usr/bin/env python3
"""Player item pickups must finish their authored AICA flows."""
import struct
import sys
from pathlib import Path

root = Path(__file__).resolve().parent.parent
sys.path[:0] = [str(root / "dreamcast"), str(root / "third_party/aicaflow/tools")]
from afx_n64_sfx import HEADER, SOUND, CONTROLLED, afx_compile, compile_pack

assets = root / "assets/.vanilla/us.v77/audio/unknown"
table = (assets / "asset_audio_7.bin").read_bytes()
assert [struct.unpack_from(">H", table, sound * 10)[0] for sound in (160, 161, 162)] == [563, 565, 567]
image, _ = compile_pack((assets / "asset_audio_2.bin").read_bytes(),
                        (assets / "asset_audio_3.bin").read_bytes(), [563, 565, 567])
_, _, count, _, _, sound_at, _, _, _, _ = HEADER.unpack_from(image)
for index in range(count):
    sound = SOUND.unpack_from(image, sound_at + index * SOUND.size)
    assert not sound[2] & CONTROLLED
    assert image[sound[5] + sound[6] - 1] == afx_compile.AFX_OP_END
print("Item SFX: all three player pickup variants end instead of parking")
