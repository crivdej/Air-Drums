#!/usr/bin/env python3
"""PC-side driver for the Air-Drums NeoPixel diagnostic firmware."""

from __future__ import annotations

import argparse
import sys
import time
from datetime import datetime
from pathlib import Path

try:
    import serial
    from serial.tools import list_ports
except ImportError:
    platformio_python = Path.home() / ".platformio" / "penv" / "bin" / "python"
    if platformio_python.exists() and Path(sys.executable).resolve() != platformio_python.resolve():
        import subprocess

        raise SystemExit(subprocess.call([str(platformio_python), *sys.argv]))
    raise SystemExit("pyserial is not installed. Run: pip install pyserial")


DEFAULT_COMMANDS = [
    ("STATUS", 0.7, "Board should report pin=23, rings=3, total_leds=48."),
    ("WIRING", 4.0, "Ring 0 should be red, ring 1 green, ring 2 blue."),
    ("COLOR RED ALL", 2.0, "All rings should be red. Wrong color means color-order/data issue."),
    ("COLOR GREEN ALL", 2.0, "All rings should be green."),
    ("COLOR BLUE ALL", 2.0, "All rings should be blue."),
    ("WALK ALL 70", 4.5, "One pixel should walk through ring 0, then 1, then 2."),
    ("CHASE 2 60", 3.5, "A soft dot should chase smoothly on every selected ring."),
    ("POWER 45", 4.0, "All rings should be even white, without flicker/yellowing/reset."),
    ("CLEAR", 0.7, "All LEDs should turn off."),
]


def find_port() -> str:
    ports = list(list_ports.comports())
    for port in ports:
        name = port.device.lower()
        desc = (port.description or "").lower()
        hwid = (port.hwid or "").lower()
        if "usbmodem" in name or "usbserial" in name:
            return port.device
        if "usb" in desc or "usb" in hwid:
            return port.device
    available = ", ".join(port.device for port in ports) or "none"
    raise SystemExit(
        "Could not auto-detect an ESP32 serial port. "
        f"Available ports: {available}. Pass --port explicitly."
    )


def timestamp() -> str:
    return datetime.now().strftime("%H:%M:%S.%f")[:-3]


def print_and_log(log_file, line: str) -> None:
    print(line, flush=True)
    if log_file is not None:
        log_file.write(line + "\n")
        log_file.flush()


def drain_serial(ser, log_file, seconds: float) -> None:
    deadline = time.monotonic() + seconds
    while time.monotonic() < deadline:
        raw = ser.readline()
        if not raw:
            continue
        text = raw.decode("utf-8", errors="replace").rstrip()
        print_and_log(log_file, f"{timestamp()} BOARD {text}")


def send_command(ser, log_file, command: str) -> None:
    print_and_log(log_file, f"{timestamp()} HOST  {command}")
    ser.write((command + "\n").encode("utf-8"))
    ser.flush()


def run_suite(ser, log_file) -> None:
    print_and_log(log_file, "Starting NeoPixel diagnostic suite.")
    drain_serial(ser, log_file, 2.0)
    for command, wait_seconds, note in DEFAULT_COMMANDS:
        print_and_log(log_file, f"\nCHECK {command}: {note}")
        send_command(ser, log_file, command)
        drain_serial(ser, log_file, wait_seconds)


def interactive(ser, log_file) -> None:
    print_and_log(log_file, "Interactive mode. Type firmware commands, 'suite', or 'quit'.")
    drain_serial(ser, log_file, 1.0)
    while True:
        try:
            command = input("led> ").strip()
        except (EOFError, KeyboardInterrupt):
            print()
            return
        if not command:
            drain_serial(ser, log_file, 0.25)
            continue
        if command.lower() in {"quit", "exit"}:
            return
        if command.lower() == "suite":
            run_suite(ser, log_file)
            continue
        send_command(ser, log_file, command)
        drain_serial(ser, log_file, 0.8)


def main() -> int:
    parser = argparse.ArgumentParser(description="Run Air-Drums NeoPixel ring diagnostics over serial.")
    parser.add_argument("--port", help="Serial port, for example /dev/cu.usbserial-0001")
    parser.add_argument("--baud", type=int, default=115200, help="Serial baud rate. Default: 115200")
    parser.add_argument("--log", type=Path, help="Optional transcript file")
    parser.add_argument("--interactive", action="store_true", help="Stay open for manual commands after the suite")
    parser.add_argument("--command", action="append", help="Send one command. Can be repeated.")
    args = parser.parse_args()

    port = args.port or find_port()
    log_file = args.log.open("a", encoding="utf-8") if args.log else None

    try:
        print_and_log(log_file, f"Using serial port: {port}")
        with serial.Serial(port, args.baud, timeout=0.2) as ser:
            time.sleep(1.5)
            if args.command:
                drain_serial(ser, log_file, 1.0)
                for command in args.command:
                    send_command(ser, log_file, command)
                    drain_serial(ser, log_file, 1.0)
            else:
                run_suite(ser, log_file)
            if args.interactive:
                interactive(ser, log_file)
    finally:
        if log_file is not None:
            log_file.close()

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
