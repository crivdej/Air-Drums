#include <Arduino.h>
#include <Adafruit_NeoPixel.h>
#include <cstring>

#include "air_drums_shared.h"
#include "rhythm_game.h"

namespace {

constexpr int game_ring_count = 3;
constexpr int game_leds_per_ring = 16;
constexpr int game_total_leds = game_ring_count * game_leds_per_ring;
constexpr int game_led_data_pin = 15;
constexpr int max_notes = 96;
constexpr int serial_buffer_size = 96;

constexpr int lane_hihat = 0;
constexpr int lane_kick = 1;
constexpr int lane_snare = 2;

constexpr int mode_idle = 0;
constexpr int mode_ready = 1;
constexpr int mode_countdown = 2;
constexpr int mode_playing = 3;
constexpr int mode_finished = 4;

constexpr int result_miss = 0;
constexpr int result_okay = 1;
constexpr int result_good = 2;
constexpr int result_perfect = 3;

struct GameNote {
  int time_ms;
  int lane;
  bool was_hit;
  bool was_missed;
};

int game_mode = mode_idle;
int game_brightness = 35;
int game_fall_time_ms = 1800;
int perfect_window_ms = 70;
int good_window_ms = 120;
int okay_window_ms = 170;
int miss_window_ms = 220;

int min_hand_cm = 3;
int max_hand_cm = 25;

int game_score = 0;
int game_combo = 0;
int game_hit_lockout_ms = 350;
int countdown_length_ms = 3000;
int song_end_time_ms = 0;

unsigned long game_start_time = 0;
unsigned long countdown_start_time = 0;
unsigned long scheduled_song_start_time = 0;
unsigned long last_sensor_read_time = 0;
unsigned long flash_until_time[game_ring_count] = {0, 0, 0};
unsigned long next_allowed_game_hit_time[game_ring_count] = {0, 0, 0};

int game_hit_pixel[game_ring_count] = {8, 8, 8};
int game_ring_offset[game_ring_count] = {0, 0, 0};
int sensor_lane_to_read = 0;
int sensor_gap_ms = 8;
int flash_result[game_ring_count] = {result_miss, result_miss, result_miss};

float game_sensor_cm[game_ring_count] = {999.0f, 999.0f, 999.0f};
bool pc_player_ready = false;

char active_track_id[32] = "steves_lava_chicken";
char ready_track_id[32] = "";
char serial_buffer[serial_buffer_size];
int serial_buffer_len = 0;

GameNote notes[max_notes];
int note_count = 0;

Adafruit_NeoPixel game_pixels(game_total_leds, game_led_data_pin, NEO_GRB + NEO_KHZ800);

bool starts_with(const char* value, const char* prefix) {
  return strncmp(value, prefix, strlen(prefix)) == 0;
}

void send_board_line(const char* line) {
  Serial.println(line);
}

void send_board_load() {
  Serial.print("BOARD LOAD ");
  Serial.println(active_track_id);
}

void send_board_start(int delay_ms) {
  Serial.print("BOARD START ");
  Serial.print(active_track_id);
  Serial.print(" ");
  Serial.println(delay_ms);
}

void send_board_stop() {
  send_board_line("BOARD STOP");
}

void announce_player_state() {
  send_board_line("BOARD HELLO");
  send_board_load();
}

int game_wrap_pixel(int pixel) {
  while (pixel < 0) {
    pixel += game_leds_per_ring;
  }

  while (pixel >= game_leds_per_ring) {
    pixel -= game_leds_per_ring;
  }

  return pixel;
}

int game_real_pixel(int lane, int logical_pixel) {
  int fixed_pixel = game_wrap_pixel(logical_pixel + game_ring_offset[lane]);
  return lane * game_leds_per_ring + fixed_pixel;
}

uint32_t game_color(int red, int green, int blue) {
  return game_pixels.Color(red, green, blue);
}

uint32_t lane_color_for_game(int lane) {
  if (lane == lane_kick) {
    return game_color(0, 90, 255);
  }

  if (lane == lane_snare) {
    return game_color(255, 35, 35);
  }

  return game_color(255, 170, 0);
}

void play_local_drum_for_lane(int lane) {
  if (lane < 0 || lane >= NUM_SENSORS) {
    return;
  }

  playDrum(sensors[lane].sample, sensors[lane].sampleLen);
}

void load_test_chart() {
  note_count = 0;

  notes[note_count++] = {1000, lane_kick, false, false};
  notes[note_count++] = {1500, lane_snare, false, false};
  notes[note_count++] = {2000, lane_hihat, false, false};
  notes[note_count++] = {2500, lane_kick, false, false};
  notes[note_count++] = {3000, lane_snare, false, false};
  notes[note_count++] = {3500, lane_hihat, false, false};
  notes[note_count++] = {4000, lane_kick, false, false};
  notes[note_count++] = {4500, lane_kick, false, false};
  notes[note_count++] = {5000, lane_snare, false, false};
  notes[note_count++] = {5500, lane_hihat, false, false};
  notes[note_count++] = {6000, lane_kick, false, false};
  notes[note_count++] = {6500, lane_snare, false, false};
  notes[note_count++] = {7000, lane_hihat, false, false};
  notes[note_count++] = {7500, lane_snare, false, false};
  notes[note_count++] = {8000, lane_kick, false, false};
  notes[note_count++] = {9000, lane_hihat, false, false};

  song_end_time_ms = notes[note_count - 1].time_ms;
}

long current_song_ms() {
  if (game_mode != mode_playing) {
    return 0;
  }

  return (long)(millis() - game_start_time);
}

float read_one_sensor_cm(int lane) {
  if (lane < 0 || lane >= NUM_SENSORS) {
    return 999.0f;
  }

  long distance = getDistance(sensors[lane].trig, sensors[lane].echo);
  if (distance == 0) {
    return 999.0f;
  }

  return (float)distance;
}

bool is_hand_inside(float cm) {
  if (cm < min_hand_cm) {
    return false;
  }

  if (cm > max_hand_cm) {
    return false;
  }

  return cm <= TRIGGER_CM;
}

void start_lane_flash(int lane, int result) {
  flash_result[lane] = result;
  flash_until_time[lane] = millis() + 110;
}

int score_for_result(int result) {
  if (result == result_perfect) {
    return 100;
  }

  if (result == result_good) {
    return 70;
  }

  if (result == result_okay) {
    return 40;
  }

  return 0;
}

const char* result_name(int result) {
  if (result == result_perfect) {
    return "perfect";
  }

  if (result == result_good) {
    return "good";
  }

  if (result == result_okay) {
    return "okay";
  }

  return "miss";
}

int result_from_delta(int abs_delta) {
  if (abs_delta <= perfect_window_ms) {
    return result_perfect;
  }

  if (abs_delta <= good_window_ms) {
    return result_good;
  }

  if (abs_delta <= okay_window_ms) {
    return result_okay;
  }

  return result_miss;
}

void handle_hit(int lane) {
  long song_ms = current_song_ms();
  int best_note = -1;
  int best_abs_delta = 9999;

  for (int i = 0; i < note_count; i++) {
    if (notes[i].lane != lane) {
      continue;
    }

    if (notes[i].was_hit || notes[i].was_missed) {
      continue;
    }

    int delta = song_ms - notes[i].time_ms;
    int abs_delta = abs(delta);

    if (abs_delta < best_abs_delta && abs_delta <= okay_window_ms) {
      best_abs_delta = abs_delta;
      best_note = i;
    }
  }

  if (best_note == -1) {
    game_combo = 0;
    start_lane_flash(lane, result_miss);
    Serial.print("hit lane=");
    Serial.print(lane);
    Serial.println(" result=miss");
    return;
  }

  int result = result_from_delta(best_abs_delta);
  notes[best_note].was_hit = true;
  game_score += score_for_result(result);
  game_combo += 1;
  start_lane_flash(lane, result);
  play_local_drum_for_lane(lane);

  Serial.print("hit lane=");
  Serial.print(lane);
  Serial.print(" local_ms=");
  Serial.print(song_ms);
  Serial.print(" result=");
  Serial.print(result_name(result));
  Serial.print(" delta_ms=");
  Serial.println(song_ms - notes[best_note].time_ms);
}

void update_sensors() {
  unsigned long now = millis();

  if (now - last_sensor_read_time < (unsigned long)sensor_gap_ms) {
    return;
  }

  last_sensor_read_time = now;

  int lane = sensor_lane_to_read;
  sensor_lane_to_read += 1;
  if (sensor_lane_to_read >= game_ring_count) {
    sensor_lane_to_read = 0;
  }

  if (lane >= NUM_SENSORS) {
    return;
  }

  DrumSensor& sensor = sensors[lane];
  game_sensor_cm[lane] = read_one_sensor_cm(lane);
  bool inside = is_hand_inside(game_sensor_cm[lane]);

  if (inside && !sensor.inZone && now >= next_allowed_game_hit_time[lane] &&
      (now - sensor.lastTriggerMs >= DEBOUNCE_MS)) {
    sensor.lastTriggerMs = now;
    next_allowed_game_hit_time[lane] = now + game_hit_lockout_ms;
    handle_hit(lane);
  }

  sensor.inZone = inside;
}

void check_for_misses() {
  if (game_mode != mode_playing) {
    return;
  }

  long song_ms = current_song_ms();

  for (int i = 0; i < note_count; i++) {
    if (notes[i].was_hit || notes[i].was_missed) {
      continue;
    }

    if (song_ms > notes[i].time_ms + miss_window_ms) {
      notes[i].was_missed = true;
      game_combo = 0;
      start_lane_flash(notes[i].lane, result_miss);
    }
  }
}

void draw_game_hit_zone(int lane) {
  int center = game_hit_pixel[lane];
  game_pixels.setPixelColor(game_real_pixel(lane, center), game_color(20, 20, 20));
  game_pixels.setPixelColor(game_real_pixel(lane, center - 1), game_color(6, 6, 6));
  game_pixels.setPixelColor(game_real_pixel(lane, center + 1), game_color(6, 6, 6));
}

void draw_game_note(int lane, float pixel_pos, uint32_t color) {
  int center = (int)(pixel_pos + 0.5f);
  game_pixels.setPixelColor(game_real_pixel(lane, center - 2), game_color(0, 0, 5));
  game_pixels.setPixelColor(game_real_pixel(lane, center - 1), color);
  game_pixels.setPixelColor(game_real_pixel(lane, center), color);
  game_pixels.setPixelColor(game_real_pixel(lane, center + 1), color);
}

void draw_game_flash(int lane) {
  if (millis() > flash_until_time[lane]) {
    return;
  }

  uint32_t color = game_color(255, 0, 0);

  if (flash_result[lane] == result_perfect) {
    color = game_color(255, 255, 255);
  } else if (flash_result[lane] == result_good) {
    color = game_color(0, 255, 100);
  } else if (flash_result[lane] == result_okay) {
    color = game_color(0, 90, 255);
  }

  for (int i = 0; i < game_leds_per_ring; i++) {
    game_pixels.setPixelColor(game_real_pixel(lane, i), color);
  }
}

void render_countdown() {
  int lit_rings = 0;
  if (countdown_length_ms > 0) {
    unsigned long elapsed = millis() - countdown_start_time;
    int remaining_ms = countdown_length_ms - (int)elapsed;
    if (remaining_ms < 0) {
      remaining_ms = 0;
    }

    lit_rings = (remaining_ms + 999) / 1000;
  }

  for (int lane = 0; lane < game_ring_count; lane++) {
    if (lane < lit_rings) {
      for (int pixel = 0; pixel < game_leds_per_ring; pixel++) {
        game_pixels.setPixelColor(game_real_pixel(lane, pixel), lane_color_for_game(lane));
      }
    }
  }
}

void render_game() {
  game_pixels.clear();

  for (int lane = 0; lane < game_ring_count; lane++) {
    draw_game_hit_zone(lane);
  }

  if (game_mode == mode_countdown) {
    render_countdown();
  }

  if (game_mode == mode_playing) {
    long song_ms = current_song_ms();

    for (int i = 0; i < note_count; i++) {
      if (notes[i].was_hit || notes[i].was_missed) {
        continue;
      }

      long note_ms = notes[i].time_ms;
      long appear_ms = note_ms - game_fall_time_ms;

      if (song_ms < appear_ms || song_ms > note_ms) {
        continue;
      }

      float progress = (float)(song_ms - appear_ms) / (float)game_fall_time_ms;
      float start_pixel = (float)(game_hit_pixel[notes[i].lane] - game_leds_per_ring);
      float pixel_pos = start_pixel + progress * game_leds_per_ring;
      draw_game_note(notes[i].lane, pixel_pos, lane_color_for_game(notes[i].lane));
    }
  }

  for (int lane = 0; lane < game_ring_count; lane++) {
    draw_game_flash(lane);
  }

  game_pixels.show();
}

void stop_game() {
  game_mode = mode_ready;
  send_board_stop();
  Serial.println("game stopped");
}

void start_countdown() {
  game_mode = mode_countdown;
  countdown_start_time = millis();
  scheduled_song_start_time = countdown_start_time + countdown_length_ms;
  game_score = 0;
  game_combo = 0;

  for (int i = 0; i < note_count; i++) {
    notes[i].was_hit = false;
    notes[i].was_missed = false;
  }

  for (int lane = 0; lane < game_ring_count; lane++) {
    next_allowed_game_hit_time[lane] = 0;
    flash_until_time[lane] = 0;
  }

  send_board_start(countdown_length_ms);
  Serial.println("countdown started");
}

void update_countdown() {
  if (game_mode != mode_countdown) {
    return;
  }

  if (millis() >= scheduled_song_start_time) {
    game_mode = mode_playing;
    game_start_time = scheduled_song_start_time;
    Serial.println("game started");
  }
}

void finish_song_if_needed() {
  if (game_mode != mode_playing) {
    return;
  }

  long song_ms = current_song_ms();
  if (song_ms <= song_end_time_ms + miss_window_ms + 300) {
    return;
  }

  game_mode = mode_finished;
  send_board_stop();
  Serial.print("song finished score=");
  Serial.print(game_score);
  Serial.print(" combo=");
  Serial.println(game_combo);
}

void handle_host_ready(const char* track_id) {
  strncpy(ready_track_id, track_id, sizeof(ready_track_id) - 1);
  ready_track_id[sizeof(ready_track_id) - 1] = '\0';
  pc_player_ready = strcmp(ready_track_id, active_track_id) == 0;

  Serial.print("player ready track=");
  Serial.println(ready_track_id);
}

void handle_host_error(const char* message) {
  pc_player_ready = false;
  ready_track_id[0] = '\0';
  Serial.print("player error=");
  Serial.println(message);
}

void process_serial_line(const char* line) {
  if (line[0] == '\0') {
    return;
  }

  if (strcmp(line, "s") == 0 || strcmp(line, "HOST START_GAME") == 0) {
    start_countdown();
    return;
  }

  if (strcmp(line, "x") == 0 || strcmp(line, "HOST STOP_GAME") == 0) {
    stop_game();
    return;
  }

  if (strcmp(line, "p") == 0 || strcmp(line, "HOST REQUEST_STATE") == 0) {
    announce_player_state();
    return;
  }

  if (strcmp(line, "HOST HELLO") == 0) {
    announce_player_state();
    return;
  }

  if (starts_with(line, "HOST READY ")) {
    handle_host_ready(line + 11);
    return;
  }

  if (starts_with(line, "HOST ERROR ")) {
    handle_host_error(line + 11);
  }
}

void handle_game_serial_input() {
  while (Serial.available() > 0) {
    char incoming = (char)Serial.read();

    if (incoming == '\r' || incoming == '\n') {
      if (serial_buffer_len > 0) {
        serial_buffer[serial_buffer_len] = '\0';
        process_serial_line(serial_buffer);
        serial_buffer_len = 0;
      }
      continue;
    }

    if (serial_buffer_len < serial_buffer_size - 1) {
      serial_buffer[serial_buffer_len++] = incoming;
    }
  }
}

}  // namespace

void setup_game_logic() {
  game_pixels.begin();
  game_pixels.setBrightness(game_brightness);
  game_pixels.clear();
  game_pixels.show();

  load_test_chart();
  game_mode = mode_ready;

  announce_player_state();
  Serial.println("game ready");
  Serial.println("commands: s=start, x=stop, p=resend player state");
  Serial.println("host commands: HOST START_GAME, HOST STOP_GAME, HOST REQUEST_STATE");
}

void loop_game_logic() {
  handle_game_serial_input();
  update_countdown();

  if (game_mode == mode_playing) {
    update_sensors();
    check_for_misses();
    finish_song_if_needed();
  }

  render_game();
}
