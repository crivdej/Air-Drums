#include <Arduino.h>
#include <Adafruit_NeoPixel.h>
#include "air_drums_shared.h"
#include "kick.h"
#include "rhythm_game.h"
#include "snare.h"
#include "hihat.h"

// This branch is for chasing NeoPixel ring wiring/data/power issues.
#define NEOPIXEL_TEST_MODE 0
#define RHYTHM_GAME_MODE 1

// ── neopixel ring test ──//
const int led_data_pin = 23;
const int ring_count = 3;
const int leds_per_ring = 16;
const int total_leds = ring_count * leds_per_ring;
const int led_serial_buffer_size = 96;

int brightness = 35;
int hit_pixel[ring_count] = {0};
int ring_offset[ring_count] = {0};

int moving_pixel = 0;
int selected_ring = -1; // -1 means all rings
unsigned long last_move_time = 0;
int move_gap_ms = 90;
bool auto_animate = true;

uint32_t lane_color[ring_count];
char led_serial_buffer[led_serial_buffer_size];
int led_serial_buffer_len = 0;

Adafruit_NeoPixel pixels(total_leds, led_data_pin, NEO_GRB + NEO_KHZ800);

int wrap_pixel(int pixel) {
  while (pixel < 0) {
    pixel = pixel + leds_per_ring;
  }

  while (pixel >= leds_per_ring) {
    pixel = pixel - leds_per_ring;
  }

  return pixel;
}

int get_real_pixel(int lane, int logical_pixel) {
  int fixed_pixel = wrap_pixel(logical_pixel + ring_offset[lane]);
  return lane * leds_per_ring + fixed_pixel;
}

uint32_t make_color_smaller(uint32_t color, int percent) {
  uint8_t red = (uint8_t)(color >> 16);
  uint8_t green = (uint8_t)(color >> 8);
  uint8_t blue = (uint8_t)(color);

  red = (red * percent) / 100;
  green = (green * percent) / 100;
  blue = (blue * percent) / 100;

  return pixels.Color(red, green, blue);
}

void add_pixel_color(int pixel_number, uint32_t color) {
  uint32_t old_color = pixels.getPixelColor(pixel_number);

  uint8_t old_red = (uint8_t)(old_color >> 16);
  uint8_t old_green = (uint8_t)(old_color >> 8);
  uint8_t old_blue = (uint8_t)(old_color);

  uint8_t new_red = (uint8_t)(color >> 16);
  uint8_t new_green = (uint8_t)(color >> 8);
  uint8_t new_blue = (uint8_t)(color);

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

  pixels.setPixelColor(pixel_number, pixels.Color(mixed_red, mixed_green, mixed_blue));
}

void draw_hit_zone(int lane) {
  int center = hit_pixel[lane];

  add_pixel_color(get_real_pixel(lane, center), pixels.Color(25, 25, 25));
  add_pixel_color(get_real_pixel(lane, center - 1), pixels.Color(8, 8, 8));
  add_pixel_color(get_real_pixel(lane, center + 1), pixels.Color(8, 8, 8));
}

void draw_glow_note(int lane, int center_pixel) {
  uint32_t color = lane_color[lane];

  add_pixel_color(get_real_pixel(lane, center_pixel - 2), make_color_smaller(color, 15));
  add_pixel_color(get_real_pixel(lane, center_pixel - 1), make_color_smaller(color, 45));
  add_pixel_color(get_real_pixel(lane, center_pixel), make_color_smaller(color, 100));
  add_pixel_color(get_real_pixel(lane, center_pixel + 1), make_color_smaller(color, 45));
  add_pixel_color(get_real_pixel(lane, center_pixel + 2), make_color_smaller(color, 15));
}

void draw_ring_numbers_once() {
  pixels.clear();

  for (int lane = 0; lane < ring_count; lane++) {
    for (int i = 0; i < leds_per_ring; i++) {
      if (i == hit_pixel[lane]) {
        pixels.setPixelColor(get_real_pixel(lane, i), pixels.Color(40, 40, 40));
      } else if (i % 4 == 0) {
        pixels.setPixelColor(get_real_pixel(lane, i), pixels.Color(0, 0, 12));
      }
    }
  }

  pixels.show();
}

