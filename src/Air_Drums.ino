#include <Arduino.h>
#include "kick.h"
#include "snare.h"
#include "hihat.h"

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
#define TRIG1 5    // kick  (built-in)
#define ECHO1 16
#define TRIG2 4    // snare
#define ECHO2 13
#define TRIG3 19   // hi-hat
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
  { TRIG1, ECHO1, "KICK",   kick_data,  kick_length,  false, 0 },
  { TRIG2, ECHO2, "SNARE",  snare_data, snare_length, false, 0 },
  { TRIG3, ECHO3, "HI-HAT", hihat_data, hihat_length, false, 0 },
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
