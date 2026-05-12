#!/usr/bin/env python3

import argparse
import json
import os
import shutil
import subprocess
import sys
import tempfile
import zipfile
from dataclasses import dataclass
from types import SimpleNamespace
from pathlib import Path


REPO_ROOT = Path(__file__).resolve().parents[1]
TOOLS_DIR = REPO_ROOT / "tools"

sys.path.insert(0, str(TOOLS_DIR))

from extract_rb3con import extract_package  # noqa: E402
from generate_chart_registry import generate_chart_registry  # noqa: E402
from midi_to_chart_header import (  # noqa: E402
    ChartNote,
    build_header_text,
    read_midi,
    sanitize_identifier,
)


STFS_MAGIC = (b"CON ", b"LIVE", b"PIRS")
DEFAULT_DIFFICULTY = "easy"
WAV_LOUDNESS_FILTER = "loudnorm=I=-14:TP=-1.5:LRA=11"
ZIP_CHART_FILENAMES = ("notes.mid", "notes.chart")
ZIP_AUDIO_EXTENSIONS = (".ogg", ".wav", ".mp3", ".flac", ".opus", ".aiff", ".aif")
ZIP_PREFERRED_AUDIO_BASENAMES = (
    "song",
    "backing",
    "background",
    "guitar",
    "rhythm",
    "bass",
    "drums",
    "keys",
    "vocals",
)
CHART_DRUM_NOTE_TO_LABEL = {
    0: "kick",
    1: "snare",
    2: "hihat",
}


@dataclass
class SongResult:
    track_id: str
    extracted: bool
    chart_path: Path
    wav_path: Path
    note_count: int
    duration_seconds: float


def display_path(path: Path) -> str:
    try:
        return path.resolve().relative_to(REPO_ROOT).as_posix()
    except ValueError:
        return path.as_posix()


def is_stfs_package(path: Path) -> bool:
    if not path.is_file():
        return False

    try:
        return path.read_bytes()[:4] in STFS_MAGIC
    except OSError:
        return False


def find_package(song_dir: Path) -> Path | None:
    packages = sorted(
        path for path in song_dir.iterdir() if path.is_file() and is_stfs_package(path)
    )
    if not packages:
        return None
    return packages[0]


def find_zip_package(song_dir: Path) -> Path | None:
    packages = sorted(path for path in song_dir.glob("*.zip") if path.is_file())
    if not packages:
        return None
    return packages[0]


def run_command(command: list[str]) -> subprocess.CompletedProcess[str]:
    try:
        return subprocess.run(
            command,
            cwd=REPO_ROOT,
            check=True,
            text=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
        )
    except subprocess.CalledProcessError as error:
        message = error.stderr.strip() or error.stdout.strip() or str(error)
        raise RuntimeError(message) from error


def ffprobe_stream(path: Path) -> dict:
    completed = run_command(
        [
            "ffprobe",
            "-v",
            "error",
            "-show_streams",
            "-show_format",
            "-of",
            "json",
            str(path),
        ]
    )
    data = json.loads(completed.stdout)
    streams = [stream for stream in data.get("streams", []) if stream.get("codec_type") == "audio"]
    if not streams:
        raise RuntimeError(f"{display_path(path)} does not contain an audio stream")
    return {"stream": streams[0], "format": data.get("format", {})}


def stereo_downmix_pan_filter(channel_count: int) -> str:
    if channel_count < 1:
        raise RuntimeError("audio stream reports zero channels")

    if channel_count == 1:
        return "pan=stereo|c0=c0|c1=c0"

    left_channels = "+".join(f"c{index}" for index in range(0, channel_count, 2))
    right_channels = "+".join(f"c{index}" for index in range(1, channel_count, 2))
    return f"pan=stereo|c0<{left_channels}|c1<{right_channels}"


def resolve_onyx_cli(onyx_arg: str | None) -> str | None:
    requested = onyx_arg or os.environ.get("ONYX_CLI")
    if requested:
        requested_path = Path(requested).expanduser()
        if requested_path.exists():
            return str(requested_path.resolve())

        found = shutil.which(requested)
        if found is not None:
            return found

        raise RuntimeError(f"could not find Onyx CLI: {requested}")

    return shutil.which("onyx")


