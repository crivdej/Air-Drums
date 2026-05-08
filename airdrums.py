#!/usr/bin/env python3

import argparse
import subprocess
import sys
from pathlib import Path


REPO_ROOT = Path(__file__).resolve().parent
CHARTS_DIR = REPO_ROOT / "charts"
GENERATED_CHARTS_DIR = REPO_ROOT / "src" / "generated_charts"
PC_TRACKS_DIR = REPO_ROOT / "pc_tracks"
CHARTLOADER = REPO_ROOT / "tools" / "chartloader.py"
SERIAL_PLAYER = REPO_ROOT / "tools" / "serial_music_player.py"
CHART_REGISTRY_GENERATOR = REPO_ROOT / "tools" / "generate_chart_registry.py"
STFS_MAGIC = (b"CON ", b"LIVE", b"PIRS")


def is_rb3con_package(path: Path) -> bool:
    if not path.is_file():
        return False

    try:
        return path.read_bytes()[:4] in STFS_MAGIC
    except OSError:
        return False


def song_has_package(song_dir: Path) -> bool:
    return any(is_rb3con_package(path) for path in song_dir.iterdir() if path.is_file())


def chart_needs_loading(song_dir: Path) -> bool:
    track_id = song_dir.name
    notes_path = song_dir / "notes.mid"
    chart_path = GENERATED_CHARTS_DIR / f"{track_id}_chart.h"
    wav_path = PC_TRACKS_DIR / f"{track_id}.wav"

    if song_has_package(song_dir) and not notes_path.exists():
        return True

    if notes_path.exists() and (not chart_path.exists() or not wav_path.exists()):
        return True

    return False


def charts_need_loading() -> bool:
    if not CHARTS_DIR.exists():
        return False

    for song_dir in sorted(CHARTS_DIR.iterdir()):
        if song_dir.is_dir() and chart_needs_loading(song_dir):
            return True

    return False


def run(command: list[str]) -> int:
    print("$ " + " ".join(command), flush=True)
    return subprocess.call(command, cwd=REPO_ROOT)


def main() -> int:
    parser = argparse.ArgumentParser(
        description="Load any new Air-Drums charts, then start the PC serial player."
    )
    parser.add_argument(
        "--skip-chartloader",
        action="store_true",
        help="Do not check or load charts before starting the player.",
    )
    parser.add_argument(
        "--force-chartloader",
        action="store_true",
        help="Run chartloader.py even if the wrapper does not see new chart work.",
    )
    parser.add_argument(
        "--no-player",
        action="store_true",
        help="Load charts if needed, then stop instead of starting the serial player.",
    )
    parser.add_argument(
        "--difficulty",
        choices=("easy", "medium", "hard", "expert", "super_easy"),
        default="easy",
        help="Difficulty to pass to chartloader.py when it runs. Default: easy.",
    )
    parser.add_argument(
        "--onyx",
        help="Path or command name for the Onyx CLI, passed to chartloader.py.",
    )
    parser.add_argument(
        "--force",
        action="store_true",
        help="Regenerate chartloader outputs. This also implies --force-chartloader.",
    )
    args, player_args = parser.parse_known_args()

    should_run_loader = (
        not args.skip_chartloader
        and (args.force or args.force_chartloader or charts_need_loading())
    )

    if should_run_loader:
        if not CHARTLOADER.exists():
            print(f"Missing chart loader: {CHARTLOADER}", file=sys.stderr)
            return 1

        command = [
            sys.executable,
            str(CHARTLOADER),
            "--difficulty",
            args.difficulty,
        ]
        if args.force:
            command.append("--force")
        if args.onyx:
            command.extend(["--onyx", args.onyx])

        loader_status = run(command)
        if loader_status != 0:
            return loader_status
    else:
        print("No new charts need loading.", flush=True)
        if CHART_REGISTRY_GENERATOR.exists():
            registry_status = run([sys.executable, str(CHART_REGISTRY_GENERATOR)])
            if registry_status != 0:
                return registry_status

    if args.no_player:
        return 0

    if not SERIAL_PLAYER.exists():
        print(f"Missing serial player: {SERIAL_PLAYER}", file=sys.stderr)
        return 1

    return run([sys.executable, str(SERIAL_PLAYER), *player_args])


if __name__ == "__main__":
    raise SystemExit(main())