void draw_frame() {
  pixels.clear();

  for (int lane = 0; lane < ring_count; lane++) {
    draw_hit_zone(lane);
  }

  if (selected_ring == -1) {
    for (int lane = 0; lane < ring_count; lane++) {
      draw_glow_note(lane, moving_pixel);
    }
  } else {
    draw_glow_note(selected_ring, moving_pixel);
  }

  pixels.show();
}

int clamp_int(int value, int low, int high) {
  if (value < low) {
    return low;
  }
  if (value > high) {
    return high;
  }
  return value;
}

bool text_equals(const char* a, const char* b) {
  while (*a != '\0' && *b != '\0') {
    char ca = *a;
    char cb = *b;

    if (ca >= 'a' && ca <= 'z') {
      ca = ca - 'a' + 'A';
    }
    if (cb >= 'a' && cb <= 'z') {
      cb = cb - 'a' + 'A';
    }

    if (ca != cb) {
      return false;
    }

    a++;
    b++;
  }

  return *a == '\0' && *b == '\0';
}

bool starts_with_text(const char* value, const char* prefix) {
  while (*prefix != '\0') {
    char ca = *value;
    char cb = *prefix;

    if (ca >= 'a' && ca <= 'z') {
      ca = ca - 'a' + 'A';
    }
    if (cb >= 'a' && cb <= 'z') {
      cb = cb - 'a' + 'A';
    }

    if (ca != cb) {
      return false;
    }

    value++;
    prefix++;
  }

  return true;
}

const char* skip_spaces(const char* value) {
  while (*value == ' ' || *value == '\t') {
    value++;
  }
  return value;
}

bool parse_target(const char* token, int* target) {
  if (text_equals(token, "ALL") || text_equals(token, "A")) {
    *target = -1;
    return true;
  }

  int ring = -99;
  if (sscanf(token, "%d", &ring) != 1) {
    return false;
  }

  if (ring < 0 || ring >= ring_count) {
    return false;
  }

  *target = ring;
  return true;
}

void print_led_ok(const char* command) {
  Serial.print("LED OK ");
  Serial.println(command);
}

void print_led_error(const char* message) {
  Serial.print("LED ERR ");
  Serial.println(message);
}

void set_ring_color(int lane, uint32_t color) {
  for (int i = 0; i < leds_per_ring; i++) {
    pixels.setPixelColor(get_real_pixel(lane, i), color);
  }
}

void set_target_color(int target, uint32_t color) {
  if (target == -1) {
    for (int lane = 0; lane < ring_count; lane++) {
      set_ring_color(lane, color);
    }
    return;
  }

  set_ring_color(target, color);
}

uint32_t named_color(const char* name) {
  if (text_equals(name, "RED") || text_equals(name, "R")) {
    return pixels.Color(80, 0, 0);
  }
  if (text_equals(name, "GREEN") || text_equals(name, "G")) {
    return pixels.Color(0, 80, 0);
  }
  if (text_equals(name, "BLUE") || text_equals(name, "B")) {
    return pixels.Color(0, 0, 80);
  }
  if (text_equals(name, "WHITE") || text_equals(name, "W")) {
    return pixels.Color(45, 45, 45);
  }
  if (text_equals(name, "YELLOW") || text_equals(name, "Y")) {
    return pixels.Color(70, 50, 0);
  }
  if (text_equals(name, "PURPLE") || text_equals(name, "P")) {
    return pixels.Color(50, 0, 70);
  }
  if (text_equals(name, "CYAN") || text_equals(name, "C")) {
    return pixels.Color(0, 60, 60);
  }
  if (text_equals(name, "OFF") || text_equals(name, "BLACK")) {
    return pixels.Color(0, 0, 0);
  }

  return 0xffffffff;
}