def extract_if_needed(
    song_dir: Path,
    package_path: Path | None,
    pc_tracks_dir: Path,
    force: bool,
) -> bool:
    notes_path = song_dir / "notes.mid"
    metadata_path = song_dir / "metadata.dta"
    audio_path = song_dir / "song.mogg"

    already_extracted = notes_path.exists() and metadata_path.exists()
    if already_extracted and audio_path.exists() and not force:
        return False

    if package_path is None:
        if already_extracted:
            return False
        raise RuntimeError(f"{display_path(song_dir)} has no RB3CON/STFS package to extract")

    extract_package(
        package_path=package_path,
        output_dir=song_dir,
        write_ogg_copy=True,
        audio_output_dir=pc_tracks_dir,
        preserve_paths=False,
    )
    return True


def safe_zip_members(zip_path: Path) -> list[zipfile.ZipInfo]:
    with zipfile.ZipFile(zip_path) as archive:
        return [
            member
            for member in archive.infolist()
            if not member.is_dir() and Path(member.filename).name
        ]


def find_zip_member_by_basename(
    zip_path: Path,
    basenames: tuple[str, ...],
) -> zipfile.ZipInfo | None:
    wanted = {name.lower() for name in basenames}
    members = safe_zip_members(zip_path)

    for member in members:
        if Path(member.filename).name.lower() in wanted:
            return member

    return None


def extract_zip_member(zip_path: Path, member: zipfile.ZipInfo, destination: Path) -> None:
    destination.parent.mkdir(parents=True, exist_ok=True)
    with zipfile.ZipFile(zip_path) as archive:
        with archive.open(member) as source, destination.open("wb") as target:
            shutil.copyfileobj(source, target)


def extract_zip_chart_if_needed(song_dir: Path, zip_path: Path, force: bool) -> Path:
    existing_mid = song_dir / "notes.mid"
    existing_chart = song_dir / "notes.chart"

    if existing_mid.exists() and not force:
        return existing_mid
    if existing_chart.exists() and not force:
        return existing_chart

    member = find_zip_member_by_basename(zip_path, ZIP_CHART_FILENAMES)
    if member is None:
        raise RuntimeError(f"{display_path(zip_path)} does not contain notes.mid or notes.chart")

    destination = song_dir / Path(member.filename).name
    extract_zip_member(zip_path, member, destination)
    print(f"extracted chart from ZIP: {display_path(destination)}")
    return destination


def audio_member_sort_key(member: zipfile.ZipInfo) -> tuple[int, int, str]:
    path = Path(member.filename)
    stem = path.stem.lower()
    suffix = path.suffix.lower()

    try:
        preferred_index = ZIP_PREFERRED_AUDIO_BASENAMES.index(stem)
    except ValueError:
        preferred_index = len(ZIP_PREFERRED_AUDIO_BASENAMES)

    wav_penalty = 0 if suffix in (".ogg", ".wav") else 1
    return (preferred_index, wav_penalty, member.filename.lower())


def find_zip_audio_member(zip_path: Path) -> zipfile.ZipInfo:
    members = [
        member
        for member in safe_zip_members(zip_path)
        if Path(member.filename).suffix.lower() in ZIP_AUDIO_EXTENSIONS
    ]
    if not members:
        raise RuntimeError(f"{display_path(zip_path)} does not contain a playable audio file")

    return sorted(members, key=audio_member_sort_key)[0]


def extract_zip_audio_to_temp(zip_path: Path, temp_dir: Path) -> Path:
    member = find_zip_audio_member(zip_path)
    source_name = Path(member.filename).name
    destination = temp_dir / source_name
    extract_zip_member(zip_path, member, destination)
    print(f"extracted audio from ZIP: {source_name}")
    return destination


def strip_ogg_from_mogg(mogg_path: Path, ogg_path: Path) -> bool:
    data = mogg_path.read_bytes()
    ogg_offset = data.find(b"OggS")
    if ogg_offset < 0:
        return False

    ogg_path.parent.mkdir(parents=True, exist_ok=True)
    ogg_path.write_bytes(data[ogg_offset:])
    return True


