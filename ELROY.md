# elroy local notes

this file is for local codex/project handoff notes only.
it is listed in `.gitignore` so it should not be committed or shown to other people in the repo.

## project shape

- project name: air drum tiles / air drum machine
- class: cics 256 final project
- final idea: 3-lane physical rhythm game with ultrasonic drum pads and led rings
- lanes:
  - lane 0: kick
  - lane 1: snare
  - lane 2: hi-hat
- hardware:
  - 3 hc-sr04 ultrasonic sensors
  - 3 chained 16-led neopixel rings
  - esp32 makerboard
- intended split:
  - makerboard handles sensors, led feedback, scoring, and responsive local timing
  - computer may later handle music playback, chart loading, and sync

## user style preferences

- build in chunks, not all at once
- keep the code simple and readable
- make it feel like a naive college student could have written it
- comments should be lowercase
- avoid fancy abstractions unless really needed
- ask before big uncertain steps

## current repo state

- main repo path: `/Users/elroydesbordes/Documents/GitHub/Air-Drums`
- original milestone project path mentioned in handoff: `/Users/elroydesbordes/Documents/New project 2`
- active air-drums sketch: `src/Air_Drums.ino`
- dormant rhythm game draft: `src/rhythm_game_draft_commented.cpp`
- the rhythm game draft is intentionally fully commented out
- do not run two `setup()` / `loop()` sketches at the same time
- current git status had only `.DS_Store` modified before these notes

## air-drums hardware layer

`src/Air_Drums.ino` already owns:

- sensor pin definitions
- `DrumSensor`
- `sensors[]`
- `NUM_SENSORS`
- `TRIGGER_CM`
- `DEBOUNCE_MS`
- `getDistance(...)`
- `playDrum(...)`
- dac audio on pin `26`
- drum samples from `kick.h`, `snare.h`, and `hihat.h`

the rhythm game should call into that code later instead of copying it.

## sensor pins

| lane | drum | trig | echo |
|---|---|---:|---:|
| 0 | kick | 5 | 16 |
| 1 | snare | 4 | 13 |
| 2 | hi-hat | 19 | 14 |

important: pin `13` is already snare echo, so do not use pin `13` for external neopixel rings.

## led pin decision

- external chained led rings should use gpio `15`
- `src/rhythm_game_draft_commented.cpp` already has `game_led_data_pin = 15`
- the old milestone project at `/Users/elroydesbordes/Documents/New project 2` also has `led_data_pin = 15`
- the class reference says pin `25` is the onboard neopixel pin
- pin `25` is good for testing the built-in board neopixel, but should not be the first choice for the external chained 16-led rings
- using pin `25` for external rings could mix up onboard neopixel behavior with the ring chain

## current active firmware mode

- `src/Air_Drums.ino` now has `NEOPIXEL_TEST_MODE` set to `1`
- while this is `1`, the board runs the simple led ring test instead of the sensor/audio drum loop
- the current led test is only one 16-led ring
- the current led test uses gpio `23`
- serial commands in led test mode:
  - `a` animates the ring
  - `0` animates ring 0
  - `+` speeds up the animation
  - `-` slows down the animation
  - `h` shows hit pixels and quarter marks
  - `p` prints settings
  - `x` clears leds
- set `NEOPIXEL_TEST_MODE` back to `0` later to return to the sensor/audio air-drum behavior

## platformio note

- `platformio.ini` now uses `src_dir = src`
- this makes platformio build `src/Air_Drums.ino`
- it also avoids the root `drumsensor_test.ino`, which is only a commented reference sketch

## docs / handoff mismatch

- the handoff said `docs/milestone_1_led_test.md` existed in this repo
- this repo currently only showed:
  - `docs/air_drums_merge_notes.md`
  - `docs/CICS256_Technical_Reference.md`
- the milestone docs do exist in `/Users/elroydesbordes/Documents/New project 2`

## build note

- `pio run` could not be verified in this shell because `pio` was not found
- the handoff says the air-drums repo may fail platformio build because `platformio.ini` uses `src_dir = .`, causing `drumsensor_test.ino` to be included even though it is fully commented out
- do not silently fix that without asking because it changes repo setup

## best next chunk

1. if working in Air-Drums, decide whether to fix platformio build setup first
2. test one external neopixel ring on gpio `15`
3. confirm ring chain order and physical pixel orientation
4. test all three ultrasonic sensors through the existing air-drums code
5. only then activate small pieces of the rhythm game draft