void print_led_status() {
  Serial.print("LED STATUS pin=");
  Serial.print(led_data_pin);
  Serial.print(" rings=");
  Serial.print(ring_count);
  Serial.print(" leds_per_ring=");
  Serial.print(leds_per_ring);
  Serial.print(" total_leds=");
  Serial.print(total_leds);
  Serial.print(" brightness=");
  Serial.print(brightness);
  Serial.print(" auto=");
  Serial.print(auto_animate ? 1 : 0);
  Serial.print(" selected_ring=");
  Serial.print(selected_ring);
  Serial.print(" move_gap_ms=");
  Serial.print(move_gap_ms);
  Serial.print(" moving_pixel=");
  Serial.println(moving_pixel);

  for (int lane = 0; lane < ring_count; lane++) {
    Serial.print("LED RING lane=");
    Serial.print(lane);
    Serial.print(" first_pixel=");
    Serial.print(lane * leds_per_ring);
    Serial.print(" last_pixel=");
    Serial.print((lane + 1) * leds_per_ring - 1);
    Serial.print(" hit_pixel=");
    Serial.print(hit_pixel[lane]);
    Serial.print(" offset=");
    Serial.println(ring_offset[lane]);
  }
}

void print_led_test_help() {
  Serial.println("LED HELP commands:");
  Serial.println("  HELP or ?                 print commands");
  Serial.println("  STATUS or p               print diagnostics");
  Serial.println("  AUTO ON|OFF or a          toggle/run animation");
  Serial.println("  RING ALL|0|1|2            select animated ring");
  Serial.println("  SPEED <ms>, +, -          set animation frame gap");
  Serial.println("  BRIGHTNESS <0-255>        set strip brightness");
  Serial.println("  COLOR <name> [ring|ALL]   show named color");
  Serial.println("  FILL <ring|ALL> <r> <g> <b>");
  Serial.println("  PIXEL <ring> <pixel> <r> <g> <b>");
  Serial.println("  MARK or h                 show hit pixels and quarter marks");
  Serial.println("  WIRING                    ring 0 red, ring 1 green, ring 2 blue");
  Serial.println("  WALK [ring|ALL] [ms]      light each pixel in chain order");
  Serial.println("  CHASE [cycles] [ms]       moving dot on selected/all rings");
  Serial.println("  CLEAR or x                turn all LEDs off");
  Serial.println("  POWER [brightness]        all white power sanity check");
}

void print_led_test_settings() {
  Serial.println();
  Serial.println("air drum tiles neopixel test");
  print_led_status();
  print_led_test_help();
  Serial.println();
}

void run_wiring_test() {
  auto_animate = false;
  pixels.clear();
  set_ring_color(0, pixels.Color(80, 0, 0));
  set_ring_color(1, pixels.Color(0, 80, 0));
  set_ring_color(2, pixels.Color(0, 0, 80));
  pixels.show();
}

void run_walk_test(int target, int delay_ms) {
  auto_animate = false;
  delay_ms = clamp_int(delay_ms, 10, 1000);

  int first_ring = target == -1 ? 0 : target;
  int last_ring = target == -1 ? ring_count - 1 : target;

  Serial.print("LED TEST WALK start target=");
  Serial.print(target);
  Serial.print(" delay_ms=");
  Serial.println(delay_ms);

  for (int lane = first_ring; lane <= last_ring; lane++) {
    for (int i = 0; i < leds_per_ring; i++) {
      pixels.clear();
      pixels.setPixelColor(get_real_pixel(lane, i), lane_color[lane]);
      pixels.show();

      Serial.print("LED STEP ring=");
      Serial.print(lane);
      Serial.print(" logical_pixel=");
      Serial.print(i);
      Serial.print(" physical_pixel=");
      Serial.println(get_real_pixel(lane, i));

      delay(delay_ms);
    }
  }

  Serial.println("LED TEST WALK done");
}

