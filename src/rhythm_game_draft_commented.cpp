#include <Arduino.h>
#include <Adafruit_NeoPixel.h>
#include <SSD1306Wire.h>
#include <cstring>

#include "air_drums_shared.h"
#include "generated_charts/chart_registry.h"
#include "rhythm_game.h"

namespace {

constexpr int game_ring_count = 3;
constexpr int game_leds_per_ring = 16;
constexpr int game_total_leds = game_ring_count * game_leds_per_ring;
constexpr int game_led_data_pin = 23;
constexpr int max_notes = 550;
constexpr int serial_buffer_size = 96;
constexpr int game_led_frame_gap_ms = 16;
constexpr bool game_leds_enabled = true;
constexpr bool game_oled_enabled = true;

constexpr int lane_hihat = 0;
constexpr int lane_kick = 1;
constexpr int lane_snare = 2;

constexpr int mode_idle = 0;
constexpr int mode_ready = 1;
constexpr int mode_countdown = 2;
constexpr int mode_playing = 3;
constexpr int mode_finished = 4;

constexpr int result_miss = 0;
constexpr int result_bad = 1;
constexpr int result_good = 2;
constexpr int result_great = 3;
constexpr int result_perfect = 4;

struct GameNote {
  int time_ms;
  int lane;
  int color_slot;
  bool was_hit;
  bool was_missed;
};

struct SongChart {
  const char* track_id;
  const AirDrumsChartNote* chart_notes;
  int note_count;
  int song_end_ms;
};

const SongChart song_charts[] = {
AIR_DRUMS_SONG_CHARTS
};

constexpr int song_chart_count = sizeof(song_charts) / sizeof(song_charts[0]);

int game_mode = mode_idle;
int game_brightness = 45;
int game_fall_time_ms = 1200;
int game_note_travel_pixels = game_leds_per_ring / 2;
int max_visible_notes_per_lane = 3;
int close_note_gap_ms = 650;
int perfect_window_ms = 70;
int great_window_ms = 120;
int good_window_ms = 170;
int bad_window_ms = 220;
int miss_window_ms = 220;

int min_hand_cm = 3;
int max_hand_cm = 25;

int game_score = 0;
int game_combo = 0;
int game_max_combo = 0;
int game_judged_notes = 0;
int game_hit_notes = 0;
int game_missed_notes = 0;
int game_extra_hits = 0;
int game_perfect_notes = 0;
int game_great_notes = 0;
int game_good_notes = 0;
int game_bad_notes = 0;
long game_abs_delta_total_ms = 0;
int game_hit_lockout_ms = 350;
int quick_note_lockout_ms = 120;
int countdown_length_ms = 3000;
int song_end_time_ms = 0;

unsigned long game_start_time = 0;
unsigned long countdown_start_time = 0;
unsigned long scheduled_song_start_time = 0;
unsigned long last_sensor_read_time = 0;
unsigned long last_game_telemetry_time = 0;
unsigned long last_oled_draw_time = 0;
unsigned long last_game_led_draw_time = 0;
unsigned long last_note_result_time = 0;
unsigned long flash_until_time[game_ring_count] = {0, 0, 0};
unsigned long next_allowed_game_hit_time[game_ring_count] = {0, 0, 0};

int game_hit_pixel[game_ring_count] = {0, 0, 0};
int game_ring_offset[game_ring_count] = {0, 0, 0};
int game_physical_ring_for_lane[game_ring_count] = {0, 2, 1};
int sensor_lane_to_read = 0;
int sensor_gap_ms = 8;
int flash_result[game_ring_count] = {result_miss, result_miss, result_miss};
int last_note_result = result_miss;
int last_note_lane = -1;
int last_note_delta_ms = 0;

float game_sensor_cm[game_ring_count] = {999.0f, 999.0f, 999.0f};
bool pc_player_ready = false;
bool game_led_dirty = true;
bool game_oled_dirty = true;

char active_track_id[32] = "takefive";
char ready_track_id[32] = "";
char serial_buffer[serial_buffer_size];
int serial_buffer_len = 0;
int active_chart_index = 0;

GameNote notes[max_notes];
int note_count = 0;
int first_pending_note = 0;
int close_color_note_count = 0;

Adafruit_NeoPixel game_pixels(game_total_leds, game_led_data_pin, NEO_GRB + NEO_KHZ800);
SSD1306Wire game_oled(0x3c, SDA, SCL);

long current_song_ms();
void setup_oled_status();
void draw_oled_status(bool force_draw);

void request_led_draw() {
  game_led_dirty = true;
}

void request_oled_draw() {
  game_oled_dirty = true;
}

void advance_first_pending_note() {
  while (first_pending_note < note_count) {
    if (notes[first_pending_note].was_hit || notes[first_pending_note].was_missed) {
      first_pending_note++;
      continue;
    }

    break;
  }
}

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

void copy_active_track_id() {
  strncpy(active_track_id, song_charts[active_chart_index].track_id, sizeof(active_track_id) - 1);
  active_track_id[sizeof(active_track_id) - 1] = '\0';
}

const char* lane_name_for_game(int lane) {
  if (lane == lane_hihat) {
    return "hihat";
  }

  if (lane == lane_kick) {
    return "kick";
  }

  if (lane == lane_snare) {
    return "snare";
  }

  return "unknown";
}

const char* mode_name_for_game() {
  if (game_mode == mode_ready) {
    return "ready";
  }

  if (game_mode == mode_countdown) {
    return "countdown";
  }

  if (game_mode == mode_playing) {
    return "playing";
  }

  if (game_mode == mode_finished) {
    return "finished";
  }

  return "idle";
}

int next_note_index(long song_ms) {
  advance_first_pending_note();

  for (int i = first_pending_note; i < note_count; i++) {
    if (notes[i].was_hit || notes[i].was_missed) {
      continue;
    }

    if (notes[i].time_ms < song_ms - miss_window_ms) {
      continue;
    }

    return i;
  }

  return -1;
}

int percent_rounded(long value, long maximum) {
  if (maximum <= 0) {
    return 0;
  }

  return (int)((value * 100L + maximum / 2L) / maximum);
}

int chart_score_percent() {
  return percent_rounded((long)game_score, (long)note_count * 100L);
}

int judged_quality_percent() {
  return percent_rounded((long)game_score, (long)game_judged_notes * 100L);
}

int hit_rate_percent() {
  return percent_rounded((long)game_hit_notes, (long)note_count);
}

int average_hit_delta_ms() {
  if (game_hit_notes <= 0) {
    return 0;
  }

  return (int)((game_abs_delta_total_ms + game_hit_notes / 2L) / game_hit_notes);
}

const char* performance_grade() {
  int score = chart_score_percent();

  if (score >= 97) {
    return "S+";
  }

  if (score >= 93) {
    return "S";
  }

  if (score >= 90) {
    return "A";
  }

  if (score >= 80) {
    return "B";
  }

  if (score >= 70) {
    return "C";
  }

  if (score >= 60) {
    return "D";
  }

  return "F";
}

void send_score_line() {
  Serial.print("GAME SCORE score=");
  Serial.print(chart_score_percent());
  Serial.print(" quality_pct=");
  Serial.print(judged_quality_percent());
  Serial.print(" combo=");
  Serial.print(game_combo);
  Serial.print(" max_combo=");
  Serial.print(game_max_combo);
  Serial.print(" hits=");
  Serial.print(game_hit_notes);
  Serial.print(" misses=");
  Serial.print(game_missed_notes);
  Serial.print(" extras=");
  Serial.print(game_extra_hits);
  Serial.print(" hit_rate_pct=");
  Serial.print(hit_rate_percent());
  Serial.print(" avg_delta_ms=");
  Serial.print(average_hit_delta_ms());
  Serial.print(" grade=");
  Serial.println(performance_grade());
}

void send_game_telemetry(bool force_send) {
  unsigned long now = millis();

  if (!force_send && now - last_game_telemetry_time < 250) {
    return;
  }

  last_game_telemetry_time = now;

  long song_ms = current_song_ms();
  long countdown_ms = 0;
  if (game_mode == mode_countdown && scheduled_song_start_time > now) {
    countdown_ms = (long)(scheduled_song_start_time - now);
  }

  int next_note = next_note_index(song_ms);
  int next_lane = -1;
  int next_note_ms = -1;
  int next_in_ms = -1;

  if (next_note >= 0) {
    next_lane = notes[next_note].lane;
    next_note_ms = notes[next_note].time_ms;
    next_in_ms = notes[next_note].time_ms - song_ms;
  }

  Serial.print("GAME TICK mode=");
  Serial.print(mode_name_for_game());
  Serial.print(" song_ms=");
  Serial.print(song_ms);
  Serial.print(" countdown_ms=");
  Serial.print(countdown_ms);
  Serial.print(" score=");
  Serial.print(chart_score_percent());
  Serial.print(" quality_pct=");
  Serial.print(judged_quality_percent());
  Serial.print(" combo=");
  Serial.print(game_combo);
  Serial.print(" max_combo=");
  Serial.print(game_max_combo);
  Serial.print(" hits=");
  Serial.print(game_hit_notes);
  Serial.print(" misses=");
  Serial.print(game_missed_notes);
  Serial.print(" extras=");
  Serial.print(game_extra_hits);
  Serial.print(" hit_rate_pct=");
  Serial.print(hit_rate_percent());
  Serial.print(" avg_delta_ms=");
  Serial.print(average_hit_delta_ms());
  Serial.print(" grade=");
  Serial.print(performance_grade());
  Serial.print(" next_lane=");
  Serial.print(next_lane);
  Serial.print(" next_name=");
  Serial.print(next_lane >= 0 ? lane_name_for_game(next_lane) : "none");
  Serial.print(" next_note_ms=");
  Serial.print(next_note_ms);
  Serial.print(" next_in_ms=");
  Serial.println(next_in_ms);
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
  return game_physical_ring_for_lane[lane] * game_leds_per_ring + fixed_pixel;
}

uint32_t game_color(int red, int green, int blue) {
  return game_pixels.Color(red, green, blue);
}

uint32_t scale_game_color(uint32_t color, int percent) {
  uint8_t red = (uint8_t)(color >> 16);
  uint8_t green = (uint8_t)(color >> 8);
  uint8_t blue = (uint8_t)(color);

  red = (red * percent) / 100;
  green = (green * percent) / 100;
  blue = (blue * percent) / 100;

  return game_color(red, green, blue);
}

void add_game_pixel_color(int pixel_number, uint32_t color) {
  uint32_t old_color = game_pixels.getPixelColor(pixel_number);

  int old_red = (uint8_t)(old_color >> 16);
  int old_green = (uint8_t)(old_color >> 8);
  int old_blue = (uint8_t)(old_color);

  int new_red = (uint8_t)(color >> 16);
  int new_green = (uint8_t)(color >> 8);
  int new_blue = (uint8_t)(color);

  int mixed_red = old_red + new_red;
  int mixed_green = old_green + new_green;
  int mixed_blue = old_blue + new_blue;

  if (mixed_red > 255) {
    mixed_red = 255;
  }
  if (mixed_green > 255) {
    mixed_green = 255;
  }
  if (mixed_blue > 255) {
    mixed_blue = 255;
  }

  game_pixels.setPixelColor(pixel_number, game_color(mixed_red, mixed_green, mixed_blue));
}

int floor_float_to_int(float value) {
  int whole = (int)value;
  if ((float)whole > value) {
    whole--;
  }
  return whole;
}

float positive_float(float value) {
  if (value < 0.0f) {
    return -value;
  }
  return value;
}

int note_glow_percent(float distance) {
  bool behind_note = distance > 0.0f;
  distance = positive_float(distance);

  if (!behind_note && distance > 0.85f) {
    return 0;
  }

  if (distance <= 0.20f) {
    return 100;
  }

  if (distance <= 0.90f) {
    return 100 - (int)((distance - 0.20f) * 85.0f);
  }

  if (behind_note && distance <= 1.70f) {
    return 40 - (int)((distance - 0.90f) * 38.0f);
  }

  return 0;
}

int note_brightness_percent(float progress) {
  int percent = 35 + (int)(progress * 75.0f);

  if (percent < 35) {
    return 35;
  }

  if (percent > 100) {
    return 100;
  }

  return percent;
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

uint32_t note_color_for_game(int lane, int color_slot) {
  if (color_slot == 1) {
    if (lane == lane_kick) {
      return game_color(0, 220, 255);
    }

    if (lane == lane_snare) {
      return game_color(255, 0, 160);
    }

    return game_color(190, 255, 0);
  }

  if (color_slot == 2) {
    if (lane == lane_kick) {
      return game_color(145, 70, 255);
    }

    if (lane == lane_snare) {
      return game_color(255, 150, 0);
    }

    return game_color(0, 255, 160);
  }

  return lane_color_for_game(lane);
}

void play_local_drum_for_lane(int lane) {
  if (lane < 0 || lane >= NUM_SENSORS) {
    return;
  }

  playDrum(sensors[lane].sample, sensors[lane].sampleLen);
}

void reset_performance_stats() {
  game_score = 0;
  game_combo = 0;
  game_max_combo = 0;
  game_judged_notes = 0;
  game_hit_notes = 0;
  game_missed_notes = 0;
  game_extra_hits = 0;
  game_perfect_notes = 0;
  game_great_notes = 0;
  game_good_notes = 0;
  game_bad_notes = 0;
  game_abs_delta_total_ms = 0;
}

void assign_note_color_slots() {
  int last_note_for_lane[game_ring_count] = {-1, -1, -1};
  close_color_note_count = 0;

  for (int i = 0; i < note_count; i++) {
    notes[i].color_slot = 0;

    int lane = notes[i].lane;
    if (lane < 0 || lane >= game_ring_count) {
      continue;
    }

    int previous_note = last_note_for_lane[lane];
    if (previous_note >= 0 && notes[i].time_ms - notes[previous_note].time_ms <= close_note_gap_ms) {
      if (notes[previous_note].color_slot == 0) {
        notes[previous_note].color_slot = 1;
      }

      if (notes[previous_note].color_slot == 1) {
        notes[i].color_slot = 2;
      } else {
        notes[i].color_slot = 1;
      }
    }

    last_note_for_lane[lane] = i;
  }

  for (int i = 0; i < note_count; i++) {
    if (notes[i].color_slot != 0) {
      close_color_note_count++;
    }
  }
}

void load_song_chart() {
  const SongChart& chart = song_charts[active_chart_index];

  note_count = 0;
  first_pending_note = 0;
  close_color_note_count = 0;
  reset_performance_stats();

  for (int i = 0; i < chart.note_count && i < max_notes; i++) {
    notes[note_count++] = {
      chart.chart_notes[i].time_ms,
      chart.chart_notes[i].lane,
      0,
      false,
      false,
    };
  }

  assign_note_color_slots();
  song_end_time_ms = chart.song_end_ms;
}

long current_song_ms() {
  if (game_mode == mode_finished) {
    return song_end_time_ms;
  }

  if (game_mode == mode_playing) {
    return (long)(millis() - game_start_time);
  }

  return 0;
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
  int flash_ms = 90;
  if (result == result_miss) {
    flash_ms = 65;
  }

  flash_result[lane] = result;
  flash_until_time[lane] = millis() + flash_ms;
  request_led_draw();
}

int score_for_result(int result) {
  if (result == result_perfect) {
    return 100;
  }

  if (result == result_great) {
    return 90;
  }

  if (result == result_good) {
    return 75;
  }

  if (result == result_bad) {
    return 50;
  }

  return 0;
}

const char* result_name(int result) {
  if (result == result_perfect) {
    return "perfect";
  }

  if (result == result_great) {
    return "great";
  }

  if (result == result_good) {
    return "good";
  }

  if (result == result_bad) {
    return "bad";
  }

  return "miss";
}

int result_from_delta(int abs_delta) {
  if (abs_delta <= perfect_window_ms) {
    return result_perfect;
  }

  if (abs_delta <= great_window_ms) {
    return result_great;
  }

  if (abs_delta <= good_window_ms) {
    return result_good;
  }

  if (abs_delta <= bad_window_ms) {
    return result_bad;
  }

  return result_miss;
}

void record_note_result(int result, int abs_delta_ms) {
  game_judged_notes += 1;

  if (result == result_miss) {
    game_missed_notes += 1;
    return;
  }

  game_hit_notes += 1;
  game_score += score_for_result(result);
  game_abs_delta_total_ms += abs_delta_ms;

  if (result == result_perfect) {
    game_perfect_notes += 1;
  } else if (result == result_great) {
    game_great_notes += 1;
  } else if (result == result_good) {
    game_good_notes += 1;
  } else if (result == result_bad) {
    game_bad_notes += 1;
  }
}

int find_best_note_for_lane(int lane, long song_ms, int* best_abs_delta) {
  int best_note = -1;
  int local_best_abs_delta = 9999;

  advance_first_pending_note();

  for (int i = first_pending_note; i < note_count; i++) {
    if (notes[i].lane != lane) {
      continue;
    }

    if (notes[i].was_hit || notes[i].was_missed) {
      continue;
    }

    int delta = song_ms - notes[i].time_ms;
    if (delta < -bad_window_ms) {
      break;
    }

    int abs_delta = abs(delta);

    if (abs_delta < local_best_abs_delta && abs_delta <= bad_window_ms) {
      local_best_abs_delta = abs_delta;
      best_note = i;
    }
  }

  if (best_abs_delta != nullptr) {
    *best_abs_delta = local_best_abs_delta;
  }

  return best_note;
}

bool can_accept_game_hit(int lane, unsigned long now) {
  if (now >= next_allowed_game_hit_time[lane]) {
    return true;
  }

  if (game_mode != mode_playing) {
    return false;
  }

  int ignored_delta = 9999;
  return find_best_note_for_lane(lane, current_song_ms(), &ignored_delta) >= 0;
}

int next_same_lane_note_in_ms(int lane, long song_ms) {
  advance_first_pending_note();

  for (int i = first_pending_note; i < note_count; i++) {
    if (notes[i].was_hit || notes[i].was_missed) {
      continue;
    }

    int delta = notes[i].time_ms - song_ms;
    if (delta < -bad_window_ms) {
      continue;
    }

    if (notes[i].lane == lane) {
      return delta;
    }

    if (delta > game_hit_lockout_ms) {
      break;
    }
  }

  return -1;
}

bool handle_hit(int lane) {
  long song_ms = current_song_ms();
  int best_abs_delta = 9999;
  int best_note = find_best_note_for_lane(lane, song_ms, &best_abs_delta);

  if (best_note == -1) {
    game_combo = 0;
    game_extra_hits += 1;
    last_note_result = result_miss;
    last_note_lane = lane;
    last_note_delta_ms = 0;
    last_note_result_time = millis();
    start_lane_flash(lane, result_miss);
    Serial.print("GAME HIT lane=");
    Serial.print(lane);
    Serial.print(" name=");
    Serial.print(lane_name_for_game(lane));
    Serial.print(" local_ms=");
    Serial.print(song_ms);
    Serial.print(" result=miss delta_ms=none score=");
    Serial.print(chart_score_percent());
    Serial.print(" combo=");
    Serial.print(game_combo);
    Serial.print(" extras=");
    Serial.print(game_extra_hits);
    Serial.print(" distance_cm=");
    Serial.println((int)game_sensor_cm[lane]);
    send_score_line();
    request_oled_draw();
    return false;
  }

  int result = result_from_delta(best_abs_delta);
  notes[best_note].was_hit = true;
  advance_first_pending_note();
  record_note_result(result, best_abs_delta);
  game_combo += 1;
  if (game_combo > game_max_combo) {
    game_max_combo = game_combo;
  }
  last_note_result = result;
  last_note_lane = lane;
  last_note_delta_ms = song_ms - notes[best_note].time_ms;
  last_note_result_time = millis();
  start_lane_flash(lane, result);

  Serial.print("GAME HIT lane=");
  Serial.print(lane);
  Serial.print(" name=");
  Serial.print(lane_name_for_game(lane));
  Serial.print(" local_ms=");
  Serial.print(song_ms);
  Serial.print(" note_ms=");
  Serial.print(notes[best_note].time_ms);
  Serial.print(" result=");
  Serial.print(result_name(result));
  Serial.print(" delta_ms=");
  Serial.print(song_ms - notes[best_note].time_ms);
  Serial.print(" score=");
  Serial.print(chart_score_percent());
  Serial.print(" quality_pct=");
  Serial.print(judged_quality_percent());
  Serial.print(" combo=");
  Serial.print(game_combo);
  Serial.print(" max_combo=");
  Serial.print(game_max_combo);
  Serial.print(" distance_cm=");
  Serial.println((int)game_sensor_cm[lane]);
  send_score_line();
  request_oled_draw();
  return true;
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

  if (inside && !sensor.inZone && can_accept_game_hit(lane, now) &&
      (now - sensor.lastTriggerMs >= DEBOUNCE_MS)) {
    sensor.lastTriggerMs = now;
    play_local_drum_for_lane(lane);

    bool hit_chart_note = false;
    if (game_mode == mode_playing) {
      hit_chart_note = handle_hit(lane);
    } else {
      Serial.print("GAME PAD lane=");
      Serial.print(lane);
      Serial.print(" name=");
      Serial.print(lane_name_for_game(lane));
      Serial.print(" mode=");
      Serial.print(mode_name_for_game());
      Serial.print(" distance_cm=");
      Serial.println((int)game_sensor_cm[lane]);
    }

    int lockout_ms = game_hit_lockout_ms;
    if (hit_chart_note) {
      int next_note_in_ms = next_same_lane_note_in_ms(lane, current_song_ms());
      if (next_note_in_ms >= 0 && next_note_in_ms < game_hit_lockout_ms) {
        lockout_ms = quick_note_lockout_ms;
      }
    }

    next_allowed_game_hit_time[lane] = now + lockout_ms;
  }

  sensor.inZone = inside;
}

void check_for_misses() {
  if (game_mode != mode_playing) {
    return;
  }

  long song_ms = current_song_ms();

  advance_first_pending_note();

  for (int i = first_pending_note; i < note_count; i++) {
    if (notes[i].was_hit || notes[i].was_missed) {
      continue;
    }

    if (song_ms > notes[i].time_ms + miss_window_ms) {
      notes[i].was_missed = true;
      record_note_result(result_miss, 0);
      game_combo = 0;
      last_note_result = result_miss;
      last_note_lane = notes[i].lane;
      last_note_delta_ms = song_ms - notes[i].time_ms;
      last_note_result_time = millis();
      start_lane_flash(notes[i].lane, result_miss);
      Serial.print("GAME MISS lane=");
      Serial.print(notes[i].lane);
      Serial.print(" name=");
      Serial.print(lane_name_for_game(notes[i].lane));
      Serial.print(" note_ms=");
      Serial.print(notes[i].time_ms);
      Serial.print(" song_ms=");
      Serial.print(song_ms);
      Serial.print(" score=");
      Serial.print(chart_score_percent());
      Serial.print(" combo=");
      Serial.print(game_combo);
      Serial.print(" misses=");
      Serial.println(game_missed_notes);
      send_score_line();
      request_oled_draw();
    } else {
      break;
    }
  }

  advance_first_pending_note();
}

void draw_game_hit_zone(int lane) {
  int center = game_hit_pixel[lane];
  add_game_pixel_color(game_real_pixel(lane, center), game_color(45, 45, 45));
  add_game_pixel_color(game_real_pixel(lane, center - 1), game_color(10, 10, 10));
  add_game_pixel_color(game_real_pixel(lane, center + 1), game_color(10, 10, 10));
}

void draw_game_note(int lane, float pixel_pos, uint32_t color) {
  int center = floor_float_to_int(pixel_pos);

  for (int pixel = center - 3; pixel <= center + 3; pixel++) {
    int percent = note_glow_percent(pixel_pos - (float)pixel);
    if (percent > 0) {
      add_game_pixel_color(game_real_pixel(lane, pixel), scale_game_color(color, percent));
    }
  }
}

void draw_game_flash(int lane) {
  unsigned long now = millis();
  if (flash_until_time[lane] == 0 || now > flash_until_time[lane]) {
    return;
  }

  uint32_t color = game_color(255, 0, 0);
  int center_percent = 45;
  int side_percent = 18;

  if (flash_result[lane] == result_perfect) {
    color = game_color(255, 255, 255);
    center_percent = 100;
    side_percent = 50;
  } else if (flash_result[lane] == result_great) {
    color = game_color(0, 255, 100);
    center_percent = 85;
    side_percent = 40;
  } else if (flash_result[lane] == result_good) {
    color = game_color(0, 90, 255);
    center_percent = 70;
    side_percent = 32;
  } else if (flash_result[lane] == result_bad) {
    color = game_color(255, 120, 0);
    center_percent = 55;
    side_percent = 25;
  }

  int center = game_hit_pixel[lane];
  add_game_pixel_color(game_real_pixel(lane, center), scale_game_color(color, center_percent));
  add_game_pixel_color(game_real_pixel(lane, center - 1), scale_game_color(color, side_percent));
  add_game_pixel_color(game_real_pixel(lane, center + 1), scale_game_color(color, side_percent));
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
  if (!game_leds_enabled) {
    return;
  }

  unsigned long now = millis();
  bool visual_is_moving = game_mode == mode_countdown || game_mode == mode_playing;

  for (int lane = 0; lane < game_ring_count; lane++) {
    if (flash_until_time[lane] > 0 && now > flash_until_time[lane]) {
      flash_until_time[lane] = 0;
      request_led_draw();
    }
  }

  if (!game_led_dirty && !visual_is_moving) {
    return;
  }

  if (now - last_game_led_draw_time < (unsigned long)game_led_frame_gap_ms) {
    return;
  }

  last_game_led_draw_time = now;
  game_led_dirty = false;

  game_pixels.clear();

  if (game_mode == mode_countdown) {
    render_countdown();
  }

  if (game_mode == mode_playing) {
    long song_ms = current_song_ms();
    int visible_notes_for_lane[game_ring_count] = {0, 0, 0};
    advance_first_pending_note();

    for (int i = first_pending_note; i < note_count; i++) {
      if (notes[i].was_hit || notes[i].was_missed) {
        continue;
      }

      long note_ms = notes[i].time_ms;
      long appear_ms = note_ms - game_fall_time_ms;
      long draw_until_ms = note_ms + miss_window_ms;

      if (song_ms < appear_ms) {
        break;
      }

      if (song_ms > draw_until_ms) {
        continue;
      }

      int lane = notes[i].lane;
      if (lane < 0 || lane >= game_ring_count) {
        continue;
      }

      if (visible_notes_for_lane[lane] >= max_visible_notes_per_lane) {
        continue;
      }
      visible_notes_for_lane[lane]++;

      long display_ms = song_ms;
      if (display_ms > note_ms) {
        display_ms = note_ms;
      }

      float progress = (float)(display_ms - appear_ms) / (float)game_fall_time_ms;
      float start_pixel = (float)(game_hit_pixel[lane] - game_note_travel_pixels);
      float pixel_pos = start_pixel + progress * game_note_travel_pixels;
      uint32_t note_color = scale_game_color(note_color_for_game(lane, notes[i].color_slot), note_brightness_percent(progress));
      draw_game_note(lane, pixel_pos, note_color);
    }
  }

  for (int lane = 0; lane < game_ring_count; lane++) {
    draw_game_flash(lane);
  }

  for (int lane = 0; lane < game_ring_count; lane++) {
    draw_game_hit_zone(lane);
  }

  game_pixels.show();
}

void format_song_time(long ms, char* output, int output_size) {
  if (ms < 0) {
    ms = 0;
  }

  int seconds = (int)(ms / 1000);
  snprintf(output, output_size, "%d:%02d", seconds / 60, seconds % 60);
}

const char* short_lane_name(int lane) {
  if (lane == lane_hihat) {
    return "hat";
  }

  if (lane == lane_kick) {
    return "kick";
  }

  if (lane == lane_snare) {
    return "snare";
  }

  return "-";
}

void setup_oled_status() {
  if (!game_oled_enabled) {
    return;
  }

  game_oled.init();
  game_oled.flipScreenVertically();
  game_oled.clear();
  game_oled.setColor(WHITE);
  game_oled.setTextAlignment(TEXT_ALIGN_LEFT);
  game_oled.setFont(ArialMT_Plain_10);
  game_oled.drawString(0, 0, "air drums");
  game_oled.drawString(0, 16, "loading chart...");
  game_oled.display();
}

void draw_oled_status(bool force_draw) {
  if (!game_oled_enabled) {
    return;
  }

  unsigned long now = millis();
  if (!force_draw && !game_oled_dirty && now - last_oled_draw_time < 150) {
    return;
  }

  if (!force_draw && now - last_oled_draw_time < 150) {
    return;
  }
  last_oled_draw_time = now;
  game_oled_dirty = false;

  long song_ms = current_song_ms();
  char elapsed_text[8];
  char duration_text[8];
  char line[32];

  format_song_time(song_ms, elapsed_text, sizeof(elapsed_text));
  format_song_time(song_end_time_ms, duration_text, sizeof(duration_text));

  int progress_pixels = 0;
  if (song_end_time_ms > 0 && song_ms > 0) {
    progress_pixels = (int)((song_ms * 126L) / song_end_time_ms);
    if (progress_pixels > 126) {
      progress_pixels = 126;
    }
  }

  game_oled.clear();
  game_oled.setColor(WHITE);
  game_oled.setFont(ArialMT_Plain_10);
  game_oled.setTextAlignment(TEXT_ALIGN_LEFT);
  game_oled.drawString(0, 0, mode_name_for_game());

  snprintf(line, sizeof(line), "%s/%s", elapsed_text, duration_text);
  game_oled.setTextAlignment(TEXT_ALIGN_RIGHT);
  game_oled.drawString(128, 0, line);

  game_oled.drawRect(0, 12, 128, 6);
  if (progress_pixels > 0) {
    game_oled.fillRect(1, 13, progress_pixels, 4);
  }

  game_oled.setTextAlignment(TEXT_ALIGN_LEFT);
  snprintf(line, sizeof(line), "score %d%%", chart_score_percent());
  game_oled.drawString(0, 21, line);

  snprintf(line, sizeof(line), "combo %d", game_combo);
  game_oled.drawString(74, 21, line);

  if (game_mode == mode_countdown && scheduled_song_start_time > now) {
    snprintf(line, sizeof(line), "starts in %lu.%01lu",
             (scheduled_song_start_time - now) / 1000,
             ((scheduled_song_start_time - now) % 1000) / 100);
    game_oled.drawString(0, 34, line);
  } else if (last_note_result_time > 0) {
    snprintf(line, sizeof(line), "%s %s %+dms",
             short_lane_name(last_note_lane),
             result_name(last_note_result),
             last_note_delta_ms);
    game_oled.drawString(0, 34, line);
  } else {
    game_oled.drawString(0, 34, "last note -");
  }

  int next_note = next_note_index(song_ms);
  if (game_mode == mode_finished) {
    snprintf(line, sizeof(line), "%s hit %d%% avg %dms",
             performance_grade(),
             hit_rate_percent(),
             average_hit_delta_ms());
    game_oled.drawString(0, 47, line);
  } else if (next_note >= 0 && game_mode == mode_playing) {
    snprintf(line, sizeof(line), "next %s in %ldms",
             short_lane_name(notes[next_note].lane),
             notes[next_note].time_ms - song_ms);
    game_oled.drawString(0, 47, line);
  } else if (game_mode == mode_ready) {
    game_oled.drawString(0, 47, active_track_id);
  } else {
    game_oled.drawString(0, 47, active_track_id);
  }

  game_oled.display();
}

void stop_game() {
  game_mode = mode_ready;
  request_led_draw();
  send_board_stop();
  Serial.println("GAME STOP mode=ready");
  send_game_telemetry(true);
  draw_oled_status(true);
}

void start_countdown() {
  game_mode = mode_countdown;
  request_led_draw();
  countdown_start_time = millis();
  scheduled_song_start_time = countdown_start_time + countdown_length_ms;
  reset_performance_stats();
  first_pending_note = 0;
  last_note_result = result_miss;
  last_note_lane = -1;
  last_note_delta_ms = 0;
  last_note_result_time = 0;

  for (int i = 0; i < note_count; i++) {
    notes[i].was_hit = false;
    notes[i].was_missed = false;
  }

  for (int lane = 0; lane < game_ring_count; lane++) {
    next_allowed_game_hit_time[lane] = 0;
    flash_until_time[lane] = 0;
  }

  send_board_start(countdown_length_ms);
  Serial.print("GAME COUNTDOWN delay_ms=");
  Serial.print(countdown_length_ms);
  Serial.print(" notes=");
  Serial.print(note_count);
  Serial.print(" song_end_ms=");
  Serial.print(song_end_time_ms);
  Serial.print(" score=");
  Serial.print(chart_score_percent());
  Serial.print(" combo=");
  Serial.println(game_combo);
  send_game_telemetry(true);
  draw_oled_status(true);
}

void update_countdown() {
  if (game_mode != mode_countdown) {
    return;
  }

  if (millis() >= scheduled_song_start_time) {
    game_mode = mode_playing;
    game_start_time = scheduled_song_start_time;
    Serial.print("GAME START song_ms=0 notes=");
    Serial.print(note_count);
    Serial.print(" song_end_ms=");
    Serial.println(song_end_time_ms);
    send_game_telemetry(true);
    draw_oled_status(true);
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
  request_led_draw();
  send_board_stop();
  Serial.print("GAME FINISH song_ms=");
  Serial.print(song_ms);
  Serial.print(" score=");
  Serial.print(chart_score_percent());
  Serial.print(" quality_pct=");
  Serial.print(judged_quality_percent());
  Serial.print(" combo=");
  Serial.print(game_combo);
  Serial.print(" max_combo=");
  Serial.print(game_max_combo);
  Serial.print(" hits=");
  Serial.print(game_hit_notes);
  Serial.print(" misses=");
  Serial.print(game_missed_notes);
  Serial.print(" extras=");
  Serial.print(game_extra_hits);
  Serial.print(" perfect=");
  Serial.print(game_perfect_notes);
  Serial.print(" great=");
  Serial.print(game_great_notes);
  Serial.print(" good=");
  Serial.print(game_good_notes);
  Serial.print(" bad=");
  Serial.print(game_bad_notes);
  Serial.print(" hit_rate_pct=");
  Serial.print(hit_rate_percent());
  Serial.print(" avg_delta_ms=");
  Serial.print(average_hit_delta_ms());
  Serial.print(" grade=");
  Serial.println(performance_grade());
  send_game_telemetry(true);
  draw_oled_status(true);
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

void print_chart_list() {
  Serial.println("GAME CHARTS");
  for (int i = 0; i < song_chart_count; i++) {
    Serial.print(i == active_chart_index ? "* " : "  ");
    Serial.print(i + 1);
    Serial.print(": ");
    Serial.print(song_charts[i].track_id);
    Serial.print(" notes=");
    Serial.print(song_charts[i].note_count);
    Serial.print(" song_end_ms=");
    Serial.println(song_charts[i].song_end_ms);
  }
}

void announce_chart_selection() {
  Serial.print("GAME CHART selected=");
  Serial.print(active_track_id);
  Serial.print(" index=");
  Serial.print(active_chart_index + 1);
  Serial.print("/");
  Serial.print(song_chart_count);
  Serial.print(" notes=");
  Serial.print(note_count);
  Serial.print(" song_end_ms=");
  Serial.println(song_end_time_ms);
}

bool can_change_chart() {
  if (game_mode == mode_countdown || game_mode == mode_playing) {
    Serial.println("GAME CHART busy stop first");
    return false;
  }

  return true;
}

void select_chart_index(int next_index) {
  if (!can_change_chart()) {
    return;
  }

  while (next_index < 0) {
    next_index += song_chart_count;
  }
  while (next_index >= song_chart_count) {
    next_index -= song_chart_count;
  }

  active_chart_index = next_index;
  copy_active_track_id();
  load_song_chart();
  game_mode = mode_ready;
  pc_player_ready = false;
  ready_track_id[0] = '\0';
  request_led_draw();
  request_oled_draw();
  announce_player_state();
  announce_chart_selection();
  send_game_telemetry(true);
  draw_oled_status(true);
}

void select_chart_by_name_or_number(const char* value) {
  while (*value == ' ' || *value == '\t') {
    value++;
  }

  int chart_number = atoi(value);
  if (chart_number > 0 && chart_number <= song_chart_count) {
    select_chart_index(chart_number - 1);
    return;
  }

  for (int i = 0; i < song_chart_count; i++) {
    if (strcmp(value, song_charts[i].track_id) == 0) {
      select_chart_index(i);
      return;
    }
  }

  Serial.print("GAME CHART unknown=");
  Serial.println(value);
  print_chart_list();
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

  if (strcmp(line, "n") == 0 || strcmp(line, "HOST NEXT_CHART") == 0) {
    select_chart_index(active_chart_index + 1);
    return;
  }

  if (strcmp(line, "b") == 0 || strcmp(line, "HOST PREV_CHART") == 0) {
    select_chart_index(active_chart_index - 1);
    return;
  }

  if (strcmp(line, "charts") == 0 || strcmp(line, "HOST LIST_CHARTS") == 0) {
    print_chart_list();
    return;
  }

  if (starts_with(line, "chart ")) {
    select_chart_by_name_or_number(line + 6);
    return;
  }

  if (starts_with(line, "HOST SELECT_CHART ")) {
    select_chart_by_name_or_number(line + 18);
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
  setup_oled_status();

  if (game_leds_enabled) {
    game_pixels.begin();
    game_pixels.setBrightness(game_brightness);
    game_pixels.clear();
    game_pixels.show();
  }

  copy_active_track_id();
  load_song_chart();
  game_mode = mode_ready;

  announce_player_state();
  announce_chart_selection();
  Serial.print("GAME READY notes=");
  Serial.print(note_count);
  Serial.print(" song_end_ms=");
  Serial.print(song_end_time_ms);
  Serial.print(" close_color_notes=");
  Serial.print(close_color_note_count);
  Serial.print(" leds_enabled=");
  Serial.println(game_leds_enabled ? 1 : 0);
  Serial.println("game ready");
  Serial.println("commands: s=start, x=stop, n=next chart, b=previous chart, charts=list, chart <id|number>");
  Serial.println("host commands: HOST START_GAME, HOST STOP_GAME, HOST REQUEST_STATE, HOST NEXT_CHART, HOST PREV_CHART, HOST SELECT_CHART <id|number>");
  send_game_telemetry(true);
  draw_oled_status(true);
}

void loop_game_logic() {
  handle_game_serial_input();
  update_countdown();
  update_sensors();

  if (game_mode == mode_countdown || game_mode == mode_playing) {
    send_game_telemetry(false);
  }

  if (game_mode == mode_playing) {
    check_for_misses();
    finish_song_if_needed();
  }

  render_game();
  draw_oled_status(false);
}
