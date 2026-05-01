# CICS 256 — MakerBoard ESP32 Coding Reference

## Board Specs
- **MCU:** ESP32 (ESP32-WROOM-32D)
- **CPU:** 240 MHz
- **RAM:** 160 KB (volatile)
- **Flash:** 4 MB (non-volatile)
- **Logic level:** 3.3 V (NOT 5 V like classic Arduino)
  - Digital HIGH = 3.3 V, LOW = 0 V
- **ADC:** 12-bit → values 0–4095 (0 V → 0, 3.3 V → 4095, linear in between)
  - Voltage from ADC: `V = adc * 3.3 / 4095`
  - ADC from voltage: `adc = V * 4095 / 3.3`
- **Wireless:** Built-in WiFi (2.4 GHz) + Bluetooth (2.4 GHz, BluetoothSerial library)

---

## Pin Map — Built-in Components (Digital)

| Pin | Component | Notes |
|---|---|---|
| **2** | Single-color LED | Built-in white LED |
| **25** | NeoPixel (WS2812) | One RGB pixel onboard |
| **17** | Buzzer | Use ledcWriteTone for sound |
| **34** | Button 1 | Read with digitalRead |
| **0** | Button 2 | Reverse logic (pressed = 0) |
| **35** | Button 3 | |
| **5, 16** | Ultrasonic sensor (HC-SR04) | 5 = TRIG/ECHO, 16 = the other |
| **23** | Servo 1 | 3-pin header, top-left |
| **18** | Servo 2 | 3-pin header, top-left |

**Spare digital pins:** 4, 13, 15, and others in the pinout area.

---

## Pin Map — Analog Inputs

| Pin | Component | Notes |
|---|---|---|
| **A0** | Potentiometer (POT) | Onboard knob |
| **A3** | IR receiver (pulse sensor) | Built-in IR pair |
| A13, A14, … | Spare | For your own analog sensors |

Many pins are dual-function — e.g., `15/A13` works as digital pin 15 OR analog pin A13.

**Touch pins:** T0–T9 (subset of GPIO pins, support `touchRead()`)

---

## Core Arduino Functions

### Setup / Loop
```cpp
void setup() { /* runs once */ }
void loop()  { /* runs forever */ }
```

### Serial
```cpp
Serial.begin(115200);     // common baud rates: 9600, 115200, 230400, 460800
Serial.print(value);
Serial.println(value);
Serial.print(15, HEX);    // base for ints, decimals for floats
```

### Timing
```cpp
delay(ms);                // blocking, milliseconds
delayMicroseconds(us);    // blocking, microseconds
millis();                 // unsigned long, ms since boot (rolls over ~50 days)
micros();                 // us since boot (rolls over ~71 minutes)
```

**Non-blocking pattern (preferred):**
```cpp
unsigned long previous = 0;
void loop() {
  if (millis() > previous + 1000) {
    // do thing every 1 second
    previous = millis();
  }
  // other tasks run freely
}
```

### Digital I/O
```cpp
pinMode(pin, OUTPUT);          // or INPUT, INPUT_PULLUP
digitalWrite(pin, HIGH);       // or LOW
int v = digitalRead(pin);      // returns 0 or 1
```

Buttons use **reverse logic** when wired to GND with pull-up: pressed = 0, released = 1. ESP32 has internal pull-ups — use `INPUT_PULLUP`, no external resistor needed.

### Analog Input
```cpp
int adc = analogRead(A0);      // returns 0..4095
```

### Analog Output (PWM via LEDC)
ESP32 has **no `analogWrite`** — use the LEDC functions:
```cpp
ledcSetup(channel, frequency, resolution);  // channel 0..15, resolution typically 8 (= 256 levels)
ledcAttachPin(pin, channel);
ledcWrite(channel, dutyCycle);              // 0..255 for 8-bit resolution
```

**Tone generation (buzzer):**
```cpp
ledcSetup(0, 5000, 8);            // 5 kHz PWM carrier
ledcAttachPin(17, 0);             // pin 17 = buzzer
ledcWriteTone(0, 1000);           // 1000 Hz tone
delay(500);
ledcWriteTone(0, 0);              // off
```

---

## Interrupts
```cpp
volatile unsigned long count = 0;          // volatile required for ISR-modified vars

void IRAM_ATTR myISR() {                   // IRAM_ATTR required
  count++;
}

void setup() {
  pinMode(BUTTON, INPUT_PULLUP);
  attachInterrupt(BUTTON, myISR, FALLING); // modes: RISING, FALLING, CHANGE
}
```

---

## NeoPixel (WS2812)

`platformio.ini`:
```ini
lib_deps = adafruit/Adafruit NeoPixel
```

Code:
```cpp
#include <Adafruit_NeoPixel.h>
Adafruit_NeoPixel pixel = Adafruit_NeoPixel(1, 25, NEO_GRB + NEO_KHZ800);
                                       //  count, pin, type

void setup() {
  pixel.begin();
  pixel.setBrightness(32);              // 0..255, keep low to limit current
}

void loop() {
  pixel.setPixelColor(0, pixel.Color(r, g, b));
  pixel.show();                         // ⚠️ MUST call show() to push to LEDs
}
```