void run_chase_test(int cycles, int delay_ms) {
  auto_animate = false;
  cycles = clamp_int(cycles, 1, 100);
  delay_ms = clamp_int(delay_ms, 10, 1000);

  Serial.print("LED TEST CHASE start cycles=");
  Serial.print(cycles);
  Serial.print(" delay_ms=");
  Serial.println(delay_ms);

  for (int cycle = 0; cycle < cycles; cycle++) {
    for (int pixel = 0; pixel < leds_per_ring; pixel++) {
      moving_pixel = pixel;
      draw_frame();
      delay(delay_ms);
    }
  }

  Serial.println("LED TEST CHASE done");
}

void process_led_test_line(const char* raw_line) {
  const char* line = skip_spaces(raw_line);

  if (line[0] == '\0') {
    return;
  }

  if (text_equals(line, "HELP") || text_equals(line, "?")) {
    print_led_test_help();
    print_led_ok("HELP");
    return;
  }

  if (text_equals(line, "STATUS") || text_equals(line, "p")) {
    print_led_status();
    print_led_ok("STATUS");
    return;
  }

  if (text_equals(line, "a")) {
    auto_animate = true;
    selected_ring = -1;
    print_led_ok("AUTO ON");
    return;
  }

  if ((line[0] == '0' || line[0] == '1' || line[0] == '2') && line[1] == '\0') {
    selected_ring = line[0] - '0';
    auto_animate = true;
    print_led_ok("RING");
    return;
  }

  if (text_equals(line, "+")) {
    move_gap_ms = move_gap_ms - 10;
    if (move_gap_ms < 20) {
      move_gap_ms = 20;
    }
    Serial.print("move_gap_ms=");
    Serial.println(move_gap_ms);
    print_led_ok("SPEED");
    return;
  }

  if (text_equals(line, "-")) {
    move_gap_ms = move_gap_ms + 10;
    if (move_gap_ms > 500) {
      move_gap_ms = 500;
    }
    Serial.print("move_gap_ms=");
    Serial.println(move_gap_ms);
    print_led_ok("SPEED");
    return;
  }

  if (text_equals(line, "h") || text_equals(line, "MARK")) {
    auto_animate = false;
    draw_ring_numbers_once();
    print_led_ok("MARK");
    return;
  }

  if (text_equals(line, "x") || text_equals(line, "CLEAR")) {
    auto_animate = false;
    pixels.clear();
    pixels.show();
    print_led_ok("CLEAR");
    return;
  }

  if (text_equals(line, "WIRING")) {
    run_wiring_test();
    print_led_ok("WIRING");
    return;
  }

  char word1[16] = "";
  char word2[16] = "";
  char word3[16] = "";
  int n1 = 0;
  int n2 = 0;
  int n3 = 0;
  int n4 = 0;
  int n5 = 0;

  if (sscanf(line, "%15s %15s", word1, word2) >= 1 && text_equals(word1, "AUTO")) {
    if (text_equals(word2, "ON")) {
      auto_animate = true;
      print_led_ok("AUTO ON");
      return;
    }
    if (text_equals(word2, "OFF")) {
      auto_animate = false;
      print_led_ok("AUTO OFF");
      return;
    }
    print_led_error("AUTO expects ON or OFF");
    return;
  }

  if (sscanf(line, "%15s %15s", word1, word2) == 2 && text_equals(word1, "RING")) {
    int target = -1;
    if (!parse_target(word2, &target)) {
      print_led_error("RING expects ALL, 0, 1, or 2");
      return;
    }
    selected_ring = target;
    auto_animate = true;
    print_led_ok("RING");
    return;
  }

  if (sscanf(line, "%15s %d", word1, &n1) == 2 && text_equals(word1, "SPEED")) {
    move_gap_ms = clamp_int(n1, 20, 1000);
    print_led_ok("SPEED");
    return;
  }

  if (sscanf(line, "%15s %d", word1, &n1) == 2 && text_equals(word1, "BRIGHTNESS")) {
    brightness = clamp_int(n1, 0, 255);
    pixels.setBrightness(brightness);
    pixels.show();
    print_led_ok("BRIGHTNESS");
    return;
  }

  if (sscanf(line, "%15s %15s %15s", word1, word2, word3) >= 2 && text_equals(word1, "COLOR")) {
    uint32_t color = named_color(word2);
    if (color == 0xffffffff) {
      print_led_error("unknown color");
      return;
    }

    int target = -1;
    if (word3[0] != '\0' && !parse_target(word3, &target)) {
      print_led_error("COLOR target expects ALL, 0, 1, or 2");
      return;
    }

    auto_animate = false;
    pixels.clear();
    set_target_color(target, color);
    pixels.show();
    print_led_ok("COLOR");
    return;
  }

  if (sscanf(line, "%15s %15s %d %d %d", word1, word2, &n1, &n2, &n3) == 5 &&
      text_equals(word1, "FILL")) {
    int target = -1;
    if (!parse_target(word2, &target)) {
      print_led_error("FILL target expects ALL, 0, 1, or 2");
      return;
    }

    auto_animate = false;
    pixels.clear();
    set_target_color(target, pixels.Color(clamp_int(n1, 0, 255), clamp_int(n2, 0, 255), clamp_int(n3, 0, 255)));
    pixels.show();
    print_led_ok("FILL");
    return;
  }

  if (sscanf(line, "%15s %d %d %d %d %d", word1, &n1, &n2, &n3, &n4, &n5) == 6 &&
      text_equals(word1, "PIXEL")) {
    if (n1 < 0 || n1 >= ring_count || n2 < 0 || n2 >= leds_per_ring) {
      print_led_error("PIXEL expects ring 0-2 and pixel 0-15");
      return;
    }

    auto_animate = false;
    pixels.clear();
    pixels.setPixelColor(get_real_pixel(n1, n2), pixels.Color(clamp_int(n3, 0, 255), clamp_int(n4, 0, 255), clamp_int(n5, 0, 255)));
    pixels.show();
    print_led_ok("PIXEL");
    return;
  }

  if (starts_with_text(line, "WALK")) {
    int target = -1;
    int delay_ms = 80;
    int count = sscanf(line, "%15s %15s %d", word1, word2, &n1);
    if (count >= 2) {
      if (!parse_target(word2, &target)) {
        print_led_error("WALK target expects ALL, 0, 1, or 2");
        return;
      }
    }
    if (count == 3) {
      delay_ms = n1;
    }
    run_walk_test(target, delay_ms);
    print_led_ok("WALK");
    return;
  }

  if (starts_with_text(line, "CHASE")) {
    int cycles = 3;
    int delay_ms = 65;
    int count = sscanf(line, "%15s %d %d", word1, &n1, &n2);
    if (count >= 2) {
      cycles = n1;
    }
    if (count >= 3) {
      delay_ms = n2;
    }
    run_chase_test(cycles, delay_ms);
    print_led_ok("CHASE");
    return;
  }

  if (starts_with_text(line, "POWER")) {
    int power_brightness = 45;
    if (sscanf(line, "%15s %d", word1, &n1) == 2) {
      power_brightness = n1;
    }

    auto_animate = false;
    brightness = clamp_int(power_brightness, 0, 120);
    pixels.setBrightness(brightness);
    pixels.clear();
    set_target_color(-1, pixels.Color(90, 90, 90));
    pixels.show();
    Serial.print("LED POWER brightness=");
    Serial.print(brightness);
    Serial.println(" expected=all_rings_even_white watch_for=brownout,flicker,yellowing,end_dropoff");
    print_led_ok("POWER");
    return;
  }

  print_led_error("unknown command; send HELP");
}

