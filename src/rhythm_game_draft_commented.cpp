// this file is a draft of the real game logic.
// it is all commented out on purpose so milestone 1 still uploads as only the led test.
// later, we can move pieces into main.cpp one chunk at a time.

// // these two libraries give us normal arduino stuff and neopixel control
// #include <Arduino.h>
// #include <Adafruit_NeoPixel.h>

// // this draft is meant to sit next to air_drums.ino without changing her file
// // her code keeps owning sensors and sound
// // this game code just calls her getDistance and playDrum helpers

// // main hardware numbers for the three led rings
// const int game_ring_count = 3;
// const int game_leds_per_ring = 16;
// const int game_total_leds = game_ring_count * game_leds_per_ring;

// // pin 15 avoids the air-drums sensor pins
// // max_notes is a memory limit so a giant chart does not eat the esp32
// const int game_led_data_pin = 15;
// const int max_notes = 96;

// // each lane is just a number so arrays can store lane data easily
// const int lane_kick = 0;
// const int lane_snare = 1;
// const int lane_hihat = 2;

// // the game moves through these modes instead of doing everything at once
// const int mode_idle = 0;
// const int mode_ready = 1;
// const int mode_countdown = 2;
// const int mode_playing = 3;
// const int mode_finished = 4;

// // scoring results are numbers because they are faster and easier to compare
// const int result_miss = 0;
// const int result_okay = 1;
// const int result_good = 2;
// const int result_perfect = 3;

// // this says what the whole game is currently doing
// int game_mode = mode_idle;

// // timing and scoring settings
// // the windows are forgiving because the display only has 16 leds
// int game_brightness = 35;
// int game_fall_time_ms = 1800;
// int perfect_window_ms = 70;
// int good_window_ms = 120;
// int okay_window_ms = 170;
// int miss_window_ms = 220;

// // hand distance settings for the ultrasonic sensors
// // trigger_cm comes from air_drums.ino
// // 999 means no useful reading
// int min_hand_cm = 3;
// int max_hand_cm = 25;

// // score values that change during play
// int game_score = 0;
// int game_combo = 0;

// // these timers are based on millis so the game loop does not block
// unsigned long game_start_time = 0;
// unsigned long countdown_start_time = 0;
// int countdown_length_ms = 3000;

// // hit pixels are the front/bottom leds on each ring
// // offsets fix rings that are physically rotated differently
// int game_hit_pixel[game_ring_count] = {8, 8, 8};
// int game_ring_offset[game_ring_count] = {0, 0, 0};

// // live sensor data for each lane
// // sensor_lane_to_read lets us call one air-drums sensor at a time
// float game_sensor_cm[game_ring_count] = {999.0, 999.0, 999.0};
// unsigned long last_sensor_read_time = 0;
// int sensor_lane_to_read = 0;
// int sensor_gap_ms = 8;

// // each lane can flash after a hit or miss
// int flash_result[game_ring_count] = {result_miss, result_miss, result_miss};
// unsigned long flash_until_time[game_ring_count] = {0, 0, 0};

// // one note in the rhythm chart
// // time_ms is when the player should hit
// // lane says which pad to hit
// // was_hit and was_missed keep the same note from scoring twice
// struct game_note {
//   int time_ms;
//   int lane;
//   bool was_hit;
//   bool was_missed;
// };

// // this array holds the whole small test chart
// game_note notes[max_notes];
// int note_count = 0;

// // this controls all 48 leds as one long chain
// Adafruit_NeoPixel game_pixels(game_total_leds, game_led_data_pin, NEO_GRB + NEO_KHZ800);

// int game_wrap_pixel(int pixel) {
//   // keep a pixel number between 0 and 15
//   // this lets -1 become 15 and 16 become 0
//   while (pixel < 0) {
//     pixel = pixel + game_leds_per_ring;
//   }

//   while (pixel >= game_leds_per_ring) {
//     pixel = pixel - game_leds_per_ring;
//   }

//   return pixel;
// }

// int game_real_pixel(int lane, int logical_pixel) {
//   // convert a ring pixel into the actual pixel number in the full led chain
//   // example: lane 1 pixel 0 is really led 16
//   int fixed_pixel = game_wrap_pixel(logical_pixel + game_ring_offset[lane]);
//   return lane * game_leds_per_ring + fixed_pixel;
// }

// uint32_t game_color(int red, int green, int blue) {
//   // this keeps color making short and readable
//   return game_pixels.Color(red, green, blue);
// }

