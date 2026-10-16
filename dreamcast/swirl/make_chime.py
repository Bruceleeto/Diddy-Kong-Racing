#!/usr/bin/env python3
"""Make the chime that sounds when the Dreamcast swirl appears on the boot screen.

usage: make_chime.py OUT_DIR MODE LEFT.wav RIGHT.wav AFX_COMPILE AFX_PROFILE

The chime is not in the repository: like the N64 ROM it is read from files the
builder has, named at build time (SWIRL_CHIME_LEFT and SWIRL_CHIME_RIGHT in
Makefile.dc). Each is a 44.1 kHz 16-bit WAV holding one take of the chime in
one of its channels (the louder one is taken). An empty or missing name means
no chime: OUT_DIR is left without chime.afb and chime.afx, and the game plays
nothing.

MODE is
  centred   the left file's take alone, in the middle;
  ringring  the left file's take on the left, then the right file's take on
            the right RING_GAP seconds later.

Each take is made mono, its leading silence cut, its end faded over 5 ms once
it has died away, and scaled to just under full scale so the AICA has all of
its precision; it is stored as PCM16. The note's velocity sets how loud it
plays, as AICA attenuation: (127 - VELOCITY) * 0.75 dB below full scale.
afx_compile makes the bank and the flow (one note a take); for ringring,
afx_profile then sets each note's pan.

OUT_DIR/chime.stamp records what the chime was made from. It is rewritten only
when that changes, so the build redoes what depends on the chime only then.
"""
import hashlib
import json
import subprocess
import sys
import wave
from pathlib import Path

import numpy as np

RATE = 44100
VELOCITY = 109      # 13.5 dB below full scale: a little under the intro music
RING_GAP = 0.15     # s between the two rings of ringring
LEAD = 16           # samples kept before a take's first sound
SILENT = 10 ** (-66 / 20)
FADE = 0.005        # s
MAX_FRAMES = 65279  # afx_compile's limit for a one-shot
# AICA direct-send words: full level (DISDL 15), panned hard to one side.
DIRECT_LEFT = 0x0F1F
DIRECT_RIGHT = 0x0F0F
TICKS_A_SECOND = 960  # the MIDI file's 480 ticks a beat at 120 bpm
VERSION = 1           # of what this script makes, for the stamp


def take(path):
    """The chime in path's louder channel, mono, trimmed and scaled, as floats."""
    with wave.open(str(path), "rb") as source:
        if source.getframerate() != RATE or source.getsampwidth() != 2:
            raise SystemExit(f"{path}: a 44.1 kHz 16-bit WAV is needed")
        channels = source.getnchannels()
        frames = np.frombuffer(source.readframes(source.getnframes()), "<i2").reshape(-1, channels) / 32768.0
    signal = frames[:, int(np.argmax(np.sqrt(np.mean(frames * frames, axis=0))))]
    loud = np.flatnonzero(np.abs(signal) > SILENT)
    if not len(loud):
        raise SystemExit(f"{path}: silent")
    first = max(int(loud[0]) - LEAD, 0)
    last = min(int(loud[-1]) + int(FADE * RATE), len(signal))
    signal = signal[first:last].copy()
    if len(signal) > MAX_FRAMES:
        raise SystemExit(f"{path}: longer than {MAX_FRAMES} frames once trimmed")
    fade = int(FADE * RATE)
    signal[-fade:] *= 0.5 + 0.5 * np.cos(np.pi * np.arange(fade) / fade)
    return signal * (0.94 / np.max(np.abs(signal)))


def pcm16(samples):
    return np.round(np.clip(samples, -1.0, 1.0) * 32767.0).astype("<i2").tobytes()


