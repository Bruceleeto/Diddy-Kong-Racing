#!/usr/bin/env python3
"""Compile selected N64 ALBank sounds into one AFB and sample-free AFX flows."""

import argparse
import hashlib
import json
import math
import struct
import sys
from pathlib import Path

# This keeps DKR's N64 importer small while sharing the current AICAflow
# compiler and codec implementation.
sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "third_party/aicaflow/tools/research"))

import afx_compile
from afx_n64_cseq import ALBank, N64Error, _attenuation, _rate_for_us


AFB_MAGIC = 0x00424641  # "AFB\\0"
AFB_HEADER = struct.Struct("<8I")
AFX_HEADER = struct.Struct("<20I")
AFX_RELOCATION = struct.Struct("<3I")
AFX_FILE_MAGIC = 0x32584641
AFX_FILE_VERSION = 7
AFX_FLAG_CONTROLLED = 1
OP_PARK = 0x13
FIELD_COUNT = 18
FIELD_CONTROL = 0
FIELD_LOOP_START = 2
FIELD_LOOP_END = 3
FIELD_ENV_AD = 4
FIELD_ENV_DR = 5
FIELD_FILTER_START = 11
FIELD_FILTER_END = 15
MIN_SNR_DB = 30.0
MIN_ATTACK_SNR_DB = 24.0


def _snr(source: bytes, restored: bytes) -> tuple[float, float]:
    frames = min(len(source), len(restored)) // 2
    original = struct.unpack(f"<{frames}h", source[:frames * 2])
    decoded = struct.unpack(f"<{frames}h", restored[:frames * 2])

    def measure(limit: int) -> float:
        signal = sum(value * value for value in original[:limit])
        noise = sum((a - b) ** 2 for a, b in zip(original[:limit], decoded[:limit]))
        return math.inf if not noise else 10 * math.log10(max(1, signal) / noise)

    return measure(frames), measure(min(frames, 1024))


def _encoded_candidate(raw: bytes, source_rate: int, rate: int, loop: bool) -> tuple[bytes, int, int]:
    """Choose the smallest AICA representation that survives N64-source A/B."""
    sampled = afx_compile.resample_pcm16(raw, source_rate, rate)
    frames = len(sampled) // 2
    candidates = [(sampled, afx_compile.PCM16)]
    candidates.append((afx_compile.pcm16_to_pcm8(sampled), afx_compile.PCM8))
    if not loop:
        candidates.append((afx_compile.pcm16_to_adpcm(sampled), afx_compile.ADPCM))
    accepted = []
    for encoded, format_id in candidates:
        if format_id == afx_compile.PCM16:
            decoded = encoded
        elif format_id == afx_compile.PCM8:
            decoded = afx_compile.pcm8_to_pcm16(encoded)
        else:
            decoded = afx_compile.aica_adpcm_decode(encoded, frames)
        restored = afx_compile.resample_pcm16(decoded, rate, source_rate)
        full, attack = _snr(raw, restored)
        if full >= MIN_SNR_DB and attack >= MIN_ATTACK_SNR_DB:
            accepted.append((encoded, format_id, full, attack))
    if not accepted:
        raise N64Error("PCM16 source did not survive its own rate conversion")
    encoded, format_id, full, attack = min(
        accepted, key=lambda item: (len(item[0]), item[1], -full, -attack))
    return encoded, frames, format_id


def _sample(raw: bytes, source_rate: int, max_rate: int, loop: bool) -> tuple[bytes, int, int, int]:
    rates = [max_rate] + [rate for rate in (16000, 11025, 8000) if 4000 <= rate < max_rate]
    candidates = []
    for rate in rates:
        try:
            encoded, frames, format_id = _encoded_candidate(raw, source_rate, rate, loop)
            candidates.append((encoded, frames, format_id, rate))
        except N64Error:
            continue
    if not candidates:
        raise N64Error("no AICA sample representation meets the N64 quality floor")
    return min(candidates, key=lambda item: (len(item[0]), -item[3], item[2]))


def align(value: int, alignment: int = 32) -> int:
    return (value + alignment - 1) & -alignment


