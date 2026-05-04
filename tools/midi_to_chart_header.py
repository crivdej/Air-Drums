#!/usr/bin/env python3

import argparse
import re
from dataclasses import dataclass
from pathlib import Path


DIFFICULTY_BASE_PITCH = {
    "easy": 60,
    "medium": 72,
    "hard": 84,
    "expert": 96,
}

DIFFICULTIES = tuple(DIFFICULTY_BASE_PITCH) + ("super_easy",)

CURRENT_CODE_LANES = {
    "hihat": 0,
    "kick": 1,
    "snare": 2,
}

DRUM_COLOR_OFFSETS = {
    "kick": 0,
    "snare": 1,
    "yellow": 2,
    "blue": 3,
    "green": 4,
}


@dataclass
class MidiTrack:
    name: str
    notes: list[tuple[int, int]]
    tempos: list[tuple[int, int]]


@dataclass
class MidiFile:
    ticks_per_quarter: int
    tracks: list[MidiTrack]


@dataclass
class ChartNote:
    time_ms: int
    lane: int
    label: str


def read_variable_length(data: bytes, pos: int) -> tuple[int, int]:
    value = 0

    while True:
        if pos >= len(data):
            raise ValueError("unexpected end of midi data while reading variable length value")

        byte = data[pos]
        pos += 1
        value = (value << 7) | (byte & 0x7F)

        if (byte & 0x80) == 0:
            return value, pos


def read_midi(path: Path) -> MidiFile:
    data = path.read_bytes()
    pos = 0

    def read(count: int) -> bytes:
        nonlocal pos
        if pos + count > len(data):
            raise ValueError("unexpected end of midi file")
        value = data[pos : pos + count]
        pos += count
        return value

    def read_u16() -> int:
        return int.from_bytes(read(2), "big")

    def read_u32() -> int:
        return int.from_bytes(read(4), "big")

    if read(4) != b"MThd":
        raise ValueError("input does not look like a midi file")

    header_length = read_u32()
    if header_length < 6:
        raise ValueError("midi header is too short")

    midi_format = read_u16()
    track_count = read_u16()
    ticks_per_quarter = read_u16()

    if header_length > 6:
        read(header_length - 6)

    if midi_format not in (0, 1):
        raise ValueError(f"unsupported midi format {midi_format}")

    if ticks_per_quarter & 0x8000:
        raise ValueError("smpte time division is not supported")

    tracks: list[MidiTrack] = []

    for _ in range(track_count):
        if read(4) != b"MTrk":
            raise ValueError("expected midi track chunk")

        track_length = read_u32()
        track_end = pos + track_length
        tick = 0
        running_status: int | None = None
        track_name = ""
        notes: list[tuple[int, int]] = []
        tempos: list[tuple[int, int]] = []

        while pos < track_end:
            delta, pos = read_variable_length(data, pos)
            tick += delta

            status = data[pos]
            if status < 0x80:
                if running_status is None:
                    raise ValueError("midi event is missing running status")
                status = running_status
            else:
                pos += 1
                if status < 0xF0:
                    running_status = status

            if status == 0xFF:
                meta_type = data[pos]
                pos += 1
                length, pos = read_variable_length(data, pos)
                payload = data[pos : pos + length]
                pos += length

                if meta_type == 0x03:
                    track_name = payload.decode("latin1", errors="replace")
                elif meta_type == 0x51 and length == 3:
                    tempos.append((tick, int.from_bytes(payload, "big")))

                continue

            if status in (0xF0, 0xF7):
                length, pos = read_variable_length(data, pos)
                pos += length
                continue

            event_type = status & 0xF0
            if event_type in (0xC0, 0xD0):
                pos += 1
                continue

            pitch = data[pos]
            velocity = data[pos + 1]
            pos += 2

            if event_type == 0x90 and velocity > 0:
                notes.append((tick, pitch))

        pos = track_end
        tracks.append(MidiTrack(track_name, notes, tempos))

    return MidiFile(ticks_per_quarter, tracks)


def find_drum_track(midi: MidiFile) -> MidiTrack:
    for track in midi.tracks:
        if track.name.upper() == "PART DRUMS":
            return track

    for track in midi.tracks:
        if "DRUM" in track.name.upper():
            return track

    raise ValueError("could not find a PART DRUMS track")


def find_track_named(midi: MidiFile, name: str) -> MidiTrack | None:
    wanted_name = name.upper()

    for track in midi.tracks:
        if track.name.upper() == wanted_name:
            return track

    return None


