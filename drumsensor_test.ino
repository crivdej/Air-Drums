// #include <Arduino.h>
 
// //sensor 1: kick
// #define TRIG1 5   // kick  (onboard)
// #define ECHO1 16
// //sensor 2: snare
// #define TRIG2 4 
// #define ECHO2 13
// //sensor 3: hi-hat
// #define TRIG3 19  
// #define ECHO3 14
 
// long getDistance(int trig, int echo) {
//   digitalWrite(trig, LOW);
//   delayMicroseconds(2);
//   digitalWrite(trig, HIGH);
//   delayMicroseconds(10);
//   digitalWrite(trig, LOW);
//   long duration = pulseIn(echo, HIGH, 30000);
//   return duration * 0.034 / 2;
// }
 
// void setup() {
//   Serial.begin(115200);
//   pinMode(TRIG1, OUTPUT); pinMode(ECHO1, INPUT);
//   pinMode(TRIG2, OUTPUT); pinMode(ECHO2, INPUT);
//   pinMode(TRIG3, OUTPUT); pinMode(ECHO3, INPUT);
// }
 
// void loop() {
//   Serial.print("Kick: ");   Serial.print(getDistance(TRIG1, ECHO1)); Serial.print(" cm  ");
//   Serial.print("Snare: ");  Serial.print(getDistance(TRIG2, ECHO2)); Serial.print(" cm  ");
//   Serial.print("Hi-hat: "); Serial.print(getDistance(TRIG3, ECHO3)); Serial.println(" cm");
//   delay(200);
// }

