# Air-Drums
CICS 256 final project — a contactless air drum machine. Three HC-SR04 ultrasonic sensors detect hand gestures within a 15cm trigger zone, each mapped to a drum sound (kick, snare, hi-hat). Audio playback uses the ESP32's built-in DAC driven by a 16kHz hardware timer ISR with drum samples. Signal conditioning via voltage dividers ensures 5V sensor output is safely stepped down to 3.3V for the ESP32 GPIO pins. Built for Spring 2026

## NeoPixel Ring Diagnostics

The `NEOpixels` branch currently boots straight into NeoPixel diagnostic mode. The chained three-ring data line is on ESP32 pin `23`, with three 16-LED rings for `48` total pixels.

Flash and run the guided PC diagnostic:

```bash
~/.platformio/penv/bin/pio run --target upload
python3 tools/neopixel_ring_diagnostics.py --interactive --log neopixel_diag.log
```

Useful manual commands in the diagnostic prompt:

- `STATUS` prints pin, ring count, brightness, selected ring, and physical pixel ranges.
- `WIRING` sets ring 0 red, ring 1 green, and ring 2 blue.
- `COLOR RED ALL`, `COLOR GREEN ALL`, and `COLOR BLUE ALL` isolate color-order/data problems.
- `WALK ALL 70` lights exactly one pixel at a time through ring 0, then 1, then 2.
- `POWER 45` lights all rings white at a capped brightness so you can watch for flicker, yellowing, resets, or end-of-chain voltage drop.
- `CLEAR` turns everything off.

If only the first ring responds, check data-out to data-in between ring 0 and ring 1. If colors are swapped, the firmware pixel order is wrong for the LEDs. If white flickers or turns yellow near the end of the chain, power/ground is the suspect before code.

## PC Backing Track Player

For the first test pass, the board now owns:

- hit detection
- local drum samples

The computer now owns:

- full-quality song playback

### Serial protocol

Board to PC:

- `BOARD HELLO`
- `BOARD LOAD <track_id>`
- `BOARD START <track_id> <delay_ms>`
- `BOARD STOP`

PC to board:

- `HOST HELLO`
- `HOST REQUEST_STATE`
- `HOST START_GAME`
- `HOST STOP_GAME`

### How it works

1. Flash the board and connect it over USB.
2. Keep the playable backing track at `pc_tracks/steves_lava_chicken.wav`.
3. Install the one Python dependency: `pip install pyserial` or `pip3 install pyserial`
4. Run `python3 tools/serial_music_player.py --port /dev/cu.usbserial-XXXX`
5. Type `start` in the player window.

On boot, the board sends `BOARD HELLO` and `BOARD LOAD steves_lava_chicken`. When you start the test, the board sends `BOARD START steves_lava_chicken 3000`, which means "start this song 3000 ms from now." The ESP32 keeps doing local drum hits, and the Python player schedules `afplay` after that delay so the song comes from the computer.

Chart/source files live in `charts/steves_lava_chicken/`. The playable computer audio lives in `pc_tracks/`.

### Testing Without The Board

To test only the Python playback side without an ESP32:

```bash
python3 tools/serial_music_player.py --no-board
```

Then type `start`, `stop`, `reload`, or `quit`. In this mode the helper loads `pc_tracks/steves_lava_chicken.wav` and simulates the board's 3000 ms start delay.

While no-board mode is running, the terminal shows whether playback is idle, scheduled, playing, or finished. During playback it also shows elapsed time and a progress bar.

## Rock Band Chart Converter

Rock Band drum charts can be converted from `notes.mid` into a small ESP32 header:

```sh
python3 tools/midi_to_chart_header.py charts/steves_lava_chicken/notes.mid
```

By default, the converter writes `src/generated_charts/<track_id>_chart.h` and uses the current firmware lane order:

| lane | drum |
|---:|---|
| 0 | hi-hat |
| 1 | kick |
| 2 | snare |

The default mapping converts easy drums because air hits are harder to time than button presses. It sends yellow notes to hi-hat and ignores blue/green notes for the first 3-sensor version. Other Rock Band drum MIDI files can use the same tool by pointing it at another `notes.mid`.

For a slower practice chart, use `super_easy`. It follows the MIDI `BEAT` track, places a note on every other beat, and alternates kick/snare:

```sh
python3 tools/midi_to_chart_header.py charts/steves_lava_chicken/notes.mid --difficulty super_easy --track-id steves_lava_chicken_super_easy
```