def midi(notes):
    """A format-0 MIDI file of (start s, end s, key) notes at VELOCITY."""
    def varlen(value):
        out = [value & 0x7F]
        value >>= 7
        while value:
            out.insert(0, 0x80 | (value & 0x7F))
            value >>= 7
        return bytes(out)

    events = []
    for start, end, key in notes:
        events.append((round(start * TICKS_A_SECOND), 1, bytes([0x90, key, VELOCITY])))
        events.append((int(np.ceil(end * TICKS_A_SECOND)), 0, bytes([0x80, key, 0x40])))
    track, now = b"\x00\xff\x51\x03\x07\xa1\x20", 0  # 500000 us a beat
    for tick, _, message in sorted(events):
        track += varlen(tick - now) + message
        now = tick
    track += b"\x00\xff\x2f\x00"
    return b"MThd" + (6).to_bytes(4, "big") + bytes([0, 0, 0, 1, 1, 0xE0]) + \
        b"MTrk" + len(track).to_bytes(4, "big") + track


def build(out, mode, left, right, compiler, profiler):
    takes = [take(left)] if mode == "centred" else [take(left), take(right)]
    zones, notes = [], []
    for i, samples in enumerate(takes):
        pcm = out / f"take{i}.pcm"
        pcm.write_bytes(pcm16(samples))
        key = 69 + i  # one key, and one zone, a take
        zones.append(f"{key} {key} {key} -1 -1 pcm16 {pcm}")
        start = i * RING_GAP
        # Released once the take, and the one-shot's silent safety tail, have played.
        notes.append((start, start + len(samples) / RATE + 0.02, key))
    (out / "chime.zones").write_text("# key_min key_max root_key loop_start loop_end format pcm_path\n" +
                                     "\n".join(zones) + "\n")
    (out / "chime.mid").write_bytes(midi(notes))
    flow = out / "chime.afx"
    base = flow if mode == "centred" else out / "chime.base.afx"
    subprocess.run([compiler, str(out / "chime.mid"), "--zones", str(out / "chime.zones"),
                    str(out / "chime.afb"), str(base)], check=True, stdout=subprocess.DEVNULL)
    if mode == "centred":
        return
    # Each ring to its side, as the rings alternate in Sonic.
    profile = out / "chime.afp"
    subprocess.run([profiler, "init", str(base), str(profile), "dry", "0"], check=True, stdout=subprocess.DEVNULL)
    inventory = json.loads(subprocess.run([profiler, "inventory", str(base)], check=True,
                                          capture_output=True, text=True).stdout)
    if len(inventory) != 2:
        raise SystemExit(f"{base}: two notes expected, not {len(inventory)}")
    document = json.loads(profile.read_text())
    document["overrides"] = [{"event": note["event"], "parameters": {"direct": direct}}
                             for note, direct in zip(sorted(inventory, key=lambda n: n["event"]["tick"]),
                                                     (DIRECT_LEFT, DIRECT_RIGHT))]
    profile.write_text(json.dumps(document, indent=2) + "\n")
    subprocess.run([profiler, "apply", str(base), str(base.with_suffix(".afc")), str(profile),
                    str(flow), str(out / "chime.afc")], check=True, stdout=subprocess.DEVNULL)


def main():
    if len(sys.argv) != 7:
        sys.exit(__doc__)
    out, mode, left, right, compiler, profiler = Path(sys.argv[1]), *sys.argv[2:]
    if mode not in ("centred", "ringring"):
        sys.exit(f"chime mode {mode!r}: centred or ringring")
    sources = [left] if mode == "centred" else [left, right]
    present = all(name and Path(name).is_file() for name in sources)
    key = hashlib.sha256(f"{VERSION} {mode} {present} {Path(__file__).read_bytes().hex()}".encode())
    for name in sources if present else []:
        key.update(Path(name).read_bytes())
    stamp = out / "chime.stamp"
    if stamp.is_file() and stamp.read_text().split()[0] == key.hexdigest():
        return
    out.mkdir(parents=True, exist_ok=True)
    for old in out.iterdir():
        old.unlink()
    if present:
        build(out, mode, Path(left), Path(right), compiler, profiler)
        print(f"swirl chime: {mode} from {', '.join(sources)}")
    else:
        print("swirl chime: none (SWIRL_CHIME_LEFT/SWIRL_CHIME_RIGHT unset or missing)")
    stamp.write_text(f"{key.hexdigest()} {mode if present else 'none'}\n")


if __name__ == "__main__":
    main()
