#!/usr/bin/env python3

import argparse
import os
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
    platformio_python = Path.home() / ".platformio" / "penv" / "bin" / "python"
    if platformio_python.exists() and Path(sys.executable).resolve() != platformio_python.resolve():
        # use the platformio python because it already has pyserial installed
        os.execv(str(platformio_python), [str(platformio_python), *sys.argv])

    serial = None
    list_ports = None


def resolve_port(requested_port: str | None) -> str:
    if requested_port:
        return requested_port

    if list_ports is None:
        raise RuntimeError("pyserial is not installed. Run: pip install pyserial")

    ports = list(list_ports.comports())
    preferred_ports = []

    for port in list_ports.comports():
        name = (port.device or "").lower()
        description = (port.description or "").lower()
        hardware_id = (port.hwid or "").lower()

        if "bluetooth" in name or "bluetooth" in description:
            continue
        if "debug-console" in name or "wlan-debug" in name:
            continue

        score = 0
        if "usbmodem" in name or "usbserial" in name:
            score += 10
        if "usb" in description or "usb" in hardware_id:
            score += 5
        if "cp210" in description or "ch340" in description or "wch" in description:
            score += 5
        if "serial" in description and "bluetooth" not in description:
            score += 1

        if score > 0:
            preferred_ports.append((score, port.device))

    if preferred_ports:
        preferred_ports.sort(reverse=True)
        return preferred_ports[0][1]

    available = ", ".join(port.device for port in ports) or "none"
    raise RuntimeError(
        "Could not auto-detect an ESP32 USB serial port. "
        f"Available ports: {available}. "
        "Reconnect the board, try a data-capable USB cable, or pass --port explicitly."
    )


def find_track_file(tracks_dir: Path, track_id: str) -> Path | None:
    # pc playback files live in pc_tracks/ and are named after the board track id
    for extension in (".wav", ".mp3", ".m4a", ".aiff", ".ogg"):
        candidate = tracks_dir / f"{track_id}{extension}"
        if candidate.exists():
            return candidate

    return None


def parse_key_values(line: str) -> dict[str, str]:
    fields: dict[str, str] = {}

    for token in line.split()[2:]:
        if "=" not in token:
            continue

        key, value = token.split("=", 1)
        fields[key] = value

    return fields


def parse_int(value: str, fallback: int) -> int:
    try:
        return int(value)
    except ValueError:
        return fallback


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
        self.board_mode = "unknown"
        self.board_song_ms = 0
        self.board_countdown_ms = 0
        self.score = 0
        self.combo = 0
        self.next_lane = "none"
        self.next_in_ms = -1
        self.last_game_event = "waiting for board"
        self.lock = threading.Lock()
        self.print_lock = threading.Lock()

    def print_line(self, text: str) -> None:
        with self.print_lock:
            print("\r" + " " * 160 + "\r" + text, flush=True)

    def send_line(self, line: str) -> None:
        # every protocol message is a single newline-terminated line
        if self.serial_port is None:
            self.print_line(f"[mock host] {line}")
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
            self.print_line(f"Missing track: {track_id}")
            return

        self.current_track_id = track_id
        self.current_track_path = track_path
        self.current_duration_seconds = audio_duration_seconds(track_path)
        self.send_line(f"HOST READY {track_id}")
        self.print_line(f"Loaded track {track_id}: {track_path.name}")

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

        self.print_line(f"Starting {track_id} in {delay_seconds:.3f}s")

    def handle_game_line(self, line: str) -> None:
        parts = line.split(maxsplit=2)
        event = parts[1] if len(parts) > 1 else "UNKNOWN"
        fields = parse_key_values(line)

        with self.lock:
            if "mode" in fields:
                self.board_mode = fields["mode"]
            if "song_ms" in fields:
                self.board_song_ms = parse_int(fields["song_ms"], self.board_song_ms)
            if "countdown_ms" in fields:
                self.board_countdown_ms = parse_int(fields["countdown_ms"], self.board_countdown_ms)
            if "score" in fields:
                self.score = parse_int(fields["score"], self.score)
            if "combo" in fields:
                self.combo = parse_int(fields["combo"], self.combo)
            if "next_name" in fields:
                self.next_lane = fields["next_name"]
            if "next_in_ms" in fields:
                self.next_in_ms = parse_int(fields["next_in_ms"], self.next_in_ms)

            if event == "READY":
                self.board_mode = "ready"
                self.last_game_event = f"ready notes={fields.get('notes', '?')}"
            elif event == "COUNTDOWN":
                self.board_mode = "countdown"
                self.last_game_event = f"countdown {fields.get('delay_ms', '?')}ms"
            elif event == "START":
                self.board_mode = "playing"
                self.board_song_ms = 0
                self.last_game_event = "song started"
            elif event == "PAD":
                name = fields.get("name", fields.get("lane", "?"))
                mode = fields.get("mode", "?")
                self.last_game_event = f"pad {name} in {mode}"
            elif event == "HIT":
                name = fields.get("name", fields.get("lane", "?"))
                result = fields.get("result", "?")
                delta = fields.get("delta_ms", "?")
                self.last_game_event = f"hit {name} {result} delta={delta}ms"
            elif event == "MISS":
                name = fields.get("name", fields.get("lane", "?"))
                note_ms = fields.get("note_ms", "?")
                self.last_game_event = f"miss {name} note={note_ms}ms"
            elif event == "FINISH":
                self.board_mode = "finished"
                self.last_game_event = "song finished"
            elif event == "STOP":
                self.board_mode = "ready"
                self.last_game_event = "stopped"

        if event in ("READY", "COUNTDOWN", "START", "PAD", "HIT", "MISS", "FINISH", "STOP"):
            self.print_line(f"[game] {self.last_game_event}")


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