void handle_led_test_serial() {
  while (Serial.available() > 0) {
    char incoming = (char)Serial.read();

    if (incoming == '\r' || incoming == '\n') {
      if (led_serial_buffer_len > 0) {
        led_serial_buffer[led_serial_buffer_len] = '\0';
        process_led_test_line(led_serial_buffer);
        led_serial_buffer_len = 0;
      }
      continue;
    }

    if (led_serial_buffer_len < led_serial_buffer_size - 1) {
      led_serial_buffer[led_serial_buffer_len++] = incoming;
    } else {
      led_serial_buffer_len = 0;
      print_led_error("command too long");
    }
  }
}

void setup_led_test() {
  Serial.begin(115200);

  pixels.begin();
  pixels.setBrightness(brightness);
  pixels.clear();
  pixels.show();

  lane_color[0] = pixels.Color(0, 90, 255);
  lane_color[1] = pixels.Color(255, 45, 0);
  lane_color[2] = pixels.Color(120, 0, 255);

  print_led_test_settings();
}

void loop_led_test() {
  handle_led_test_serial();

  unsigned long now = millis();

  if (auto_animate && now - last_move_time >= (unsigned long)move_gap_ms) {
    last_move_time = now;

    // this moves clockwise if the led ring is wired in the usual direction
    // if it looks backwards, flip the ring or we can change this later
    moving_pixel = moving_pixel + 1;
    if (moving_pixel >= leds_per_ring) {
      moving_pixel = 0;
    }

    draw_frame();
  }
}

