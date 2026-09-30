#!/usr/bin/env python3
"""Compile an N64 compact sequence and ALBank into an AICAFLOW file.

The importer is deliberately strict: malformed offsets, unsupported codebooks
and missing key/velocity zones are errors rather than best-effort audio.
"""

from __future__ import annotations

import argparse
import functools
import math
import struct
import sys
from pathlib import Path

# The parser is DKR-specific; shared compiler support comes from the pinned
# AICAflow submodule rather than a second copy of its research modules.
sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "third_party/aicaflow/tools/research"))

import mido

import afx_adpcm
import afx_compile
import afx_midi


class N64Error(ValueError):
    pass


def _clip16(value: int) -> int:
    return max(-32768, min(32767, value))


class CSeqReader:
    def __init__(self, data: bytes, offset: int):
        self.data, self.pos = data, offset
        self.backup_pos = self.backup_left = 0

    def read(self) -> int:
        if self.backup_left:
            if self.backup_pos >= len(self.data):
                raise N64Error("CSeq backup reads past sequence")
            value = self.data[self.backup_pos]
            self.backup_pos += 1
            self.backup_left -= 1
            return value
        if self.pos >= len(self.data):
            raise N64Error("CSeq track ends without end-of-track")
        value = self.data[self.pos]
        self.pos += 1
        if value != 0xFE:
            return value
        if self.pos >= len(self.data):
            raise N64Error("truncated CSeq block code")
        high = self.data[self.pos]
        self.pos += 1
        if high == 0xFE:
            return 0xFE
        if self.pos + 2 > len(self.data):
            raise N64Error("truncated CSeq backup block")
        distance = high << 8 | self.data[self.pos]
        length = self.data[self.pos + 1]
        self.pos += 2
        start = self.pos - distance - 4
        if length == 0 or start < 0 or start + length > len(self.data):
            raise N64Error("invalid CSeq backup block")
        self.backup_pos, self.backup_left = start + 1, length - 1
        return self.data[start]

    def varlen(self) -> int:
        value = self.read()
        if value & 0x80:
            value &= 0x7F
            while True:
                byte = self.read()
                value = value << 7 | byte & 0x7F
                if not byte & 0x80:
                    break
        return value


def _channel_message(status: int, first: int, second: int | None) -> mido.Message | None:
    if first > 0x7F or second is not None and second > 0x7F:
        return None
    channel, kind = status & 15, status & 0xF0
    if kind == 0x80:
        return mido.Message("note_off", channel=channel, note=first, velocity=second)
    if kind == 0x90:
        return mido.Message("note_on", channel=channel, note=first, velocity=second)
    if kind == 0xA0:
        return mido.Message("polytouch", channel=channel, note=first, value=second)
    if kind == 0xB0:
        return mido.Message("control_change", channel=channel, control=first, value=second)
    if kind == 0xC0:
        return mido.Message("program_change", channel=channel, program=first)
    if kind == 0xD0:
        return mido.Message("aftertouch", channel=channel, value=first)
    if kind == 0xE0:
        return mido.Message("pitchwheel", channel=channel, pitch=((second << 7) | first) - 8192)
    raise N64Error(f"unsupported CSeq status 0x{status:02x}")