// uint32_t lane_color_for_game(int lane) {
//   // kick is blue
//   if (lane == lane_kick) {
//     return game_color(0, 90, 255);
//   }

//   // snare is red
//   if (lane == lane_snare) {
//     return game_color(255, 35, 35);
//   }

//   // hi-hat is yellow/orange
//   return game_color(255, 170, 0);
// }

// void play_local_drum_for_lane(int lane) {
//   // call her existing audio code instead of copying the sample logic
//   // air_drums.ino already has sensors[lane].sample and sensors[lane].sampleLen
//   playDrum(sensors[lane].sample, sensors[lane].sampleLen);
// }

// void load_test_chart() {
//   // reset the chart before adding notes
//   note_count = 0;

//   // this is a tiny test pattern so the board can play without a computer yet
//   // each line means time in ms, lane, hit flag, miss flag
//   notes[note_count++] = {1000, lane_kick, false, false};
//   notes[note_count++] = {1500, lane_snare, false, false};
//   notes[note_count++] = {2000, lane_hihat, false, false};
//   notes[note_count++] = {2500, lane_kick, false, false};
//   notes[note_count++] = {3000, lane_snare, false, false};
//   notes[note_count++] = {3500, lane_hihat, false, false};
//   notes[note_count++] = {4000, lane_kick, false, false};
//   notes[note_count++] = {4500, lane_kick, false, false};
//   notes[note_count++] = {5000, lane_snare, false, false};
//   notes[note_count++] = {5500, lane_hihat, false, false};
//   notes[note_count++] = {6000, lane_kick, false, false};
//   notes[note_count++] = {6500, lane_snare, false, false};
//   notes[note_count++] = {7000, lane_hihat, false, false};
//   notes[note_count++] = {7500, lane_snare, false, false};
//   notes[note_count++] = {8000, lane_kick, false, false};
//   notes[note_count++] = {9000, lane_hihat, false, false};
// }

// long current_song_ms() {
//   // during countdown the song has not started yet
//   if (game_mode == mode_countdown) {
//     return 0;
//   }

//   // outside gameplay we do not want random time values
//   if (game_mode != mode_playing) {
//     return 0;
//   }

//   // song time is just now minus when the game started
//   return (long)(millis() - game_start_time);
// }

// float read_one_sensor_cm(int lane) {
//   // call her existing distance helper instead of copying the sensor pulse code
//   if (lane < 0 || lane >= NUM_SENSORS) {
//     return 999.0;
//   }

//   long distance = getDistance(sensors[lane].trig, sensors[lane].echo);

//   // her helper returns zero when pulsein times out
//   if (distance == 0) {
//     return 999.0;
//   }

//   return distance;
// }

// bool is_hand_inside(float cm) {
//   // readings too close can be weird, so ignore them
//   if (cm < min_hand_cm) {
//     return false;
//   }

//   // readings too far away should not count as a drum hit
//   if (cm > max_hand_cm) {
//     return false;
//   }

//   // trigger_cm is defined in air_drums.ino
//   return cm <= TRIGGER_CM;
// }

// void start_lane_flash(int lane, int result) {
//   // remember what kind of flash to draw and when it should end
//   flash_result[lane] = result;
//   flash_until_time[lane] = millis() + 110;
// }

// int score_for_result(int result) {
//   // perfect hits get the most points
//   if (result == result_perfect) {
//     return 100;
//   }

//   // good hits are still solid
//   if (result == result_good) {
//     return 70;
//   }

//   // okay hits count, but score less
//   if (result == result_okay) {
//     return 40;
//   }

//   // misses do not add score
//   return 0;
// }

// const char *result_name(int result) {
//   // these words are printed over serial for debugging
//   if (result == result_perfect) {
//     return "perfect";
//   }

//   if (result == result_good) {
//     return "good";
//   }

//   if (result == result_okay) {
//     return "okay";
//   }

//   return "miss";
// }

// int result_from_delta(int abs_delta) {
//   // abs_delta is how early or late the hit was
//   if (abs_delta <= perfect_window_ms) {
//     return result_perfect;
//   }

//   if (abs_delta <= good_window_ms) {
//     return result_good;
//   }

//   if (abs_delta <= okay_window_ms) {
//     return result_okay;
//   }

//   return result_miss;
// }

// void handle_hit(int lane) {
//   // this runs when a sensor sees a new hand entry
//   long song_ms = current_song_ms();
//   int best_note = -1;
//   int best_abs_delta = 9999;

