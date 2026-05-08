# Air-Drums

This is our CICS 256 final project. It is an ESP32 air drum setup with three ultrasonic sensors. Each sensor watches for a hand in front of it and acts like one drum pad: hi-hat, kick, or snare.

The ESP32 handles the sensors, short drum samples, NeoPixel rings, and OLED. The computer plays the full song audio because whole songs are too much for the board. The two stay lined up over USB serial.

The game is not tied to one song. Each song has a chart id, and the PC player looks for audio in `pc_tracks/` with the same name.

## Hardware

| lane | drum | trigger pin | echo pin |
|---:|---|---:|---:|
| 0 | hi-hat | 5 | 16 |
| 1 | kick | 4 | 13 |
| 2 | snare | 19 | 14 |

Other pins:

| part | pin |
|---|---:|
| ESP32 DAC audio out | 26 |
| NeoPixel ring data | 23 |
| OLED | default I2C `SDA` and `SCL` |

The HC-SR04 echo pins output 5 V. ESP32 pins are 3.3 V, so each echo line needs a voltage divider.

## What Is In This Repo

| path | what it is for |
|---|---|
| `src/Air_Drums.ino` | main sketch, pins, sensors, samples, mode switches |
| `src/rhythm_game_draft_commented.cpp` | rhythm game, scoring, chart list, OLED, LED rings |
| `src/generated_charts/` | chart headers that get compiled into the ESP32 code |
| `charts/` | one folder per song, usually with the original RB3CON package |
| `pc_tracks/` | local song audio for the Python player, ignored by git except `.gitkeep` |
| `airdrums.py` | checks for new charts, loads them if needed, then starts the PC player |
| `tools/chartloader.py` | imports RB3CON files and makes charts plus WAV files |
| `tools/extract_rb3con.py` | low-level RB3CON extractor used by `tools/chartloader.py` |
| `tools/midi_to_chart_header.py` | converts `notes.mid` into an ESP32 chart header |
| `tools/serial_music_player.py` | PC audio player and game dashboard |
| `tools/neopixel_ring_diagnostics.py` | LED ring test helper |

## Main Modes

At the top of `src/Air_Drums.ino`:

```cpp
#define NEOPIXEL_TEST_MODE 0
#define RHYTHM_GAME_MODE 1
```

- `RHYTHM_GAME_MODE 1` runs the rhythm game.
- `NEOPIXEL_TEST_MODE 1` runs only the LED ring test.
- If both are `0`, the board runs a simple free-play mode with the sensors and basic PC audio messages.

## Setup

Install PlatformIO for the ESP32 build.

The Python scripts use:

```bash
pip install pyserial pygame
```

For RB3CON importing, also install `ffmpeg` and `ffprobe`. On macOS with Homebrew:

```bash
brew install ffmpeg
```

Some Rock Band `.mogg` files are encrypted. If `tools/chartloader.py` says it cannot find playable audio, install the Onyx CLI and make sure `onyx` is on your PATH, or pass it with `--onyx /path/to/onyx`.

## Uploading To The ESP32

Plug in the board and run:

```bash
pio run --target upload
```

To watch serial output directly:

```bash
pio device monitor --baud 115200
```

## Playing The Game

Upload the ESP32 code first. Then start the PC player:

```bash
python3 airdrums.py
```

This is the easiest command to use most of the time. It checks the `charts/` folders first. If a song still needs to be extracted, converted, or turned into a WAV, it runs `tools/chartloader.py`. If there is nothing new to load, or if loading finishes successfully, it starts `tools/serial_music_player.py`.

You can pass normal PC player options after it:

```bash
python3 airdrums.py --port /dev/cu.usbserial-XXXX
python3 airdrums.py --no-board --track-id tusk
python3 airdrums.py --log-status
```

To only load charts and not start the player:

```bash
python3 airdrums.py --no-player
```

To skip the chart check and go straight to the player:

```bash
python3 airdrums.py --skip-chartloader
```

You can also run the PC player directly:

```bash
python3 tools/serial_music_player.py
```

The script tries to find the ESP32 serial port. If it picks the wrong one, pass the port:

```bash
python3 tools/serial_music_player.py --port /dev/cu.usbserial-XXXX
```

The player shows a small dashboard with the current chart, score, combo, timing sync, and recent hits or misses. Score is now a chart-normalized percent: each note can earn up to 100 timing-quality points, and the song score is the percent of possible chart quality earned. The dashboard also reports grade, hit rate, max combo, extra hits, average timing error, and perfect/great/good/bad counts so the end of a song explains what went well and what needs work.

For plain scrolling logs instead:

```bash
python3 tools/serial_music_player.py --log-status
```

## PC Player Commands

Type these into the Python player:

| command | what it does |
|---|---|
| `start` or `s` | starts the countdown and song |
| `stop` or `x` | stops the game |
| `next` or `n` | next chart |
| `prev` or `b` | previous chart |
| `charts` | lists the charts on the board |
| `chart <id>` | selects a chart by id |
| `chart <number>` | selects a chart by number from the list |
| `reload` or `p` | asks the board to resend its state |
| `quit` | stops playback and exits |

When a chart is selected, the board sends `BOARD LOAD <track_id>`. The Python script then looks in `pc_tracks/` for audio with that same id. When the game starts, the board sends `BOARD START <track_id> <delay_ms>`, so the computer starts the song after the same countdown.

## Testing Without The Board

This checks the audio files and PC player without plugging in the ESP32:

```bash
python3 tools/serial_music_player.py --no-board --track-id tusk
```