def parse_cseq(data: bytes, source: str = "<CSeq>") -> tuple[dict, list[dict]]:
    if len(data) < 68:
        raise N64Error("CSeq is shorter than its header")
    offsets = struct.unpack_from(">16I", data)
    division = struct.unpack_from(">I", data, 64)[0]
    if not 1 <= division <= 0x7FFF:
        raise N64Error(f"invalid CSeq division {division}")
    midi = mido.MidiFile(type=1, ticks_per_beat=division)
    loops = []
    for track_index, offset in enumerate(offsets):
        if not offset:
            continue
        if not 68 <= offset < len(data):
            raise N64Error(f"track {track_index} offset is outside sequence")
        reader = CSeqReader(data, offset)
        tick, order, status = 0, 0, 0
        events = []
        delta = reader.varlen()
        loop_counts = {}
        for _ in range(1_000_000):
            tick += delta
            byte = reader.read()
            if byte == 0xFF:
                meta = reader.read()
                if meta == 0x51:
                    tempo = reader.read() << 16 | reader.read() << 8 | reader.read()
                    if not tempo:
                        raise N64Error(f"track {track_index} has zero tempo")
                    events.append((tick, order, mido.MetaMessage("set_tempo", tempo=tempo)))
                    order += 1
                    status = 0
                elif meta == 0x2F:
                    break
                elif meta == 0x2E:
                    reader.read()
                    reader.read()
                    status = 0
                elif meta == 0x2D:
                    if reader.backup_left:
                        raise N64Error("CSeq loop end inside a backup block is unsupported")
                    loop_site = reader.pos
                    initial, current = reader.read(), reader.read()
                    distance = reader.read() << 24 | reader.read() << 16 | reader.read() << 8 | reader.read()
                    if current == 0xFF:
                        # The N64 player keeps this track alive forever.  An
                        # offline one-shot flow stops at its first loop
                        # boundary, including the zero-distance form used by
                        # DKR's Ancient Lake track.
                        loops.append({"track": track_index, "start_offset": reader.pos - distance,
                                      "end_offset": reader.pos, "end_tick": tick})
                        break
                    if distance:
                        remaining = loop_counts.setdefault(loop_site, current)
                        target = reader.pos - distance
                        if not offset <= target < reader.pos:
                            raise N64Error(f"track {track_index} has invalid loop target")
                        if remaining:
                            loop_counts[loop_site] = remaining - 1
                            reader.pos = target
                        else:
                            loop_counts[loop_site] = initial
                    status = 0
                else:
                    # libaudio ignores unknown compact-sequence meta bytes and
                    # advances to the following event delta.
                    pass
            else:
                if byte & 0x80:
                    status, first = byte, reader.read()
                elif status:
                    first = byte
                else:
                    raise N64Error(f"track {track_index} uses running status before a status byte")
                kind = status & 0xF0
                second = None if kind in (0xC0, 0xD0) else reader.read()
                message = _channel_message(status, first, second)
                if message:
                    events.append((tick, order, message))
                    order += 1
                if kind == 0x90 and second:
                    duration = reader.varlen()
                    if message:
                        events.append((tick + duration, order,
                                       mido.Message("note_off", channel=status & 15,
                                                    note=first, velocity=0)))
                        order += 1
            delta = reader.varlen()
        else:
            raise N64Error(f"track {track_index} exceeds the event limit")
        track = mido.MidiTrack()
        previous = 0
        for when, _, message in sorted(events, key=lambda event: event[:2]):
            track.append(message.copy(time=when - previous))
            previous = when
        track.append(mido.MetaMessage("end_of_track", time=0))
        midi.tracks.append(track)
    if not midi.tracks:
        raise N64Error("CSeq contains no tracks")
    timeline = afx_midi.parse_midi(midi, source)
    for note in timeline["notes"]:
        note["lane"] = note["channel"]
    timeline["n64_loops"] = loops
    return timeline, loops


def sequence_from_file(data: bytes, index: int) -> bytes:
    if len(data) < 4 or data[:2] != b"S1":
        raise N64Error("sequence file is not an S1 ALSeqFile")
    count = struct.unpack_from(">H", data, 2)[0]
    if not 0 <= index < count:
        raise N64Error(f"sequence index {index} is outside 0..{count - 1}")
    offset, length = struct.unpack_from(">II", data, 4 + index * 8)
    if offset < 4 + count * 8 or length < 68 or offset + length > len(data):
        raise N64Error(f"sequence {index} has an invalid range")
    return data[offset:offset + length]