def pc_elapsed_ms(state: PlayerState) -> int:
    with state.lock:
        playback_start_time = state.playback_start_time
        audio_process = state.audio_process

    if audio_process is None or audio_process.poll() is not None or playback_start_time <= 0:
        return 0

    return int((time.monotonic() - playback_start_time) * 1000)


def serial_status_line(state: PlayerState) -> str:
    pc_ms = pc_elapsed_ms(state)

    with state.lock:
        board_mode = state.board_mode
        board_song_ms = state.board_song_ms
        board_countdown_ms = state.board_countdown_ms
        score = state.score
        combo = state.combo
        next_lane = state.next_lane
        next_in_ms = state.next_in_ms
        last_game_event = state.last_game_event

    sync_text = "sync=n/a"
    if pc_ms > 0 and board_song_ms > 0:
        sync_text = f"sync={pc_ms - board_song_ms:+d}ms"

    next_text = "next=none"
    if next_in_ms >= 0:
        next_text = f"next={next_lane} in {next_in_ms}ms"

    if board_countdown_ms > 0:
        next_text = f"countdown={board_countdown_ms}ms"

    return (
        f"{progress_line(state)} | board={board_mode} {board_song_ms}ms | "
        f"{sync_text} | score={score} combo={combo} | {next_text} | {last_game_event}"
    )


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
            state.print_line(f"Serial error: {exc}")
            return

        if not raw_line:
            continue

        try:
            line = raw_line.decode("utf-8", errors="ignore").strip()
        except UnicodeDecodeError:
            continue

        if not line:
            continue

        if line.startswith("GAME "):
            state.handle_game_line(line)
            continue

        if not line.startswith("BOARD "):
            # keep normal board debug output visible without treating it as protocol
            state.print_line(f"[board] {line}")
            continue

        state.print_line(f"[protocol] {line}")

        if line == "BOARD HELLO":
            continue

        if line.startswith("BOARD LOAD "):
            state.handle_load(line[len("BOARD LOAD "):].strip())
            continue

        if line.startswith("BOARD START "):
            # board start lines carry both the track id and the requested delay
            payload = line[len("BOARD START "):].strip().split()
            if len(payload) != 2:
                state.print_line(f"Ignoring malformed start line: {line}")
                continue

            track_id, delay_text = payload
            try:
                delay_ms = int(delay_text)
            except ValueError:
                state.print_line(f"Ignoring malformed delay: {line}")
                continue

            state.handle_start(track_id, delay_ms)
            continue

        if line == "BOARD STOP":
            state.stop_audio()
            state.print_line("Stopped playback")


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
            readable, _, _ = select.select([sys.stdin], [], [], 0.25)

            if not readable:
                with state.print_lock:
                    print(f"\r{serial_status_line(state):<160}", end="", flush=True)
                continue

            raw_command = sys.stdin.readline()
            if raw_command == "":
                command = "quit"
            else:
                command = raw_command.strip().lower()

            with state.print_lock:
                print("\r" + " " * 160 + "\r", end="", flush=True)

            if command in ("", "start", "s"):
                # keep manual testing simple by letting start mean "ask the board to fire the song cue"
                state.send_line("HOST START_GAME")
            elif command in ("stop", "x"):
                state.send_line("HOST STOP_GAME")
            elif command in ("reload", "p"):
                state.send_line("HOST REQUEST_STATE")
            elif command == "quit":
                state.send_line("HOST STOP_GAME")
                state.stop_audio()
                print()
                return 0
            else:
                state.print_line("Unknown command. Use: start, stop, reload, quit")
    finally:
        state.stop_audio()
        serial_port.close()


if __name__ == "__main__":
    raise SystemExit(main())