// ── DAC Environment────//
#define DAC_PIN     26
#define SAMPLE_RATE 16000

constexpr unsigned long sensor_echo_timeout_us = 6000;

volatile const uint8_t* current_sample = nullptr;
volatile uint32_t sample_pos = 0;
volatile uint32_t sample_len = 0;

hw_timer_t* audio_timer = NULL;

void IRAM_ATTR onAudioTimer() {
  if (current_sample == nullptr) return;
  if (sample_pos >= sample_len) {
    current_sample = nullptr;
    dacWrite(DAC_PIN, 128);
    return;
  }
  dacWrite(DAC_PIN, pgm_read_byte(&current_sample[sample_pos]));
  sample_pos++;
}

void playDrum(const uint8_t* data, uint32_t length) {
  noInterrupts();
  current_sample = nullptr;
  sample_pos = 0;
  sample_len = length;
  current_sample = data;
  interrupts();
}

// ── Sensor pins ───//
#define TRIG1 5    // hi-Hat  (built-in)
#define ECHO1 16
#define TRIG2 4    // kick
#define ECHO2 13
#define TRIG3 19   // snare
#define ECHO3 14

DrumSensor sensors[NUM_SENSORS] = {
  { TRIG1, ECHO1, "HI-HAT", hihat_data, hihat_length, false, 0 },
  { TRIG2, ECHO2, "KICK",   kick_data,  kick_length,  false, 0 },
  { TRIG3, ECHO3, "SNARE",  snare_data, snare_length, false, 0 },
};

const char active_track_id[] = "steves_lava_chicken";
const int pc_song_start_delay_ms = 3000;

// this small buffer collects one serial command at a time from the pc player
char music_serial_buffer[64];
int music_serial_buffer_len = 0;

void sendBoardHello() {
  // tell the pc player that the board is online
  Serial.println("BOARD HELLO");
}

void sendBoardLoad() {
  // tell the pc player which backing track file to prepare
  Serial.print("BOARD LOAD ");
  Serial.println(active_track_id);
}

void sendBoardStart() {
  // tell the pc player to start the loaded song after a short delay
  Serial.print("BOARD START ");
  Serial.print(active_track_id);
  Serial.print(" ");
  Serial.println(pc_song_start_delay_ms);
}

void sendBoardStop() {
  Serial.println("BOARD STOP");
}

void announcePcTrack() {
  // on boot or resync, send the minimal state the pc needs
  sendBoardHello();
  sendBoardLoad();
}

void printMusicTestCommands() {
  Serial.println("music test commands:");
  Serial.println("  s = ask the PC to start the song after 3000 ms");
  Serial.println("  x = ask the PC to stop the song");
  Serial.println("  p = resend BOARD HELLO and BOARD LOAD");
}