class ALBank:
    def __init__(self, control: bytes, samples: bytes, bank_index: int = 0):
        self.control, self.samples = control, samples
        if len(control) < 8 or control[:2] != b"B1":
            raise N64Error("bank is not a B1 ALBankFile")
        count = self.u16(2)
        if not 0 <= bank_index < count:
            raise N64Error(f"bank index {bank_index} is outside 0..{count - 1}")
        self.bank_offset = self.u32(4 + bank_index * 4)
        self._span(self.bank_offset, 12)
        self.instrument_count = self.s16(self.bank_offset)
        self.sample_rate = self.u32(self.bank_offset + 4)
        self.percussion = self.u32(self.bank_offset + 8)
        if not 1 <= self.instrument_count <= 128 or not 4000 <= self.sample_rate <= 96000:
            raise N64Error("bank header has implausible instrument count or sample rate")
        self.instruments = [self.u32(self.bank_offset + 12 + index * 4)
                            for index in range(self.instrument_count)]
        self._sample_cache = {}

    def _span(self, offset: int, size: int) -> None:
        if offset < 0 or size < 0 or offset + size > len(self.control):
            raise N64Error(f"bank range 0x{offset:x}+{size} is outside control file")

    def u16(self, offset: int) -> int:
        self._span(offset, 2)
        return struct.unpack_from(">H", self.control, offset)[0]

    def s16(self, offset: int) -> int:
        self._span(offset, 2)
        return struct.unpack_from(">h", self.control, offset)[0]

    def u32(self, offset: int) -> int:
        self._span(offset, 4)
        return struct.unpack_from(">I", self.control, offset)[0]

    def s32(self, offset: int) -> int:
        self._span(offset, 4)
        return struct.unpack_from(">i", self.control, offset)[0]

    def instrument(self, program: int, percussion: bool = False) -> dict:
        offset = self.percussion if percussion and self.percussion else (self.instruments[program]
                 if 0 <= program < len(self.instruments) else 0)
        if not offset:
            offset = next((value for value in self.instruments if value), 0)
        self._span(offset, 16)
        count = self.s16(offset + 14)
        if not 1 <= count <= 1024:
            raise N64Error(f"instrument {program} has invalid sound count {count}")
        return {"volume": self.control[offset], "pan": self.control[offset + 1],
                "bend_range": self.s16(offset + 12),
                "sounds": [self.sound(self.u32(offset + 16 + index * 4)) for index in range(count)]}

    def sound(self, offset: int) -> dict:
        self._span(offset, 16)
        envelope, keymap, wavetable = self.u32(offset), self.u32(offset + 4), self.u32(offset + 8)
        self._span(envelope, 16)
        self._span(keymap, 6)
        return {"envelope": {"attack_us": self.s32(envelope), "decay_us": self.s32(envelope + 4),
                             "release_us": self.s32(envelope + 8),
                             "attack_volume": self.control[envelope + 12],
                             "decay_volume": self.control[envelope + 13]},
                "velocity_min": self.control[keymap], "velocity_max": self.control[keymap + 1],
                "key_min": self.control[keymap + 2], "key_max": self.control[keymap + 3],
                "root_key": self.control[keymap + 4],
                "detune": struct.unpack_from("b", self.control, keymap + 5)[0],
                "sample_pan": self.control[offset + 12], "sample_volume": self.control[offset + 13],
                **self.wavetable(wavetable)}

    def wavetable(self, offset: int) -> dict:
        self._span(offset, 20)
        if offset in self._sample_cache:
            return self._sample_cache[offset]
        start, length, kind = self.u32(offset), self.s32(offset + 4), self.control[offset + 8]
        if length <= 0 or start + length > len(self.samples):
            raise N64Error("wavetable sample range is outside table")
        loop = None
        if kind == 0:
            book_offset, loop_offset = self.u32(offset + 16), self.u32(offset + 12)
            order, predictors = self.s32(book_offset), self.s32(book_offset + 4)
            if order != 2 or not 1 <= predictors <= 16:
                raise N64Error(f"unsupported VADPCM book order={order}, predictors={predictors}")
            coefficients = [self.s16(book_offset + 8 + index * 2)
                            for index in range(order * predictors * 8)]
            raw = decode_vadpcm(self.samples[start:start + length], coefficients, predictors)
            if loop_offset:
                loop = (self.u32(loop_offset), self.u32(loop_offset + 4), self.u32(loop_offset + 8))
        elif kind == 1:
            if length & 1:
                raise N64Error("RAW16 wavetable has an odd byte length")
            raw = struct.pack(f"<{length // 2}h", *struct.unpack(f">{length // 2}h", self.samples[start:start + length]))
            loop_offset = self.u32(offset + 12)
            if loop_offset:
                loop = (self.u32(loop_offset), self.u32(loop_offset + 4), self.u32(loop_offset + 8))
        else:
            raise N64Error(f"unsupported wavetable type {kind}")
        frames = len(raw) // 2
        if loop and not 0 <= loop[0] < loop[1] <= frames:
            raise N64Error(f"wavetable loop {loop[:2]} is outside {frames} decoded samples")
        result = {"pcm16_bytes": raw, "loop": loop,
                  "source_format": "vadpcm" if kind == 0 else "raw16",
                  "source_offset": start,
                  "source_bytes": length}
        self._sample_cache[offset] = result
        return result


