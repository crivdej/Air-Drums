#!/usr/bin/env python3

import argparse
import select
import subprocess
import sys
import threading
import time
import wave
from pathlib import Path

try:
    import serial
    from serial.tools import list_ports
except ModuleNotFoundError:
    serial = None
    list_ports = None


def resolve_port(requested_port: str | None) -> str:
    if requested_port:
        return requested_port

    if list_ports is None:
        raise RuntimeError("pyserial is not installed. Run: pip install pyserial")

    for port in list_ports.comports():
        name = (port.device or "").lower()
        description = (port.description or "").lower()
        if "usb" in description or "serial" in description or "usb" in name or "cu." in name:
            return port.device

    raise RuntimeError("Could not auto-detect a serial port. Pass --port explicitly.")


def find_track_file(tracks_dir: Path, track_id: str) -> Path | None:
    # pc playback files live in pc_tracks/ and are named after the board track id
    for extension in (".wav", ".mp3", ".m4a", ".aiff", ".ogg"):
        candidate = tracks_dir / f"{track_id}{extension}"
        if candidate.exists():
            return candidate

    return None


class PlayerState:
    def __init__(self, serial_port, tracks_dir: Path) -> None:
        self.serial_port = serial_port
        self.tracks_dir = tracks_dir
        self.current_track_id = ""
        self.current_track_path: Path | None = None
        self.audio_process: subprocess.Popen | None = None
        self.start_timer: threading.Timer | None = None
        self.scheduled_start_time = 0.0
        self.playback_start_time = 0.0
        self.current_duration_seconds = 0.0
        self.lock = threading.Lock()

    def send_line(self, line: str) -> None:
        # every protocol message is a single newline-terminated line
        if self.serial_port is None:
            print(f"[mock host] {line}", flush=True)
            return

        self.serial_port.write((line + "\n").encode("utf-8"))
        self.serial_port.flush()

    def stop_audio(self) -> None:
        with self.lock:
            if self.start_timer is not None:
                self.start_timer.cancel()
                self.start_timer = None

            if self.audio_process is not None and self.audio_process.poll() is None:
                self.audio_process.terminate()
                try:
                    self.audio_process.wait(timeout=1.0)
                except subprocess.TimeoutExpired:
                    self.audio_process.kill()
            self.audio_process = None
            self.scheduled_start_time = 0.0
            self.playback_start_time = 0.0

    def launch_audio(self) -> None:
        with self.lock:
            self.start_timer = None
            if self.current_track_path is None:
                return

            self.playback_start_time = time.monotonic()
            self.scheduled_start_time = 0.0

            # afplay keeps this first test tiny because it is built into macos
            self.audio_process = subprocess.Popen(
                ["afplay", str(self.current_track_path)],
                stdout=subprocess.DEVNULL,
                stderr=subprocess.DEVNULL,
            )

    def handle_load(self, track_id: str) -> None:
        # when the board announces a track id, find the matching file on the computer
        track_path = find_track_file(self.tracks_dir, track_id)
        self.stop_audio()

        if track_path is None:
            self.current_track_id = ""
            self.current_track_path = None
            self.send_line(f"HOST ERROR missing_track {track_id}")
            print(f"Missing track: {track_id}", flush=True)
            return

        self.current_track_id = track_id
        self.current_track_path = track_path
        self.current_duration_seconds = audio_duration_seconds(track_path)
        self.send_line(f"HOST READY {track_id}")
        print(f"Loaded track {track_id}: {track_path.name}", flush=True)

    def handle_start(self, track_id: str, delay_ms: int) -> None:
        # the board sends a future delay so both sides can agree on when music should begin
        if self.current_track_path is None or self.current_track_id != track_id:
            self.handle_load(track_id)

        if self.current_track_path is None:
            return

        self.stop_audio()
        delay_seconds = max(delay_ms, 0) / 1000.0

        with self.lock:
            self.scheduled_start_time = time.monotonic() + delay_seconds
            self.playback_start_time = 0.0
            self.start_timer = threading.Timer(delay_seconds, self.launch_audio)
            self.start_timer.start()

        print(f"Starting {track_id} in {delay_seconds:.3f}s", flush=True)


def audio_duration_seconds(path: Path) -> float:
    if path.suffix.lower() == ".wav":
        try:
            with wave.open(str(path), "rb") as audio_file:
                frame_count = audio_file.getnframes()
                frame_rate = audio_file.getframerate()
                if frame_rate > 0:
                    return frame_count / float(frame_rate)
        except wave.Error:
            return 0.0

    return 0.0


