// Prueba por etapas del circuito (sin WiFi). Abre el Monitor Serie a 115200.
// Librerías: "DHT sensor library" y "Adafruit Unified Sensor".
// Buzzer pasivo en oposición (puente): + a GPIO21, - a GPIO22, tono de 3 kHz.
// Suena solo: un pitido de 200 ms cada 2 s. Si hay pulsador y lo mantienes apretado, suena continuo.
// Cada 1 s imprime los sensores.
#include <DHT.h>

const int PIN_DHT = 4, PIN_MQ2 = 34, PIN_FLAME = 27, PIN_BUTTON = 14;
const int PIN_BUZZ_A = 21, PIN_BUZZ_B = 22;
const int BUZZER_HZ = 3000;

DHT dht(PIN_DHT, DHT22);
unsigned long last = 0;
hw_timer_t* buzzTimer = nullptr;
volatile bool buzzOn = false;
volatile bool buzzPhase = false;

// Se ejecuta 2 veces por periodo: los dos pines van en fase opuesta (~6.6 V pico a pico).
void IRAM_ATTR onBuzzTimer() {
  if (buzzOn) {
    buzzPhase = !buzzPhase;
    digitalWrite(PIN_BUZZ_A, buzzPhase);
    digitalWrite(PIN_BUZZ_B, !buzzPhase);
  } else {
    digitalWrite(PIN_BUZZ_A, LOW);
    digitalWrite(PIN_BUZZ_B, LOW);
  }
}

void setup() {
  Serial.begin(115200);
  pinMode(PIN_BUZZ_A, OUTPUT);
  pinMode(PIN_BUZZ_B, OUTPUT);
  buzzTimer = timerBegin(1000000);                       // 1 MHz
  timerAttachInterrupt(buzzTimer, &onBuzzTimer);
  timerAlarm(buzzTimer, 1000000 / (2 * BUZZER_HZ), true, 0);
  pinMode(PIN_BUTTON, INPUT);
  pinMode(PIN_FLAME, INPUT);
  analogSetPinAttenuation(PIN_MQ2, ADC_11db);
  dht.begin();
  delay(2000);  // el DHT22 necesita ~2 s tras alimentarse antes de la primera lectura
}

void loop() {
  bool button = digitalRead(PIN_BUTTON) == HIGH;
  bool beep = (millis() % 2000) < 200;
  buzzOn = button || beep;

  if (millis() - last >= 2500) {  // el DHT22 admite como máximo una lectura cada 2 s
    last = millis();
    // MQ-2 en aire limpio tras calentar: esperable ~300-1500 de 4095 (máx ~3724 con el divisor).
    // El DHT22 mide pulsos de microsegundos: se pausa el temporizador del buzzer durante la lectura.
    timerStop(buzzTimer);
    float t = dht.readTemperature(), h = dht.readHumidity();
    timerStart(buzzTimer);
    Serial.printf("DHT22 T=%.1f C H=%.1f %% | MQ2 raw=%d | llama(DO)=%d (0=llama) | boton=%d\n",
                  t, h, analogRead(PIN_MQ2), digitalRead(PIN_FLAME), button);
  }
}