void processMusicSerialLine(const char* line) {
  if (line[0] == '\0') {
    return;
  }

  if (strcmp(line, "s") == 0 || strcmp(line, "HOST START_GAME") == 0) {
    // start only asks the pc to begin song playback; drum hits stay local on the esp32
    sendBoardStart();
    Serial.println("pc song start requested");
    return;
  }

  if (strcmp(line, "x") == 0 || strcmp(line, "HOST STOP_GAME") == 0) {
    // stop tells the pc to halt backing-track playback
    sendBoardStop();
    Serial.println("pc song stop requested");
    return;
  }

  if (strcmp(line, "p") == 0 || strcmp(line, "HOST REQUEST_STATE") == 0 ||
      strcmp(line, "HOST HELLO") == 0) {
    // resend state so the pc can recover if it connected late or restarted
    announcePcTrack();
    return;
  }
}

void handleMusicSerial() {
  while (Serial.available() > 0) {
    char incoming = (char)Serial.read();

    if (incoming == '\r' || incoming == '\n') {
      if (music_serial_buffer_len > 0) {
        // once we hit a line break, treat the buffered text as one complete command
        music_serial_buffer[music_serial_buffer_len] = '\0';
        processMusicSerialLine(music_serial_buffer);
        music_serial_buffer_len = 0;
      }
      continue;
    }

    if (music_serial_buffer_len < (int)sizeof(music_serial_buffer) - 1) {
      music_serial_buffer[music_serial_buffer_len++] = incoming;
    }
  }
}

// Sensor helper 
long getDistance(int trig, int echo) {
  digitalWrite(trig, LOW);
  delayMicroseconds(2);
  digitalWrite(trig, HIGH);
  delayMicroseconds(10);
  digitalWrite(trig, LOW);
  long duration = pulseIn(echo, HIGH, sensor_echo_timeout_us);
  return duration / 58;
}

// Setup 
void setup() {
  if (NEOPIXEL_TEST_MODE == 1) {
    setup_led_test();
    return;
  }

  Serial.begin(115200);

  for (int i = 0; i < NUM_SENSORS; i++) {
    pinMode(sensors[i].trig, OUTPUT);
    pinMode(sensors[i].echo, INPUT);
  }

  audio_timer = timerBegin(0, 80, true);
  timerAttachInterrupt(audio_timer, &onAudioTimer, true);
  timerAlarmWrite(audio_timer, 1000000 / SAMPLE_RATE, true);
  timerAlarmEnable(audio_timer);

  dacWrite(DAC_PIN, 128);

  if (RHYTHM_GAME_MODE == 1) {
    setup_game_logic();
    return;
  }

  // tell the pc which track to load as soon as the board starts
  announcePcTrack();
  printMusicTestCommands();
}

// ── Main loop ─────────────────────────────────────────────────────────────────
void loop() {
  if (NEOPIXEL_TEST_MODE == 1) {
    loop_led_test();
    return;
  }

  if (RHYTHM_GAME_MODE == 1) {
    loop_game_logic();
    return;
  }

  // listen for simple song-control commands while the board keeps scanning sensors
  handleMusicSerial();

  uint32_t now = millis();

  for (int i = 0; i < NUM_SENSORS; i++) {
    DrumSensor& s = sensors[i];
    long dist = getDistance(s.trig, s.echo);

    if (dist == 0) {
      continue;
    }

    // this is the same simple rising-edge trigger logic for local drum sounds
    bool handPresent = (dist < TRIGGER_CM);

    if (handPresent && !s.inZone && (now - s.lastTriggerMs >= DEBOUNCE_MS)) {
      playDrum(s.sample, s.sampleLen);
      s.lastTriggerMs = now;
      Serial.print(s.name);
      Serial.println(" triggered");
    }

    s.inZone = handPresent;
  }
}

// void loop() {
//   long d1 = getDistance(TRIG1, ECHO1);
//   long d2 = getDistance(TRIG2, ECHO2);
//   long d3 = getDistance(TRIG3, ECHO3);
  
//   Serial.print("HI-HAT: "); Serial.print(d1);
//   Serial.print("  KICK: "); Serial.print(d2);
//   Serial.print("  SNARE: "); Serial.println(d3);
//   delay(200);
// }