The same commands work here: `start`, `stop`, `next`, `prev`, `charts`, `chart <id|number>`, `reload`, and `quit`.

## Adding Your Own RB3CON Song

Use a folder name that can also be a C++ identifier. Lowercase letters with `_` between words is easiest.

Example for a song called `my_song`:

```bash
mkdir -p charts/my_song
```

Put the original RB3CON file in that folder:

```text
charts/my_song/my_song.rb3con
```

Then run the chart loader:

```bash
python3 tools/chartloader.py
```

The loader scans every folder in `charts/`. For a new song, it will:

- extract `notes.mid`, `metadata.dta`, album art, venue data, and song audio from the RB3CON
- generate `src/generated_charts/my_song_chart.h`
- create `pc_tracks/my_song.wav`
- check that the chart has notes and the WAV is stereo 44.1 kHz 16-bit audio

The extracted files under `charts/` are ignored by git because they can be recreated from the RB3CON. The original RB3CON file is not ignored, so it can be committed if you want the source package in the repo. The generated chart header should be committed if the song is meant to be built into the firmware. The generated WAV in `pc_tracks/` is local and ignored because it can be large.

If you want a different drum difficulty:

```bash
python3 tools/chartloader.py --difficulty medium
```

Choices are `easy`, `medium`, `hard`, `expert`, and `super_easy`. The default is `easy` because air hits are harder than pressing Rock Band buttons.

To rebuild everything even if files already exist:

```bash
python3 tools/chartloader.py --force
```

If Onyx is not on PATH:

```bash
python3 tools/chartloader.py --onyx /path/to/onyx
```

## Adding The New Chart To The Firmware

`tools/chartloader.py` makes the chart header and refreshes the firmware chart registry automatically.
PlatformIO also refreshes the registry before every build, so any `src/generated_charts/*_chart.h`
file is included in the ESP32 chart list by default.

Upload the firmware again:

```bash
pio run --target upload
```

Start the PC player, then select the song:

```text
chart my_song
start
```

## Adding A Chart From `notes.mid` Only

If you already have a `notes.mid` and do not want to use an RB3CON, run the lower-level converter:

```bash
python3 tools/midi_to_chart_header.py charts/my_song/notes.mid --track-id my_song
```

That writes:

```text
src/generated_charts/my_song_chart.h
```

Then refresh the generated firmware registry:

```bash
python3 tools/generate_chart_registry.py
```

You also need to provide your own audio file:

```text
pc_tracks/my_song.wav
```

The PC player also looks for `.mp3`, `.m4a`, `.aiff`, and `.ogg`, but `tools/chartloader.py` creates `.wav`.

## Chart Converter Notes

Lane order in the firmware:

| lane | drum |
|---:|---|
| 0 | hi-hat |
| 1 | kick |
| 2 | snare |

Default Rock Band mapping:

| Rock Band note | Air-Drums lane |
|---|---|
| kick | kick |
| red/snare | snare |
| yellow | hi-hat |
| blue | ignored |
| green | ignored |

You can change where the colored notes go when using `midi_to_chart_header.py` directly:

```bash
python3 tools/midi_to_chart_header.py charts/my_song/notes.mid \
  --track-id my_song \
  --difficulty medium \
  --yellow-to hihat \
  --blue-to snare \
  --green-to kick
```

There is also a `super_easy` mode. It uses the MIDI beat track when it can, takes every other beat, and alternates kick and snare:

```bash
python3 tools/midi_to_chart_header.py charts/my_song/notes.mid \
  --difficulty super_easy \
  --track-id my_song_super_easy
```

## Charts Currently In The Firmware

The current rhythm game list includes:

- `takefive`
- `boysdontcry`
- `cruelangelsthesis`
- `expertinadyingfield`
- `tusk`
- `steves_lava_chicken`
- `steves_lava_chicken_super_easy`

To play one of these, the board needs the chart compiled in and the PC needs matching audio in `pc_tracks/`.

## Serial Messages

The board sends:

- `BOARD HELLO`
- `BOARD LOAD <track_id>`
- `BOARD START <track_id> <delay_ms>`
- `BOARD STOP`
- `GAME ...` lines with score, combo, timing, hits, misses, and chart state

The PC sends:

- `HOST HELLO`
- `HOST REQUEST_STATE`
- `HOST START_GAME`
- `HOST STOP_GAME`
- `HOST NEXT_CHART`
- `HOST PREV_CHART`
- `HOST LIST_CHARTS`
- `HOST SELECT_CHART <id|number>`
- `HOST READY <track_id>`
- `HOST ERROR <message>`

## NeoPixel Ring Test

To test only the LED rings, set this in `src/Air_Drums.ino`:

```cpp
#define NEOPIXEL_TEST_MODE 1
```

Then upload and run the helper:

```bash
pio run --target upload
python3 tools/neopixel_ring_diagnostics.py --interactive --log neopixel_diag.log
```

Useful commands in the LED test:

| command | what it does |
|---|---|
| `STATUS` | prints pin, ring count, brightness, and pixel ranges |
| `WIRING` | sets ring 0 red, ring 1 green, and ring 2 blue |
| `COLOR RED ALL` | checks one color across every ring |
| `WALK ALL 70` | lights one pixel at a time through the chain |
| `POWER 45` | lights all rings white at low brightness |
| `CLEAR` | turns all LEDs off |

If only the first ring lights up, check the data wire between the rings. If the colors are wrong, the LED color order probably needs changing. If white flickers or turns yellow near the end, check power and ground first.