def unwrap_mogg_with_onyx(onyx_cli: str, mogg_path: Path, ogg_path: Path) -> None:
    ogg_path.parent.mkdir(parents=True, exist_ok=True)
    run_command([onyx_cli, "unwrap", str(mogg_path), "--to", str(ogg_path)])


def find_audio_source(
    track_id: str,
    song_dir: Path,
    pc_tracks_dir: Path,
    onyx_cli: str | None,
) -> Path:
    ogg_candidates = [
        pc_tracks_dir / f"{track_id}.ogg",
        song_dir / "song.ogg",
    ]

    for candidate in ogg_candidates:
        if candidate.exists():
            return candidate

    generated_ogg_path = pc_tracks_dir / f"{track_id}.ogg"
    mogg_candidates = [
        pc_tracks_dir / f"{track_id}.mogg",
        song_dir / "song.mogg",
    ]

    for candidate in mogg_candidates:
        if candidate.exists() and strip_ogg_from_mogg(candidate, generated_ogg_path):
            print(f"stripped OGG stream: {display_path(generated_ogg_path)}")
            return generated_ogg_path

    if onyx_cli is not None:
        for candidate in mogg_candidates:
            if candidate.exists():
                unwrap_mogg_with_onyx(onyx_cli, candidate, generated_ogg_path)
                print(f"unwrapped MOGG with Onyx: {display_path(generated_ogg_path)}")
                return generated_ogg_path

    raise RuntimeError(
        "could not find playable audio. If this song extracted only an encrypted MOGG, install Onyx "
        "so `onyx` is on PATH, pass `--onyx /path/to/onyx`, or add a decrypted OGG at "
        f"{display_path(generated_ogg_path)} and rerun chartloader.py."
    )


def make_wav(track_id: str, audio_source: Path, wav_path: Path, force: bool) -> None:
    if wav_path.exists() and not force:
        return

    if shutil.which("ffmpeg") is None or shutil.which("ffprobe") is None:
        raise RuntimeError("ffmpeg and ffprobe are required to create and verify WAV files")

    source_info = ffprobe_stream(audio_source)
    channels = int(source_info["stream"].get("channels", 0))
    pan_filter = stereo_downmix_pan_filter(channels)
    audio_filter = f"{pan_filter},{WAV_LOUDNESS_FILTER}"

    wav_path.parent.mkdir(parents=True, exist_ok=True)
    run_command(
        [
            "ffmpeg",
            "-hide_banner",
            "-loglevel",
            "error",
            "-y",
            "-i",
            str(audio_source),
            "-filter_complex",
            audio_filter,
            "-ar",
            "44100",
            "-c:a",
            "pcm_s16le",
            str(wav_path),
        ]
    )


def parse_chart_sections(chart_path: Path) -> dict[str, list[str]]:
    sections: dict[str, list[str]] = {}
    current_section: str | None = None
    in_section_body = False

    for raw_line in chart_path.read_text(encoding="utf-8-sig").splitlines():
        line = raw_line.strip()
        if not line:
            continue

        if line.startswith("[") and line.endswith("]"):
            current_section = line[1:-1]
            sections.setdefault(current_section, [])
            in_section_body = False
            continue

        if current_section is None:
            continue
        if line == "{":
            in_section_body = True
            continue
        if line == "}":
            current_section = None
            in_section_body = False
            continue

        if in_section_body:
            sections[current_section].append(line)

    return sections


def chart_setting(sections: dict[str, list[str]], key: str, default: str | None = None) -> str | None:
    for line in sections.get("Song", []):
        if "=" not in line:
            continue

        left, right = line.split("=", 1)
        if left.strip().lower() == key.lower():
            return right.strip().strip('"')

    return default