---

## OLED Display (SSD1306, I²C address `0x3C`, 128×64)

`platformio.ini`:
```ini
lib_deps = https://github.com/ThingPulse/esp8266-oled-ssd1306/archive/refs/tags/4.6.1.zip
```

Code:
```cpp
#include <SSD1306Wire.h>
SSD1306Wire lcd(0x3c, SDA, SCL);

void setup() {
  lcd.init();
  lcd.flipScreenVertically();
  lcd.clear();
  lcd.setColor(WHITE);
  lcd.setFont(ArialMT_Plain_16);              // also _10, _24
  lcd.setTextAlignment(TEXT_ALIGN_CENTER_BOTH);
  lcd.drawString(64, 32, "Hello!");
  lcd.display();                              // ⚠️ MUST call display() to push buffer
}
```

**Useful draw functions:** `drawString`, `drawRect`, `fillRect`, `drawCircle`, `fillCircle`, `clear`, `println`, `print`. Old text is NOT auto-erased — call `clear()` first.

---

## Servo Motor

`platformio.ini`:
```ini
lib_deps = madhephaestus/ESP32Servo
```

Code:
```cpp
#include <ESP32Servo.h>
Servo myservo;

void setup() {
  myservo.attach(23);                  // SERVO1 = pin 23, SERVO2 = pin 18
}

void loop() {
  myservo.write(90);                   // angle 0..180
}
```

**Servo wiring (3-pin header on board):** Brown=GND (top), Red=5V, Yellow=DATA (bottom).

**Signal:** PWM, 20 ms period, pulse width 1 ms (0°) → 2 ms (180°).

---

## Ultrasonic Distance Sensor (HC-SR04)

4 pins: VCC, GND, TRIG (output), ECHO (input). Range 3–450 cm, ~0.3 cm accuracy. Plugs directly into board's 4-pin header.

```cpp
float readDistance() {
  digitalWrite(TRIG, LOW);
  delayMicroseconds(2);
  digitalWrite(TRIG, HIGH);
  delayMicroseconds(10);              // 10 us trigger pulse
  digitalWrite(TRIG, LOW);

  while (digitalRead(ECHO) == LOW) { }
  unsigned long start = micros();
  while (digitalRead(ECHO) == HIGH) { }
  unsigned long T = micros() - start; // time of flight, us

  return T * 0.01716f;                // cm: speed of sound 34320 cm/s ÷ 2 ÷ 10^6
}
```

---

## Capacitive Touch
```cpp
int v = touchRead(T0);                // works on T0..T9 only
// LOWER value = MORE capacitance = touched
```

---

## I²C (Wire library)
- 2 wires: SDA, SCL
- Speed: 100 kHz default, 400 kHz fast mode
- Each device has a 1-byte address

**Known I²C addresses on board:**
- OLED SSD1306: `0x3C`
- (External examples) IO expander: `0x20`, humidity sensor: `0x5C`

---

## SPI
- 4 wires: SCK, MOSI, MISO, CS
- Up to 8 MHz (~1 MB/s) — fastest option
- Used for SD card, Ethernet, color LCD, SPI flash

---

## Voltage Divider (for analog sensors like LDR/TDR)

For a sensor `Rs` in series with a fixed resistor `R` between VCC and GND:
- If sensor is on top: `Vout = R / (Rs + R) * Vs`
- If sensor is on bottom: `Vout = Rs / (Rs + R) * Vs`

Then read `Vout` with `analogRead`. To pick R, set it near the sensor's nominal resistance for best sensitivity.

---

## Final Project Components Available
- 16×16 NeoPixel matrices, 8×8 NeoPixel matrix, NeoPixel strips
- PAJ7620U2 gesture sensor (I²C, 9 gestures)
- VL53L0X laser range sensor (I²C, 2–1000 mm)
- MPU6050 6-axis IMU
- Microphone sensor
- Various: color sensor, barometric, RFID reader/tags, keypads, 7-seg, motors, audio amp + speaker
- Standalone Arduino Uno, Nano, compact ESP32 boards

---

## Quick "Don't Forget" Checklist
- ✅ `pixel.show()` after setting NeoPixel colors
- ✅ `lcd.display()` after drawing on OLED
- ✅ `lcd.clear()` before redrawing or old text stays
- ✅ `volatile` on any variable an ISR writes to
- ✅ `IRAM_ATTR` keyword on every ISR function
- ✅ ESP32 uses 3.3 V logic — don't feed it 5 V signals
- ✅ ADC range is 0–4095, not 0–1023 (that's classic Arduino)
- ✅ Use `ledcWrite`, NOT `analogWrite` (doesn't exist on ESP32)
- ✅ Buttons read 0 when pressed (reverse logic with pull-up)