//   // find the nearest unscored note in the same lane
//   for (int i = 0; i < note_count; i++) {
//     // ignore notes for other drums
//     if (notes[i].lane != lane) {
//       continue;
//     }

//     // ignore notes that already got handled
//     if (notes[i].was_hit || notes[i].was_missed) {
//       continue;
//     }

//     // delta is positive if the player is late and negative if early
//     int delta = song_ms - notes[i].time_ms;
//     int abs_delta = abs(delta);

//     // keep the closest note that is still inside the okay window
//     if (abs_delta < best_abs_delta && abs_delta <= okay_window_ms) {
//       best_abs_delta = abs_delta;
//       best_note = i;
//     }
//   }

//   // no note was close enough, so this hand wave is a miss
//   if (best_note == -1) {
//     game_combo = 0;
//     start_lane_flash(lane, result_miss);
//     Serial.print("hit lane=");
//     Serial.print(lane);
//     Serial.println(" result=miss");
//     return;
//   }

//   // a matching note was found, so score it
//   int result = result_from_delta(best_abs_delta);
//   notes[best_note].was_hit = true;
//   game_score = game_score + score_for_result(result);
//   game_combo = game_combo + 1;
//   start_lane_flash(lane, result);
//   play_local_drum_for_lane(lane);

//   Serial.print("hit lane=");
//   Serial.print(lane);
//   Serial.print(" local_ms=");
//   Serial.print(song_ms);
//   Serial.print(" result=");
//   Serial.print(result_name(result));
//   Serial.print(" delta_ms=");
//   Serial.println(song_ms - notes[best_note].time_ms);

//   Serial.print("score score=");
//   Serial.print(game_score);
//   Serial.print(" combo=");
//   Serial.println(game_combo);
// }

// void update_sensors() {
//   // this calls one air-drums sensor at a time instead of running her whole loop
//   unsigned long now = millis();

//   // wait a little between sensor reads to reduce cross-talk
//   if (now - last_sensor_read_time < (unsigned long)sensor_gap_ms) {
//     return;
//   }

//   last_sensor_read_time = now;

//   // pick one lane to read this time through the loop
//   int lane = sensor_lane_to_read;
//   sensor_lane_to_read = sensor_lane_to_read + 1;
//   if (sensor_lane_to_read >= game_ring_count) {
//     sensor_lane_to_read = 0;
//   }

//   // use her sensor array so her pin/sample data stays the source of truth
//   if (lane >= NUM_SENSORS) {
//     return;
//   }

//   DrumSensor& sensor = sensors[lane];
//   game_sensor_cm[lane] = read_one_sensor_cm(lane);
//   bool inside = is_hand_inside(game_sensor_cm[lane]);

//   // only a change from outside to inside counts as a hit
//   // debounce_ms and sensor.inZone come from air_drums.ino
//   if (inside && !sensor.inZone && (now - sensor.lastTriggerMs >= DEBOUNCE_MS)) {
//     sensor.lastTriggerMs = now;
//     handle_hit(lane);
//   }

//   // update her in-zone state so the next loop does not double trigger
//   sensor.inZone = inside;
// }

// void check_for_misses() {
//   // only mark misses while the chart is playing
//   if (game_mode != mode_playing) {
//     return;
//   }

//   long song_ms = current_song_ms();

//   // if a note is too far in the past, the player missed it
//   for (int i = 0; i < note_count; i++) {
//     if (notes[i].was_hit || notes[i].was_missed) {
//       continue;
//     }

//     if (song_ms > notes[i].time_ms + miss_window_ms) {
//       notes[i].was_missed = true;
//       game_combo = 0;
//       start_lane_flash(notes[i].lane, result_miss);

//       Serial.print("miss lane=");
//       Serial.print(notes[i].lane);
//       Serial.print(" note_ms=");
//       Serial.println(notes[i].time_ms);
//     }
//   }
// }

// void draw_game_hit_zone(int lane) {
//   // draw the bottom/front target area on the ring
//   int center = game_hit_pixel[lane];
//   game_pixels.setPixelColor(game_real_pixel(lane, center), game_color(20, 20, 20));
//   game_pixels.setPixelColor(game_real_pixel(lane, center - 1), game_color(6, 6, 6));
//   game_pixels.setPixelColor(game_real_pixel(lane, center + 1), game_color(6, 6, 6));
// }

// void draw_game_note(int lane, float pixel_pos, uint32_t color) {
//   // round the floating note position to the nearest led for now
//   int center = (int)(pixel_pos + 0.5);