def chart_tempos(sections: dict[str, list[str]]) -> list[tuple[int, int]]:
    tempos: list[tuple[int, int]] = []

    for line in sections.get("SyncTrack", []):
        parts = line.split()
        if len(parts) >= 4 and parts[1] == "=" and parts[2] == "B":
            tempos.append((int(parts[0]), int(parts[3])))

    if not tempos:
        return [(0, 120000)]

    tempos.sort()
    if tempos[0][0] != 0:
        tempos.insert(0, (0, 120000))

    return tempos


def chart_tick_to_ms(tick: int, resolution: int, tempos: list[tuple[int, int]]) -> int:
    total_ms = 0.0
    last_tick = 0
    current_bpm_x1000 = 120000

    for tempo_tick, bpm_x1000 in tempos:
        if tempo_tick > tick:
            break

        total_ms += ((tempo_tick - last_tick) * 60000000.0) / (
            current_bpm_x1000 * resolution
        )
        last_tick = tempo_tick
        current_bpm_x1000 = bpm_x1000

    total_ms += ((tick - last_tick) * 60000000.0) / (current_bpm_x1000 * resolution)
    return int(round(total_ms))


def chart_drums_section_name(difficulty: str) -> str:
    if difficulty == "super_easy":
        return "EasyDrums"
    return f"{difficulty.capitalize()}Drums"


def convert_chart_notes(chart_path: Path, difficulty: str) -> list[ChartNote]:
    sections = parse_chart_sections(chart_path)
    section_name = chart_drums_section_name(difficulty)
    section_lines = sections.get(section_name)
    if section_lines is None:
        raise RuntimeError(f"{display_path(chart_path)} does not contain [{section_name}]")

    resolution = int(chart_setting(sections, "Resolution", "192") or "192")
    tempos = chart_tempos(sections)
    notes: list[ChartNote] = []
    seen_notes: set[tuple[int, int]] = set()

    for line in section_lines:
        parts = line.split()
        if len(parts) < 5 or parts[1] != "=" or parts[2] != "N":
            continue

        tick = int(parts[0])
        note_number = int(parts[3])
        label = CHART_DRUM_NOTE_TO_LABEL.get(note_number)
        if label is None:
            continue

        lane = {"hihat": 0, "kick": 1, "snare": 2}[label]
        time_ms = chart_tick_to_ms(tick, resolution, tempos)
        key = (time_ms, lane)
        if key in seen_notes:
            continue

        seen_notes.add(key)
        notes.append(ChartNote(time_ms, lane, label))

    notes.sort(key=lambda note: (note.time_ms, note.lane))
    return notes


def make_chart_from_chart_file(
    chart_source_path: Path,
    track_id: str,
    chart_path: Path,
    difficulty: str,
) -> None:
    notes = convert_chart_notes(chart_source_path, difficulty)
    header_text = build_header_text(
        midi_path=chart_source_path,
        track_id=track_id,
        notes=notes,
        args=SimpleNamespace(difficulty=difficulty),
        source_label="source chart",
    )
    chart_path.parent.mkdir(parents=True, exist_ok=True)
    chart_path.write_text(header_text, encoding="utf-8")


def make_chart(
    song_dir: Path,
    track_id: str,
    chart_source_path: Path,
    chart_path: Path,
    difficulty: str,
    force: bool,
) -> None:
    if chart_path.exists() and not force:
        return

    if not chart_source_path.exists():
        raise RuntimeError(f"missing chart source: {display_path(chart_source_path)}")

    if chart_source_path.suffix.lower() == ".chart":
        make_chart_from_chart_file(chart_source_path, track_id, chart_path, difficulty)
        return

    chart_path.parent.mkdir(parents=True, exist_ok=True)
    run_command(
        [
            sys.executable,
            str(TOOLS_DIR / "midi_to_chart_header.py"),
            str(chart_source_path),
            "--track-id",
            track_id,
            "--difficulty",
            difficulty,
            "-o",
            str(chart_path),
        ]
    )


def read_note_count(chart_path: Path, track_id: str) -> int:
    text = chart_path.read_text(encoding="utf-8")
    identifier = sanitize_identifier(track_id)
    marker = f"constexpr int {identifier}_note_count = "
    for line in text.splitlines():
        if line.startswith(marker):
            return int(line.removeprefix(marker).rstrip(";"))

    raise RuntimeError(f"could not verify note count in {display_path(chart_path)}")


