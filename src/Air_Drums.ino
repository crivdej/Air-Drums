#include <Arduino.h>
#include <Adafruit_NeoPixel.h>
#include "kick.h"
#include "snare.h"
#include "hihat.h"

// set this to 0 later when we want the air drum sensors again
#define NEOPIXEL_TEST_MODE 1

// ── neopixel ring test ──//
const int led_data_pin = 23;
const int ring_count = 1;
const int leds_per_ring = 16;
const int total_leds = ring_count * leds_per_ring;

int brightness = 35;
int hit_pixel[ring_count] = {8};
int ring_offset[ring_count] = {0};

int moving_pixel = 0;
int selected_ring = -1; // -1 means the ring
unsigned long last_move_time = 0;
int move_gap_ms = 90;

uint32_t lane_color[ring_count];

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

void print_led_test_settings() {
  Serial.println();
  Serial.println("air drum tiles neopixel test");
  Serial.print("led_data_pin=");
  Serial.println(led_data_pin);
  Serial.print("brightness=");
  Serial.println(brightness);
  Serial.print("move_gap_ms=");
  Serial.println(move_gap_ms);
  Serial.print("selected_ring=");
  Serial.println(selected_ring);

  for (int lane = 0; lane < ring_count; lane++) {
    Serial.print("lane=");
    Serial.print(lane);
    Serial.print(" hit_pixel=");
    Serial.print(hit_pixel[lane]);
    Serial.print(" offset=");
    Serial.println(ring_offset[lane]);
  }

  Serial.println();
  Serial.println("commands:");
  Serial.println("  a = animate the ring");
  Serial.println("  0 = animate ring 0");
  Serial.println("  + = faster");
  Serial.println("  - = slower");
  Serial.println("  h = show hit pixels and quarter marks");
  Serial.println("  p = print these settings");
  Serial.println("  x = clear leds");
  Serial.println();
}

void handle_led_test_serial() {
  if (Serial.available() == 0) {
    return;
  }

  char command = Serial.read();

  if (command == 'a') {
    selected_ring = -1;
    Serial.println("animating the ring");
  } else if (command == '0') {
    selected_ring = 0;
    Serial.println("animating ring 0");
  } else if (command == '+') {
    move_gap_ms = move_gap_ms - 10;
    if (move_gap_ms < 20) {
      move_gap_ms = 20;
    }
    Serial.print("move_gap_ms=");
    Serial.println(move_gap_ms);
  } else if (command == '-') {
    move_gap_ms = move_gap_ms + 10;
    if (move_gap_ms > 500) {
      move_gap_ms = 500;
    }
    Serial.print("move_gap_ms=");
    Serial.println(move_gap_ms);
  } else if (command == 'h') {
    draw_ring_numbers_once();
    Serial.println("showing hit pixels and quarter marks");
  } else if (command == 'p') {
    print_led_test_settings();
  } else if (command == 'x') {
    pixels.clear();
    pixels.show();
    Serial.println("cleared leds");
  }
}

void setup_led_test() {
  Serial.begin(115200);

  pixels.begin();
  pixels.setBrightness(brightness);
  pixels.clear();
  pixels.show();

  lane_color[0] = pixels.Color(0, 90, 255);

  print_led_test_settings();
}

void loop_led_test() {
  handle_led_test_serial();

  unsigned long now = millis();

  if (now - last_move_time >= (unsigned long)move_gap_ms) {
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
  current_sample = nullptr;
  sample_pos = 0;
  sample_len = length;
  current_sample = data;
}

// ── Sensor pins ───//
#define TRIG1 5    // hi-Hat  (built-in)
#define ECHO1 16
#define TRIG2 4    // kick
#define ECHO2 13
#define TRIG3 19   // snare
#define ECHO3 14

// ── Trigger config ────────────────────────────────────────────────────────────
#define TRIGGER_CM   15     // max position for hand
#define DEBOUNCE_MS  100    // minimum ms between re-triggers on the same drum

// Per-sensor state
struct DrumSensor {
  int       trig;
  int       echo;
  const char* name;
  const uint8_t* sample;
  uint32_t  sampleLen;
  bool      inZone;
  uint32_t  lastTriggerMs;
};

DrumSensor sensors[] = {
  { TRIG1, ECHO1, "HI-HAT", hihat_data, hihat_length, false, 0 },
  { TRIG2, ECHO2, "KICK",   kick_data,  kick_length,  false, 0 },
  { TRIG3, ECHO3, "SNARE",  snare_data, snare_length, false, 0 },

};
const int NUM_SENSORS = sizeof(sensors) / sizeof(sensors[0]);

// Sensor helper 
long getDistance(int trig, int echo) {
  digitalWrite(trig, LOW);
  delayMicroseconds(2);
  digitalWrite(trig, HIGH);
  delayMicroseconds(10);
  digitalWrite(trig, LOW);
  long duration = pulseIn(echo, HIGH, 30000);
  return duration * 0.034 / 2;
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
}

// ── Main loop ─────────────────────────────────────────────────────────────────
void loop() {
  if (NEOPIXEL_TEST_MODE == 1) {
    loop_led_test();
    return;
  }

  uint32_t now = millis();

  for (int i = 0; i < NUM_SENSORS; i++) {
    DrumSensor& s = sensors[i];
    long dist = getDistance(s.trig, s.echo);

    if (dist == 0) continue;  // pulseIn timeout — not a real reading

    bool handPresent = (dist < TRIGGER_CM);

    if (handPresent && !s.inZone && (now - s.lastTriggerMs >= DEBOUNCE_MS)) {
      // Rising edge into zone — fire once
      playDrum(s.sample, s.sampleLen);
      s.lastTriggerMs = now;
      Serial.print(s.name);
      Serial.println(" triggered");
    }

    s.inZone = handPresent;
  }
}
