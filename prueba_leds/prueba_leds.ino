// Prueba de LED: enciende verde, amarillo y rojo uno tras otro, en bucle.
const int LEDS[] = {16, 17, 18};  // verde, amarillo, rojo

void setup() {
  for (int pin : LEDS) pinMode(pin, OUTPUT);
}

void loop() {
  for (int pin : LEDS) {
    digitalWrite(pin, HIGH);
    delay(500);
    digitalWrite(pin, LOW);
  }
}