def verify_chart_source(chart_source_path: Path, difficulty: str) -> None:
    if chart_source_path.suffix.lower() == ".chart":
        convert_chart_notes(chart_source_path, difficulty)
        return

    read_midi(chart_source_path)


def verify_outputs(
    track_id: str,
    chart_source_path: Path,
    chart_path: Path,
    wav_path: Path,
    difficulty: str,
) -> tuple[int, float]:
    if not chart_source_path.exists():
        raise RuntimeError(f"missing chart source: {display_path(chart_source_path)}")

    verify_chart_source(chart_source_path, difficulty)

    if not chart_path.exists() or chart_path.stat().st_size == 0:
        raise RuntimeError(f"missing generated chart: {display_path(chart_path)}")

    note_count = read_note_count(chart_path, track_id)
    if note_count <= 0:
        raise RuntimeError(f"generated chart has no notes: {display_path(chart_path)}")

    if not wav_path.exists() or wav_path.stat().st_size == 0:
        raise RuntimeError(f"missing generated WAV: {display_path(wav_path)}")

    wav_info = ffprobe_stream(wav_path)
    stream = wav_info["stream"]
    sample_rate = int(stream.get("sample_rate", 0))
    channels = int(stream.get("channels", 0))
    codec_name = stream.get("codec_name")
    bits_per_sample = int(stream.get("bits_per_sample") or 0)
    duration = float(wav_info["format"].get("duration") or stream.get("duration") or 0)

    if codec_name != "pcm_s16le":
        raise RuntimeError(f"{display_path(wav_path)} is {codec_name}, expected pcm_s16le")
    if sample_rate != 44100:
        raise RuntimeError(f"{display_path(wav_path)} is {sample_rate} Hz, expected 44100 Hz")
    if channels != 2:
        raise RuntimeError(f"{display_path(wav_path)} has {channels} channels, expected stereo")
    if bits_per_sample != 16:
        raise RuntimeError(f"{display_path(wav_path)} is {bits_per_sample}-bit, expected 16-bit")
    if duration <= 0:
        raise RuntimeError(f"{display_path(wav_path)} has no measurable duration")

    return note_count, duration


def should_process_song(
    song_dir: Path,
    package_path: Path | None,
    zip_path: Path | None,
    chart_path: Path,
    wav_path: Path,
    force: bool,
) -> bool:
    if force:
        return True
    if package_path is not None and not (song_dir / "notes.mid").exists():
        return True
    if zip_path is not None and not (
        (song_dir / "notes.mid").exists() or (song_dir / "notes.chart").exists()
    ):
        return True
    return (
        ((song_dir / "notes.mid").exists() or (song_dir / "notes.chart").exists())
        and (not chart_path.exists() or not wav_path.exists())
    )


def load_song(
    song_dir: Path,
    generated_charts_dir: Path,
    pc_tracks_dir: Path,
    onyx_cli: str | None,
    difficulty: str,
    force: bool,
) -> SongResult | None:
    track_id = song_dir.name
    package_path = find_package(song_dir)
    zip_path = find_zip_package(song_dir)
    chart_path = generated_charts_dir / f"{track_id}_chart.h"
    wav_path = pc_tracks_dir / f"{track_id}.wav"

    if not should_process_song(song_dir, package_path, zip_path, chart_path, wav_path, force):
        return None

    print(f"\n== {track_id} ==")

    extracted = False
    if package_path is not None:
        extracted = extract_if_needed(song_dir, package_path, pc_tracks_dir, force)
        if extracted:
            print(f"extracted RB3CON into {display_path(song_dir)}")
        chart_source_path = song_dir / "notes.mid"
    elif zip_path is not None:
        chart_source_path = extract_zip_chart_if_needed(song_dir, zip_path, force)
        extracted = True
    else:
        chart_source_path = (
            song_dir / "notes.mid"
            if (song_dir / "notes.mid").exists()
            else song_dir / "notes.chart"
        )

    make_chart(song_dir, track_id, chart_source_path, chart_path, difficulty, force)
    print(f"chart ready: {display_path(chart_path)}")

    if zip_path is not None and (force or not wav_path.exists()):
        with tempfile.TemporaryDirectory(prefix=f"{track_id}_", dir=pc_tracks_dir) as temp_dir_name:
            audio_source = extract_zip_audio_to_temp(zip_path, Path(temp_dir_name))
            make_wav(track_id, audio_source, wav_path, force)
    else:
        audio_source = find_audio_source(track_id, song_dir, pc_tracks_dir, onyx_cli)
        make_wav(track_id, audio_source, wav_path, force)
    print(f"wav ready: {display_path(wav_path)}")

    note_count, duration = verify_outputs(
        track_id,
        chart_source_path,
        chart_path,
        wav_path,
        difficulty,
    )
    print(f"verified: {note_count} notes, {duration:.2f}s WAV")

    return SongResult(
        track_id=track_id,
        extracted=extracted,
        chart_path=chart_path,
        wav_path=wav_path,
        note_count=note_count,
        duration_seconds=duration,
    )