def _pitch_word(rate: int, sound: dict) -> tuple[int, int, float]:
    # DKR starts pitch-slide sounds at keyBase; detune is its per-tick slide.
    slide_cents = sound["root_key"] * 100 - 6000
    if not sound["key_max"] & 0x20:
        slide_cents += sound["detune"]
    cents = slide_cents + afx_compile.aica_rate_tuning_cents(rate)
    return afx_compile.pitch(60, 60, cents), slide_cents, 2 ** (cents / 1200)


def _setup(sound: dict, bank_rate: int) -> tuple[bytes, dict]:
    loop = sound["loop"] if sound["loop"] and sound["loop"][2] else None
    frames = len(sound["pcm16_bytes"]) // 2
    rate = min(bank_rate, max(4000, 65535 * bank_rate // frames))
    raw, frames, format_id, rate = _sample(sound["pcm16_bytes"], bank_rate, rate, bool(loop))
    if loop:
        loop_start = round(loop[0] * rate / bank_rate)
        loop_end = round(loop[1] * rate / bank_rate) - 1
        if not 0 <= loop_start < loop_end < frames:
            raise N64Error(f"resampled loop {loop_start}..{loop_end} is outside {frames} frames")
    else:
        loop_start, loop_end = 0, frames - 1

    pitch, slide_cents, playback_ratio = _pitch_word(rate, sound)
    envelope = sound["envelope"]
    envelope_scale = 2 ** (slide_cents / 1200)
    attack_us = max(0, envelope["attack_us"]) / envelope_scale
    decay_us = envelope["decay_us"]
    release_us = max(0, envelope["release_us"]) / envelope_scale
    attack_rate = _rate_for_us(round(attack_us), afx_compile.AR_TIME_MS)
    decay_rate = 0 if decay_us < 0 else _rate_for_us(round(decay_us / envelope_scale), afx_compile.DR_TIME_MS)
    release_rate = _rate_for_us(round(release_us), afx_compile.DR_TIME_MS)
    attack_volume = max(1, envelope["attack_volume"])
    decay_volume = max(1, envelope["decay_volume"])
    decay_level = max(0, min(31, round(-20 * math.log10(decay_volume / attack_volume) / 3.0103)))

    fields = [0] * FIELD_COUNT
    fields[FIELD_CONTROL] = 0x0200 if loop else 0
    fields[FIELD_LOOP_START] = loop_start
    fields[FIELD_LOOP_END] = loop_end
    fields[FIELD_ENV_AD] = attack_rate | decay_rate << 6
    fields[FIELD_ENV_DR] = release_rate | decay_level << 5 | 15 << 10
    fields[afx_compile.AFX_FIELD_PITCH] = pitch
    fields[afx_compile.AFX_FIELD_DSP_SEND] = (sound["key_max"] & 15) << 4
    fields[afx_compile.AFX_FIELD_DIRECT] = afx_compile.aica_direct(
        {"source_pan": "apply"}, channel_pan=sound["sample_pan"])
    fields[afx_compile.AFX_FIELD_TOTAL_LEVEL] = afx_compile.aica_total_level(
        _attenuation(sound["sample_volume"], attack_volume))
    fields[FIELD_FILTER_START:FIELD_FILTER_END + 1] = [0x1FFF] * 5

    metadata = {
        "raw": raw, "frames": frames, "format": format_id, "resample_hz": rate,
        "fields": fields, "group": sound["key_min"] & 0x3F,
        "base_fx": sound["key_max"] & 15,
        "slide_cents": sound["detune"] if sound["key_max"] & 0x20 else 0,
        "source_pan": sound["sample_pan"], "loop": bool(loop),
        "sustain": bool(loop and decay_us < 0),
        "duration_ticks": max(1, round(frames / (44100 * playback_ratio) * 1000)),
        "keyoff_ticks": None if decay_us < 0 else max(1, round((attack_us + decay_us / envelope_scale) / 1000)),
        "release_ticks": max(1, round(release_us / 1000)),
    }
    return raw, metadata


def _chain(sounds: list[dict], sound_id: int) -> list[tuple[int, dict]]:
    chain, seen = [], set()
    while sound_id:
        if sound_id in seen:
            raise N64Error(f"sound chain loops at {sound_id}")
        if not 1 <= sound_id <= len(sounds):
            raise N64Error(f"sound chain selects {sound_id}, outside 1..{len(sounds)}")
        if len(chain) == 16:
            raise N64Error("sound chain needs more than 16 AICA channels")
        seen.add(sound_id)
        sound = sounds[sound_id - 1]
        chain.append((sound_id, sound))
        sound_id = sound["velocity_min"] + (sound["key_min"] & 0xC0) * 4
    return chain


def _control_flow(bank_id: tuple[int, int], payload_offsets: list[int],
                  components: list[tuple[int, dict]], stream: bytes,
                  channels: int, controlled: bool) -> bytes:
    """Emit one fixed-layout AFX whose setups address the containing AFB."""
    setups = bytearray(len(components) * FIELD_COUNT * 2)
    relocations = bytearray(len(components) * AFX_RELOCATION.size)
    for index, (sample_index, setup) in enumerate(components):
        offset = payload_offsets[sample_index]
        fields = list(setup["fields"])
        fields[FIELD_CONTROL] = ((fields[FIELD_CONTROL] & ~0x1FF) |
                                 (setup["format"] << 7) | (offset >> 16))
        fields[1] = offset & 0xFFFF
        struct.pack_into("<18H", setups, index * FIELD_COUNT * 2, *fields)
        AFX_RELOCATION.pack_into(relocations, index * AFX_RELOCATION.size,
                                 index * FIELD_COUNT * 2, offset, len(setup["raw"]))
    image = setups + stream
    image_at = align(AFX_HEADER.size + len(relocations))
    result = bytearray(image_at + len(image))
    flags = AFX_FLAG_CONTROLLED if controlled else 0
    control_id = struct.unpack_from("<I", hashlib.sha256(
        struct.pack("<5I", flags, channels, 1000, *bank_id) + relocations + image).digest())[0] or 1
    AFX_HEADER.pack_into(result, 0, AFX_FILE_MAGIC, AFX_FILE_VERSION, len(result),
                         flags,
                         image_at, len(image), len(setups), len(stream),
                         control_id, len(components), bank_id[0], bank_id[1],
                         AFX_HEADER.size, len(components), 0, 0,
                         channels, 1000, 1, 0)
    result[AFX_HEADER.size:AFX_HEADER.size + len(relocations)] = relocations
    result[image_at:] = image
    return bytes(result)


def compile_pack(control: bytes, samples: bytes, sound_ids: list[int], *,
                 bank: ALBank | None = None, component_cache: dict | None = None) -> tuple[bytes, dict[int, bytes], dict]:
    """Compile one bank, optionally reusing caller-owned parse/encode caches."""
    if bank is None:
        bank = ALBank(control, samples)
    sounds = bank.instrument(0)["sounds"]
    requested = sorted(set(sound_ids))
    if not requested:
        raise N64Error("at least one sound id is required")

    sample_records, sample_by_key, plans = [], {}, []
    if component_cache is None:
        component_cache = {}
    for requested_id in requested:
        if not 1 <= requested_id <= len(sounds):
            raise N64Error(f"sound id {requested_id} is outside 1..{len(sounds)}")
        chain = _chain(sounds, requested_id)
        starts, cursor_us = [], 0
        for _, sound in chain:
            starts.append(round(cursor_us / 1000))
            cursor_us += sound["velocity_max"] * 33333

        events, flow_end, controlled, components = [], 0, False, []
        for channel, ((component_id, sound), start) in enumerate(zip(chain, starts)):
            cached = component_cache.get(component_id)
            if cached is None:
                _, cached = _setup(sound, bank.sample_rate)
                component_cache[component_id] = cached
            raw = cached["raw"]
            identity = (hashlib.sha256(raw).digest(), cached["format"], cached["frames"])
            sample_index = sample_by_key.setdefault(identity, len(sample_records))
            if sample_index == len(sample_records):
                sample_records.append((raw, cached["frames"], cached["format"]))
            setup_index = len(components)
            components.append((sample_index, cached))
            events.append((start, 1, afx_compile.encode_note(
                channel, setup_index,
                cached["fields"][afx_compile.AFX_FIELD_PITCH],
                cached["fields"][afx_compile.AFX_FIELD_TOTAL_LEVEL])))
            if cached["sustain"]:
                controlled = True
            elif cached["keyoff_ticks"] is not None:
                keyoff = start + cached["keyoff_ticks"]
                events.append((keyoff, 0, bytes((afx_compile.AFX_OP_KEYOFF, channel))))
                flow_end = max(flow_end, keyoff + cached["release_ticks"])
            flow_end = max(flow_end, start + cached["duration_ticks"])

        events.sort(key=lambda event: (event[0], event[1]))
        stream, previous = bytearray(), 0
        for tick, _, encoded in events:
            stream += afx_compile.encode_wait(tick - previous)
            stream += encoded
            previous = tick
        if controlled:
            stream.append(OP_PARK)
        else:
            stream += afx_compile.encode_wait(flow_end - previous)
            stream.append(afx_compile.AFX_OP_END)
        plans.append((requested_id, components, bytes(stream), len(chain), controlled))

    cursor, sample_offsets = 0, []
    for raw, _, _ in sample_records:
        cursor = align(cursor)
        sample_offsets.append(cursor)
        cursor += len(raw)
    payload = bytearray(cursor)
    for (raw, _, _), offset in zip(sample_records, sample_offsets):
        payload[offset:offset + len(raw)] = raw
    digest = hashlib.sha256(payload).digest()
    bank_id = struct.unpack_from("<2I", digest)
    if not any(bank_id): bank_id = (1, 0)
    output = bytearray(AFB_HEADER.size + len(payload))
    AFB_HEADER.pack_into(output, 0, AFB_MAGIC, 1, *bank_id, AFB_HEADER.size,
                         len(payload), len(output), 0)
    output[AFB_HEADER.size:] = payload
    controls = {sound_id: _control_flow(bank_id, sample_offsets, components, stream,
                                        channels, controlled)
                for sound_id, components, stream, channels, controlled in plans}
    diagnostics = {
        "version": 1, "sounds": requested, "sound_count": len(plans),
        "sample_count": len(sample_records), "setup_count": sum(len(plan[1]) for plan in plans),
        "pcm16_bytes": sum(len(raw) for raw, _, format_id in sample_records
                           if format_id == afx_compile.PCM16),
        "pcm8_bytes": sum(len(raw) for raw, _, format_id in sample_records
                          if format_id == afx_compile.PCM8),
        "adpcm_bytes": sum(len(raw) for raw, _, format_id in sample_records
                           if format_id == afx_compile.ADPCM),
        "sample_bytes": sum(len(raw) for raw, _, _ in sample_records),
        "stream_bytes": sum(len(plan[2]) for plan in plans), "total_bytes": len(output),
        "flow_bytes": sum(len(flow) for flow in controls.values()),
    }
    return bytes(output), controls, diagnostics


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("control", type=Path)
    parser.add_argument("samples", type=Path)
    parser.add_argument("output", type=Path)
    parser.add_argument("--controls-dir", type=Path, required=True)
    parser.add_argument("sound", type=int, nargs="+")
    parser.add_argument("--diagnostics", type=Path)
    args = parser.parse_args(argv)
    try:
        image, controls, diagnostics = compile_pack(args.control.read_bytes(), args.samples.read_bytes(), args.sound)
        args.output.write_bytes(image)
        args.controls_dir.mkdir(parents=True, exist_ok=True)
        for sound_id, flow in controls.items():
            (args.controls_dir / f"{sound_id}.afx").write_bytes(flow)
        if args.diagnostics:
            args.diagnostics.write_text(json.dumps(diagnostics, indent=2) + "\n")
    except (OSError, N64Error, afx_compile.CompileError, struct.error) as error:
        print(f"afx-n64-sfx: {error}", file=sys.stderr)
        return 2
    print(f"wrote {args.output}: {diagnostics}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