def build_tempo_map(midi: MidiFile) -> list[tuple[int, int]]:
    tempos: list[tuple[int, int]] = []

    for track in midi.tracks:
        tempos.extend(track.tempos)

    if not tempos:
        return [(0, 500000)]

    tempos.sort()
    if tempos[0][0] != 0:
        tempos.insert(0, (0, 500000))

    return tempos


def tick_to_ms(tick: int, ticks_per_quarter: int, tempos: list[tuple[int, int]]) -> int:
    total_us = 0.0
    last_tick = 0
    current_tempo = 500000

    for tempo_tick, tempo in tempos:
        if tempo_tick > tick:
            break

        total_us += ((tempo_tick - last_tick) * current_tempo) / ticks_per_quarter
        last_tick = tempo_tick
        current_tempo = tempo

    total_us += ((tick - last_tick) * current_tempo) / ticks_per_quarter
    return int(round(total_us / 1000.0))


def destination_lane_for_color(color: str, args: argparse.Namespace) -> str | None:
    if color == "kick":
        return "kick"
    if color == "snare":
        return "snare"
    if color == "yellow":
        return args.yellow_to
    if color == "blue":
        return args.blue_to
    if color == "green":
        return args.green_to

    return None


def first_and_last_drum_tick(drum_track: MidiTrack) -> tuple[int, int]:
    drum_ticks = [tick for tick, pitch in drum_track.notes if 24 <= pitch <= 116]
    if not drum_ticks:
        raise ValueError("PART DRUMS does not contain any drum notes")

    return min(drum_ticks), max(drum_ticks)


