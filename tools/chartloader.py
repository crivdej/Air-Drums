#!/usr/bin/env python3

import argparse
import json
import os
import shutil
import subprocess
import sys
from dataclasses import dataclass
from pathlib import Path


REPO_ROOT = Path(__file__).resolve().parents[1]
TOOLS_DIR = REPO_ROOT / "tools"

sys.path.insert(0, str(TOOLS_DIR))

from extract_rb3con import extract_package  # noqa: E402
from generate_chart_registry import generate_chart_registry  # noqa: E402
from midi_to_chart_header import read_midi, sanitize_identifier  # noqa: E402


STFS_MAGIC = (b"CON ", b"LIVE", b"PIRS")
DEFAULT_DIFFICULTY = "easy"


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


def last_stereo_pair_pan_filter(channel_count: int) -> str:
    if channel_count < 1:
        raise RuntimeError("audio stream reports zero channels")

    if channel_count == 1:
        return "pan=stereo|c0=c0|c1=c0"

    left_channel = channel_count - 2
    right_channel = channel_count - 1
    return f"pan=stereo|c0=c{left_channel}|c1=c{right_channel}"


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
    pan_filter = last_stereo_pair_pan_filter(channels)

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
            pan_filter,
            "-ar",
            "44100",
            "-c:a",
            "pcm_s16le",
            str(wav_path),
        ]
    )


def make_chart(song_dir: Path, track_id: str, chart_path: Path, difficulty: str, force: bool) -> None:
    if chart_path.exists() and not force:
        return

    notes_path = song_dir / "notes.mid"
    if not notes_path.exists():
        raise RuntimeError(f"missing MIDI file: {display_path(notes_path)}")

    chart_path.parent.mkdir(parents=True, exist_ok=True)
    run_command(
        [
            sys.executable,
            str(TOOLS_DIR / "midi_to_chart_header.py"),
            str(notes_path),
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


def verify_outputs(track_id: str, song_dir: Path, chart_path: Path, wav_path: Path) -> tuple[int, float]:
    notes_path = song_dir / "notes.mid"
    if not notes_path.exists():
        raise RuntimeError(f"missing extracted MIDI: {display_path(notes_path)}")

    read_midi(notes_path)

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
    chart_path: Path,
    wav_path: Path,
    force: bool,
) -> bool:
    if force:
        return True
    if package_path is not None and not (song_dir / "notes.mid").exists():
        return True
    return (song_dir / "notes.mid").exists() and (not chart_path.exists() or not wav_path.exists())


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
    chart_path = generated_charts_dir / f"{track_id}_chart.h"
    wav_path = pc_tracks_dir / f"{track_id}.wav"

    if not should_process_song(song_dir, package_path, chart_path, wav_path, force):
        return None

    print(f"\n== {track_id} ==")

    extracted = extract_if_needed(song_dir, package_path, pc_tracks_dir, force)
    if extracted:
        print(f"extracted RB3CON into {display_path(song_dir)}")

    make_chart(song_dir, track_id, chart_path, difficulty, force)
    print(f"chart ready: {display_path(chart_path)}")

    audio_source = find_audio_source(track_id, song_dir, pc_tracks_dir, onyx_cli)
    make_wav(track_id, audio_source, wav_path, force)
    print(f"wav ready: {display_path(wav_path)}")

    note_count, duration = verify_outputs(track_id, song_dir, chart_path, wav_path)
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