def main() -> int:
    parser = argparse.ArgumentParser(
        description=(
            "Scan charts/<song>/ folders for new RB3CON packages, extract them, "
            "generate easy drum chart headers, and create stereo WAV files in pc_tracks/."
        )
    )
    parser.add_argument(
        "--charts-dir",
        type=Path,
        default=REPO_ROOT / "charts",
        help="Folder containing one subfolder per song. Default: charts/",
    )
    parser.add_argument(
        "--generated-charts-dir",
        type=Path,
        default=REPO_ROOT / "src" / "generated_charts",
        help="Output folder for generated chart headers. Default: src/generated_charts/",
    )
    parser.add_argument(
        "--pc-tracks-dir",
        type=Path,
        default=REPO_ROOT / "pc_tracks",
        help="Output folder for playable PC audio. Default: pc_tracks/",
    )
    parser.add_argument(
        "--difficulty",
        default=DEFAULT_DIFFICULTY,
        choices=("easy", "medium", "hard", "expert", "super_easy"),
        help="Drum difficulty to generate. Default: easy.",
    )
    parser.add_argument(
        "--onyx",
        help=(
            "Path or command name for the Onyx CLI. Defaults to `onyx` on PATH, "
            "or the ONYX_CLI environment variable."
        ),
    )
    parser.add_argument(
        "--force",
        action="store_true",
        help="Regenerate extracted files, chart headers, and WAVs even when outputs already exist.",
    )
    args = parser.parse_args()

    charts_dir = args.charts_dir.expanduser().resolve()
    generated_charts_dir = args.generated_charts_dir.expanduser().resolve()
    pc_tracks_dir = args.pc_tracks_dir.expanduser().resolve()
    onyx_cli = resolve_onyx_cli(args.onyx)

    if not charts_dir.exists():
        raise SystemExit(f"charts folder does not exist: {display_path(charts_dir)}")

    song_dirs = sorted(path for path in charts_dir.iterdir() if path.is_dir())
    results: list[SongResult] = []
    failures: list[tuple[str, str]] = []

    for song_dir in song_dirs:
        try:
            result = load_song(
                song_dir=song_dir,
                generated_charts_dir=generated_charts_dir,
                pc_tracks_dir=pc_tracks_dir,
                onyx_cli=onyx_cli,
                difficulty=args.difficulty,
                force=args.force,
            )
            if result is not None:
                results.append(result)
        except Exception as error:
            failures.append((song_dir.name, str(error)))
            print(f"\n!! {song_dir.name}: {error}", file=sys.stderr)

    if not results and not failures:
        print("No new chart folders needed loading.")

    if not failures:
        generate_chart_registry()

    if results:
        print("\nLoaded songs:")
        for result in results:
            print(
                f"- {result.track_id}: {display_path(result.chart_path)}, "
                f"{display_path(result.wav_path)}, {result.note_count} notes"
            )

    if failures:
        print("\nFailures:", file=sys.stderr)
        for track_id, message in failures:
            print(f"- {track_id}: {message}", file=sys.stderr)
        return 1

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