def decode_vadpcm(data: bytes, coefficients: list[int], predictors: int) -> bytes:
    # libaudio's loader makes this same whole-frame truncation; bank builders
    # commonly include up to eight alignment bytes in ALWaveTable.len.
    data = data[:len(data) // 9 * 9]
    if not data:
        raise N64Error("VADPCM wavetable contains no complete frame")
    native = afx_adpcm.n64_vadpcm_decode(data, coefficients, predictors)
    if native is not None:
        return native
    output, last1, last2 = [], 0, 0
    for frame_offset in range(0, len(data), 9):
        header = data[frame_offset]
        scale, predictor = header >> 4, header & 15
        if predictor >= predictors:
            raise N64Error(f"VADPCM frame selects missing predictor {predictor}")
        packed = data[frame_offset + 1:frame_offset + 9]
        residual = []
        for byte in packed:
            residual.extend(((byte >> 4 if byte >> 4 < 8 else (byte >> 4) - 16) << scale,
                             (byte & 15 if byte & 15 < 8 else (byte & 15) - 16) << scale))
        book1 = coefficients[predictor * 16:predictor * 16 + 8]
        book2 = coefficients[predictor * 16 + 8:predictor * 16 + 16]
        decoded = []
        for half in range(2):
            block = residual[half * 8:half * 8 + 8]
            for index, value in enumerate(block):
                accumulator = book1[index] * last2 + book2[index] * last1
                accumulator += sum(book2[index - prior - 1] * block[prior]
                                   for prior in range(index))
                decoded.append(_clip16((accumulator >> 11) + value))
            last2, last1 = decoded[-2:]
        output.extend(decoded)
    return struct.pack(f"<{len(output)}h", *output)


def _rate_for_us(microseconds: int, table: tuple) -> int:
    if microseconds <= 0:
        return 30
    # aica_rate consumes SoundFont timecents, whose zero is one second in
    # milliseconds.  N64 AL envelopes are microseconds.
    return afx_compile.aica_rate(round(1200 * math.log2(microseconds / 1_000_000)), table)


def _attenuation(*levels: int) -> float:
    gain = math.prod(max(1, level) / 127 for level in levels)
    return max(0.0, -2000 * math.log10(gain))


@functools.cache
def _sample_format(raw: bytes, loop: tuple | None) -> str:
    if loop:
        return "pcm16"
    encoded = afx_compile.pcm16_to_adpcm(raw)
    decoded = afx_compile.aica_adpcm_decode(encoded, len(raw) // 2)
    source = struct.unpack(f"<{len(raw) // 2}h", raw)
    restored = struct.unpack(f"<{len(decoded) // 2}h", decoded)
    signal = sum(value * value for value in source)
    noise = sum((a - b) ** 2 for a, b in zip(source, restored))
    snr = math.inf if not noise else 10 * math.log10(max(1, signal) / noise)
    return "adpcm" if snr >= 24 else "pcm16"


def mapping_for_timeline(timeline: dict, bank: ALBank) -> dict:
    instruments, overrides = {}, {}
    for note in timeline["notes"]:
        instrument = bank.instrument(note["program"], note["channel"] == 9)
        matches = [sound for sound in instrument["sounds"]
                   if sound["key_min"] <= note["key"] <= sound["key_max"] and
                   sound["velocity_min"] <= note["velocity"] <= sound["velocity_max"]]
        if not matches:
            # DKR's bank has placeholder instruments (one silent 0..0/0..1
            # zone) that libaudio leaves silent. They are not AICA voices.
            continue
        if len(matches) != 1:
            raise N64Error(f"note {note['id']} selects {len(matches)} bank sounds")
        sound = matches[0]
        loop = sound["loop"] if sound["loop"] and sound["loop"][2] else None
        frames = len(sound["pcm16_bytes"]) // 2
        rate = min(bank.sample_rate, max(4000, 65535 * bank.sample_rate // frames))
        if round(frames * rate / bank.sample_rate) > 65535:
            raise N64Error(f"note {note['id']} sample cannot fit an AICA slot")
        envelope = sound["envelope"]
        attack_volume = max(1, envelope["attack_volume"])
        decay_volume = max(1, envelope["decay_volume"])
        decay_level = max(0, min(31, round(-20 * math.log10(decay_volume / attack_volume) / 3.0103)))
        kind = "wavetable" if loop else "raw_pcm16"
        zone = {"name": f"program {note['program']} note {note['key']}",
                "key_min": sound["key_min"], "key_max": sound["key_max"],
                "velocity_min": sound["velocity_min"], "velocity_max": sound["velocity_max"],
                "root_key": sound["root_key"], "tuning_cents": sound["detune"],
                "source_rate": bank.sample_rate, "resample_hz": rate,
                "pcm16_bytes": sound["pcm16_bytes"]}
        if loop:
            zone.update(loop_start=loop[0], loop_end=loop[1])
        config = {"kind": kind, "velocity": "midi", "release": "keyoff", "zones": [zone],
                  "sample_format": _sample_format(sound["pcm16_bytes"], loop),
                  "attack_rate": _rate_for_us(envelope["attack_us"], afx_compile.AR_TIME_MS),
                  "decay_rate": _rate_for_us(envelope["decay_us"], afx_compile.DR_TIME_MS),
                  "decay_level": decay_level, "sustain_decay_rate": 0,
                  "release_rate": _rate_for_us(envelope["release_us"], afx_compile.DR_TIME_MS),
                  "attenuation_centibels": _attenuation(instrument["volume"], sound["sample_volume"], attack_volume),
                  "midi_reverb": "apply", "source_pan": "apply"}
        key = afx_compile.mapping_key(note)
        instruments.setdefault(key, config)
        overrides[str(note["id"])] = config
        note["channel_pan"] = max(0, min(127, note["channel_pan"] + instrument["pan"] - 64 + sound["sample_pan"] - 64))
        note["pitch_bend_range_cents"] = max(0, instrument["bend_range"])
    timeline["notes"] = [note for note in timeline["notes"]
                         if str(note["id"]) in overrides]
    return {"title": Path(timeline["source"]).stem, "instruments": instruments,
            "note_overrides": overrides, "event_cluster_limit": 4}


def compile_sequence_bank(control: bytes, samples: bytes, sequence_file: bytes, index: int,
                          tick_rate: int = 1000):
    sequence = sequence_from_file(sequence_file, index)
    timeline, loops = parse_cseq(sequence, f"N64 sequence {index}")
    mapping = mapping_for_timeline(timeline, ALBank(control, samples))
    bank, flow, seek, diagnostics = afx_compile.compile_bank_flow_timeline(timeline, mapping, tick_rate)
    diagnostics["n64"] = {"sequence_index": index, "source_loops": loops}
    return bank, flow, seek, diagnostics


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("control", type=Path, help="B1 ALBank control file")
    parser.add_argument("samples", type=Path, help="ALBank sample table")
    parser.add_argument("sequences", type=Path, help="S1 ALSeqFile containing compact sequences")
    parser.add_argument("index", type=int)
    parser.add_argument("output", type=Path)
    parser.add_argument("--bank", type=Path,
                        help="AFB output path (defaults to OUTPUT with an .afb suffix)")
    parser.add_argument("--tick-rate", type=int, default=1000)
    parser.add_argument("--quiet", action="store_true")
    args = parser.parse_args(argv)
    try:
        inputs = (args.control.read_bytes(), args.samples.read_bytes(), args.sequences.read_bytes())
        bank, flow, seek, diagnostics = compile_sequence_bank(*inputs, args.index, args.tick_rate)
        bank_path = args.bank or args.output.with_suffix(".afb")
        bank_path.write_bytes(bank)
        args.output.write_bytes(flow)
        if seek: args.output.with_suffix(".afc").write_bytes(seek)
        diagnostics = {**diagnostics, "bank": str(bank_path), "bank_bytes": len(bank),
                       "flow_bytes": len(flow)}
    except (OSError, N64Error, afx_compile.CompileError) as error:
        print(f"afx-n64: {error}", file=sys.stderr)
        return 2
    if not args.quiet:
        print(f"wrote {args.output}: {diagnostics}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