//   // draw a chunky glow instead of one tiny dot
//   game_pixels.setPixelColor(game_real_pixel(lane, center - 2), game_color(0, 0, 5));
//   game_pixels.setPixelColor(game_real_pixel(lane, center - 1), color);
//   game_pixels.setPixelColor(game_real_pixel(lane, center), color);
//   game_pixels.setPixelColor(game_real_pixel(lane, center + 1), color);
// }

// void draw_game_flash(int lane) {
//   // skip if this lane is not flashing right now
//   if (millis() > flash_until_time[lane]) {
//     return;
//   }

//   // red is the default miss color
//   uint32_t color = game_color(255, 0, 0);

//   // better hits get happier colors
//   if (flash_result[lane] == result_perfect) {
//     color = game_color(255, 255, 255);
//   } else if (flash_result[lane] == result_good) {
//     color = game_color(0, 255, 100);
//   } else if (flash_result[lane] == result_okay) {
//     color = game_color(0, 90, 255);
//   }

//   // flash the whole ring for quick feedback
//   for (int i = 0; i < game_leds_per_ring; i++) {
//     game_pixels.setPixelColor(game_real_pixel(lane, i), color);
//   }
// }

// void render_game() {
//   // build one led frame from scratch
//   game_pixels.clear();

//   // hit zones stay visible even if no notes are nearby
//   for (int lane = 0; lane < game_ring_count; lane++) {
//     draw_game_hit_zone(lane);
//   }

//   // only draw moving notes during actual gameplay
//   if (game_mode == mode_playing) {
//     long song_ms = current_song_ms();

//     // draw every note that is currently visible
//     for (int i = 0; i < note_count; i++) {
//       if (notes[i].was_hit || notes[i].was_missed) {
//         continue;
//       }

//       long note_ms = notes[i].time_ms;
//       long appear_ms = note_ms - game_fall_time_ms;

//       // skip notes that have not appeared yet or already reached the hit time
//       if (song_ms < appear_ms || song_ms > note_ms) {
//         continue;
//       }

//       // progress goes from 0.0 at spawn to 1.0 at the hit pixel
//       float progress = (float)(song_ms - appear_ms) / (float)game_fall_time_ms;

//       // start one full ring before the hit pixel so it travels around the ring
//       float start_pixel = game_hit_pixel[notes[i].lane] - game_leds_per_ring;
//       float pixel_pos = start_pixel + progress * game_leds_per_ring;
//       draw_game_note(notes[i].lane, pixel_pos, lane_color_for_game(notes[i].lane));
//     }
//   }

//   // flashes are drawn last so they show over the notes
//   for (int lane = 0; lane < game_ring_count; lane++) {
//     draw_game_flash(lane);
//   }

//   // send the finished frame to the leds
//   game_pixels.show();
// }

// void start_countdown() {
//   // reset play state and begin a short countdown before notes move
//   game_mode = mode_countdown;
//   countdown_start_time = millis();
//   game_score = 0;
//   game_combo = 0;

//   // make every note playable again
//   for (int i = 0; i < note_count; i++) {
//     notes[i].was_hit = false;
//     notes[i].was_missed = false;
//   }

//   Serial.println("countdown started");
// }

// void update_countdown() {
//   // only do countdown work during countdown mode
//   if (game_mode != mode_countdown) {
//     return;
//   }

//   // when countdown is done, lock in the start time
//   if (millis() - countdown_start_time >= (unsigned long)countdown_length_ms) {
//     game_mode = mode_playing;
//     game_start_time = millis();
//     Serial.println("game started");
//   }
// }

// void setup_game_logic() {
//   // start the led chain and turn everything off
//   game_pixels.begin();
//   game_pixels.setBrightness(game_brightness);
//   game_pixels.clear();
//   game_pixels.show();

//   // air_drums.ino already sets up sensor pins and the audio timer
//   // do not copy that setup here

//   // load the built-in test chart for the local prototype
//   load_test_chart();
//   game_mode = mode_ready;
// }

// void loop_game_logic() {
//   // for the local prototype, typing s in serial starts the countdown
//   if (Serial.available() > 0) {
//     char command = Serial.read();
//     if (command == 's') {
//       start_countdown();
//     }
//   }

//   // these updates are small and non-blocking
//   update_countdown();

//   // sensors and misses matter only while playing
//   if (game_mode == mode_playing) {
//     update_sensors();
//     check_for_misses();
//   }

//   // render every loop so the rings feel responsive
//   render_game();
// }