def generate_tempo_grid_beats(
    start_tick: int,
    end_tick: int,
    ticks_per_quarter: int,
) -> list[int]:
    first_beat = ((start_tick + ticks_per_quarter - 1) // ticks_per_quarter) * ticks_per_quarter
    return list(range(first_beat, end_tick + 1, ticks_per_quarter))


def build_super_easy_notes(midi: MidiFile, args: argparse.Namespace) -> list[ChartNote]:
    drum_track = find_drum_track(midi)
    beat_track = find_track_named(midi, "BEAT")
    tempos = build_tempo_map(midi)
    first_drum_tick, last_drum_tick = first_and_last_drum_tick(drum_track)

    if beat_track is not None:
        beat_ticks = sorted({tick for tick, _pitch in beat_track.notes})
        beat_ticks = [tick for tick in beat_ticks if first_drum_tick <= tick <= last_drum_tick]
    else:
        beat_ticks = generate_tempo_grid_beats(
            first_drum_tick,
            last_drum_tick,
            midi.ticks_per_quarter,
        )

    chart_notes: list[ChartNote] = []
    lanes = [
        ("kick", CURRENT_CODE_LANES["kick"]),
        ("snare", CURRENT_CODE_LANES["snare"]),
    ]

    for note_number, beat_tick in enumerate(beat_ticks[::2]):
        label, lane = lanes[note_number % len(lanes)]
        chart_notes.append(
            ChartNote(
                tick_to_ms(beat_tick, midi.ticks_per_quarter, tempos),
                lane,
                label,
            )
        )

    return chart_notes


def convert_notes(midi: MidiFile, args: argparse.Namespace) -> list[ChartNote]:
    if args.difficulty == "super_easy":
        return build_super_easy_notes(midi, args)

    drum_track = find_drum_track(midi)
    tempos = build_tempo_map(midi)
    base_pitch = DIFFICULTY_BASE_PITCH[args.difficulty]

    pitch_to_label: dict[int, str] = {}
    for color, offset in DRUM_COLOR_OFFSETS.items():
        lane_label = destination_lane_for_color(color, args)
        if lane_label is not None and lane_label != "ignore":
            pitch_to_label[base_pitch + offset] = lane_label

    chart_notes: list[ChartNote] = []
    seen_notes: set[tuple[int, int]] = set()

    for tick, pitch in drum_track.notes:
        if pitch not in pitch_to_label:
            continue

        label = pitch_to_label[pitch]
        lane = CURRENT_CODE_LANES[label]
        time_ms = tick_to_ms(tick, midi.ticks_per_quarter, tempos)
        key = (time_ms, lane)

        # rock band files can carry marker notes; keep one note per lane and time.
        if key in seen_notes:
            continue

        seen_notes.add(key)
        chart_notes.append(ChartNote(time_ms, lane, label))

    chart_notes.sort(key=lambda note: (note.time_ms, note.lane))
    return chart_notes


def sanitize_identifier(value: str) -> str:
    cleaned = re.sub(r"[^a-zA-Z0-9_]+", "_", value).strip("_").lower()
    if not cleaned:
        return "chart"
    if cleaned[0].isdigit():
        return f"chart_{cleaned}"
    return cleaned


def default_track_id(midi_path: Path) -> str:
    if midi_path.name.lower() == "notes.mid" and midi_path.parent.name:
        return midi_path.parent.name
    return midi_path.stem


def default_output_path(midi_path: Path, track_id: str) -> Path:
    repo_root = Path.cwd()
    return repo_root / "src" / "generated_charts" / f"{sanitize_identifier(track_id)}_chart.h"


def display_path(path: Path) -> str:
    try:
        return path.relative_to(Path.cwd()).as_posix()
    except ValueError:
        return path.as_posix()


def build_header_text(
    midi_path: Path,
    track_id: str,
    notes: list[ChartNote],
    args: argparse.Namespace,
) -> str:
    identifier = sanitize_identifier(track_id)
    song_end_ms = notes[-1].time_ms if notes else 0
    source_path = display_path(midi_path)

    lines = [
        "#pragma once",
        "",
        "// generated by tools/midi_to_chart_header.py",
        f"// source midi: {source_path}",
        f"// difficulty: {args.difficulty}",
        "// lane order: 0=hihat, 1=kick, 2=snare",
        "// include this after a compatible chart note struct is available.",
        "",
        "#ifndef AIR_DRUMS_CHART_NOTE_STRUCT",
        "#define AIR_DRUMS_CHART_NOTE_STRUCT",
        "struct AirDrumsChartNote {",
        "  int time_ms;",
        "  int lane;",
        "};",
        "#endif",
        "",
        f"constexpr int {identifier}_note_count = {len(notes)};",
        f"constexpr int {identifier}_song_end_ms = {song_end_ms};",
        f"constexpr AirDrumsChartNote {identifier}_notes[{max(len(notes), 1)}] = {{",
    ]

    if notes:
        for note in notes:
            lines.append(f"  {{ {note.time_ms}, {note.lane} }}, // {note.label}")
    else:
        lines.append("  { 0, 0 },")

    lines.extend(
        [
            "};",
            "",
        ]
    )

    return "\n".join(lines)


def main() -> int:
    parser = argparse.ArgumentParser(
        description="Convert a Rock Band PART DRUMS midi track into an ESP32 chart header."
    )
    parser.add_argument("midi", type=Path, help="Path to notes.mid")
    parser.add_argument(
        "-o",
        "--output",
        type=Path,
        help="Output header path. Defaults to src/generated_charts/<track>_chart.h",
    )
    parser.add_argument(
        "--track-id",
        help="Chart id used for generated C++ names. Defaults to the chart folder name.",
    )
    parser.add_argument(
        "--difficulty",
        choices=sorted(DIFFICULTIES),
        default="easy",
        help="Rock Band drum difficulty to convert. Default: easy",
    )
    parser.add_argument(
        "--yellow-to",
        choices=("hihat", "kick", "snare", "ignore"),
        default="hihat",
        help="Where yellow drum notes should go. Default: hihat",
    )
    parser.add_argument(
        "--blue-to",
        choices=("hihat", "kick", "snare", "ignore"),
        default="ignore",
        help="Where blue drum notes should go. Default: ignore",
    )
    parser.add_argument(
        "--green-to",
        choices=("hihat", "kick", "snare", "ignore"),
        default="ignore",
        help="Where green drum notes should go. Default: ignore",
    )
    args = parser.parse_args()

    midi_path = args.midi.expanduser().resolve()
    track_id = args.track_id or default_track_id(midi_path)
    output_path = args.output
    if output_path is None:
        output_path = default_output_path(midi_path, track_id)
    output_path = output_path.expanduser().resolve()

    midi = read_midi(midi_path)
    notes = convert_notes(midi, args)
    header_text = build_header_text(midi_path, track_id, notes, args)

    output_path.parent.mkdir(parents=True, exist_ok=True)
    output_path.write_text(header_text, encoding="utf-8")

    print(f"wrote {output_path}")
    print(f"track_id={track_id}")
    print(f"difficulty={args.difficulty}")
    print(f"notes={len(notes)}")
    print(f"song_end_ms={notes[-1].time_ms if notes else 0}")

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
