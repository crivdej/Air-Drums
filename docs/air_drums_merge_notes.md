# air-drums merge notes

these notes are based on the read-only repo at:

`/Users/elroydesbordes/Documents/GitHub/Air-Drums`

## what their repo has

their main file is `src/Air_Drums.ino`.

it already has:

- three ultrasonic sensors
- simple trigger-on-entry logic
- kick, snare, and hi-hat lane names
- esp32 dac audio playback on pin `26`
- drum sample headers

## pins from their repo

use these in the game code:

| lane | drum | trig | echo |
|---|---|---:|---:|
| 0 | hi-hat | 5 | 16 |
| 1 | kick | 4 | 13 |
| 2 | snare | 19 | 14 |

important: kick echo uses pin `13`, so the neopixel ring data pin cannot use `13`.

this NeoPixel diagnostic branch uses pin `23` for the chained ring data line.

## how this should connect

keep her files intact.

the game should call into her code instead of copying it.

her repo should keep owning:

- sensor pins
- `DrumSensor`
- `sensors[]`
- `NUM_SENSORS`
- `TRIGGER_CM`
- `DEBOUNCE_MS`
- `getDistance(...)`
- `playDrum(...)`
- sample data

this diagnostic branch uses pin `23` for the chained led rings because kick echo uses pin `13`.

## how to merge later

do not run both loops. her `loop()` is a complete simple air-drum app, and the rhythm game loop is also a complete app loop.

keep from their repo:

- the sensor pin choices
- the voltage divider wiring idea
- the dac audio setup if we want local drum sounds
- `playDrum`
- the sample headers

add the game logic as a separate file, then make the main sketch call the game loop instead of the simple trigger loop.

the game draft should keep using the active `sensors[]` order from `src/Air_Drums.ino`:

- lane 0: hi-hat
- lane 1: kick
- lane 2: snare

the game draft now calls her code like this:

- `read_one_sensor_cm(lane)` calls `getDistance(sensors[lane].trig, sensors[lane].echo)`
- `update_sensors()` uses `sensors[lane].inZone` and `sensors[lane].lastTriggerMs`
- `is_hand_inside(...)` uses `TRIGGER_CM`
- `update_sensors()` uses `DEBOUNCE_MS`
- `play_local_drum_for_lane(lane)` calls `playDrum(sensors[lane].sample, sensors[lane].sampleLen)`

this keeps her files as the hardware/audio layer and your file as the rhythm game layer.

the important game functions are:

- `setup_game_logic()`
- `loop_game_logic()`
- `update_sensors()`
- `handle_hit()`
- `check_for_misses()`
- `render_game()`
- `start_countdown()`

## audio choice

the original rhythm plan uses the computer for audio.

the air-drums repo uses the esp32 dac for drum sounds.

the game draft has a small `play_local_drum_for_lane()` hook. right now it is written to call her existing `playDrum(...)` helper through `sensors[lane]`.

## chart conversion

rock band drum `notes.mid` files can be converted with:

```sh
python3 tools/midi_to_chart_header.py charts/steves_lava_chicken/notes.mid
```

the generated header goes to `src/generated_charts/<track_id>_chart.h` by default.

default chart settings:

- difficulty: easy
- reason: air drums are harder to time than physical buttons

default chart mapping:

- rock band kick -> lane 1 kick
- rock band red/snare -> lane 2 snare
- rock band yellow -> lane 0 hi-hat
- rock band blue and green -> ignored for now

super easy chart mode:

- command example:

```sh
python3 tools/midi_to_chart_header.py charts/steves_lava_chicken/notes.mid --difficulty super_easy --track-id steves_lava_chicken_super_easy
```

- uses the midi `BEAT` track when it exists
- falls back to a tempo-grid beat track if needed
- starts at the first drum note and stops at the last drum note
- takes every other beat
- alternates lane 1 kick and lane 2 snare

this only adds chart data. the active firmware still does not include or run the full rhythm game module.