def format_time(seconds: float) -> str:
    seconds = max(0.0, seconds)
    minutes = int(seconds // 60)
    whole_seconds = int(seconds % 60)
    return f"{minutes}:{whole_seconds:02d}"


def progress_line(state: PlayerState) -> str:
    now = time.monotonic()

    with state.lock:
        scheduled_start_time = state.scheduled_start_time
        playback_start_time = state.playback_start_time
        duration = state.current_duration_seconds
        audio_process = state.audio_process
        track_name = state.current_track_path.name if state.current_track_path else "(none)"

    if scheduled_start_time > 0:
        remaining = max(0.0, scheduled_start_time - now)
        return f"scheduled: {track_name} starts in {remaining:0.1f}s"

    if audio_process is None:
        return f"idle: {track_name}"

    if audio_process.poll() is not None:
        return f"finished: {track_name}"

    elapsed = max(0.0, now - playback_start_time)
    if duration <= 0:
        return f"playing: {track_name} {format_time(elapsed)}"

    progress = min(1.0, elapsed / duration)
    filled = int(progress * 24)
    bar = "#" * filled + "-" * (24 - filled)
    return f"playing: [{bar}] {format_time(elapsed)} / {format_time(duration)}"


def run_no_board_mode(state: PlayerState, track_id: str, delay_ms: int) -> int:
    # this mode lets us test the pc playback side without an esp32 connected
    state.handle_load(track_id)
    print("No-board mode: commands are start, stop, reload, quit", flush=True)
    print("> ", end="", flush=True)

    try:
        while True:
            readable, _, _ = select.select([sys.stdin], [], [], 0.25)

            if not readable:
                print(f"\r{progress_line(state):<80}", end="", flush=True)
                continue

            raw_command = sys.stdin.readline()
            if raw_command == "":
                command = "quit"
            else:
                command = raw_command.strip().lower()

            print("\r" + " " * 80 + "\r", end="", flush=True)

            if command in ("", "start"):
                state.handle_start(track_id, delay_ms)
            elif command == "stop":
                state.stop_audio()
                print("Stopped playback", flush=True)
            elif command == "reload":
                state.handle_load(track_id)
            elif command == "quit":
                state.stop_audio()
                return 0
            else:
                print("Unknown command. Use: start, stop, reload, quit", flush=True)

            print("> ", end="", flush=True)
    finally:
        state.stop_audio()


def read_serial_loop(state: PlayerState) -> None:
    while True:
        try:
            raw_line = state.serial_port.readline()
        except Exception as exc:
            print(f"Serial error: {exc}", flush=True)
            return

        if not raw_line:
            continue

        try:
            line = raw_line.decode("utf-8", errors="ignore").strip()
        except UnicodeDecodeError:
            continue

        if not line:
            continue

        if not line.startswith("BOARD "):
            # keep normal board debug output visible without treating it as protocol
            print(f"[board] {line}", flush=True)
            continue

        print(f"[protocol] {line}", flush=True)

        if line == "BOARD HELLO":
            continue

        if line.startswith("BOARD LOAD "):
            state.handle_load(line[len("BOARD LOAD "):].strip())
            continue

        if line.startswith("BOARD START "):
            # board start lines carry both the track id and the requested delay
            payload = line[len("BOARD START "):].strip().split()
            if len(payload) != 2:
                print(f"Ignoring malformed start line: {line}", flush=True)
                continue

            track_id, delay_text = payload
            try:
                delay_ms = int(delay_text)
            except ValueError:
                print(f"Ignoring malformed delay: {line}", flush=True)
                continue

            state.handle_start(track_id, delay_ms)
            continue

        if line == "BOARD STOP":
            state.stop_audio()
            print("Stopped playback", flush=True)


def main() -> int:
    parser = argparse.ArgumentParser(
        description="Play PC backing tracks for the Air-Drums board over serial."
    )
    parser.add_argument("--port", help="Serial port, for example /dev/cu.usbserial-0001")
    parser.add_argument(
        "--baud",
        type=int,
        default=115200,
        help="Serial baud rate. Default: 115200",
    )
    parser.add_argument(
        "--tracks-dir",
        default="pc_tracks",
        help="Folder containing playable audio files. Default: ./pc_tracks",
    )
    parser.add_argument(
        "--no-board",
        action="store_true",
        help="Run without serial hardware and control playback from this terminal.",
    )
    parser.add_argument(
        "--track-id",
        default="steves_lava_chicken",
        help="Track id to load in --no-board mode. Default: steves_lava_chicken",
    )
    parser.add_argument(
        "--delay-ms",
        type=int,
        default=3000,
        help="Start delay for --no-board mode. Default: 3000",
    )
    args = parser.parse_args()

    tracks_dir = Path(args.tracks_dir).expanduser().resolve()
    tracks_dir.mkdir(parents=True, exist_ok=True)

    if args.no_board:
        print(f"Using tracks folder: {tracks_dir}", flush=True)
        state = PlayerState(None, tracks_dir)
        return run_no_board_mode(state, args.track_id, args.delay_ms)

    if serial is None:
        print("pyserial is not installed. Run: pip install pyserial", file=sys.stderr)
        return 1

    try:
        port = resolve_port(args.port)
    except RuntimeError as exc:
        print(str(exc), file=sys.stderr)
        return 1

    print(f"Using serial port: {port}", flush=True)
    print(f"Using tracks folder: {tracks_dir}", flush=True)
    print("Commands: start, stop, reload, quit", flush=True)

    try:
        serial_port = serial.Serial(port, args.baud, timeout=0.25)
    except Exception as exc:
        print(f"Could not open serial port: {exc}", file=sys.stderr)
        return 1

    state = PlayerState(serial_port, tracks_dir)
    # ask the board to identify itself and resend the current track selection
    state.send_line("HOST HELLO")
    state.send_line("HOST REQUEST_STATE")

    reader = threading.Thread(target=read_serial_loop, args=(state,), daemon=True)
    reader.start()

    try:
        while True:
            try:
                command = input("> ").strip().lower()
            except EOFError:
                command = "quit"

            if command in ("", "start"):
                # keep manual testing simple by letting start mean "ask the board to fire the song cue"
                state.send_line("HOST START_GAME")
            elif command == "stop":
                state.send_line("HOST STOP_GAME")
            elif command == "reload":
                state.send_line("HOST REQUEST_STATE")
            elif command == "quit":
                state.send_line("HOST STOP_GAME")
                state.stop_audio()
                return 0
            else:
                print("Unknown command. Use: start, stop, reload, quit", flush=True)
    finally:
        state.stop_audio()
        serial_port.close()


if __name__ == "__main__":
    raise SystemExit(main())
