// PyroGuard - nodo ESP32 DevKit V1: DHT22 + MQ-2 + llama IR + 3 LEDs + buzzer + pulsador.
// Librerías (Gestor de librerías): "DHT sensor library" y "Adafruit Unified Sensor".
// NO subas este archivo a git con la clave real.

#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <DHT.h>
#include <time.h>
#include "secrets.h"   // WIFI_SSID, WIFI_PASS, API_KEY (no se sube a git)
#include "root_ca.h"   // ROOT_CA para validar el HTTPS

// Se define arriba para que el IDE genere bien los prototipos de las funciones.
struct Reading { float t, h, smoke; bool flame; };

// ---- Configuración -------------------------------------------------------
const char* API_URL   = "https://pyroguard-backend.onrender.com/api/readings/";
const char* NODE_ID   = "NODE-001";            // debe existir en el backend

const bool FLAME_ACTIVE_LOW = true;            // la mayoría de módulos IR dan LOW con llama
const unsigned long SEND_EVERY_MS = 3000;

// ---- Pines ---------------------------------------------------------------
const int PIN_DHT    = 4;    // DATA del DHT22 (con pull-up de 10k a 3V3 si tu DHT22 es sin módulo)
const int PIN_MQ2    = 34;   // salida del divisor 10k/15k (ADC1, compatible con WiFi)
const int PIN_FLAME  = 27;   // DO del sensor de llama
const int PIN_LED_G  = 16;   // verde  -> 220 ohm -> GND (RX2)
const int PIN_LED_Y  = 17;   // amarillo (TX2)
const int PIN_LED_R  = 18;   // rojo
const int PIN_BUZ_A  = 21;   // buzzer pasivo en oposición: + a GPIO21
const int PIN_BUZ_B  = 22;   //                             - a GPIO22 (sin transistor)
const int BUZZER_HZ  = 3000; // frecuencia donde suena más fuerte
const int PIN_BUTTON = 14;   // pulsador a 3V3 con pull-down 10k a GND

// Umbrales: iguales a api/services/risk_rules.py del backend.
const float TEMP_WARNING = 40, TEMP_FIRE = 50, SMOKE_WARNING = 40, SMOKE_FIRE = 60;

// MQ-2: en aire limpio lee ~0-5 cuentas; el humo o el gas cercano lo suben a cientos o miles.
// Con MQ2_SPAN = 400 cuentas sobre la línea base se da 100 %: 160 cuentas ya cruzan la
// advertencia (40) y 240 el fuego (60), así se detecta a más distancia (con 1200 hacía falta pegarse).
const int MQ2_SPAN = 400;
int mq2Baseline = 0;         // lectura en aire limpio, se mide al arrancar
int mq2Raw = 0;              // última lectura cruda (para diagnóstico)

DHT dht(PIN_DHT, DHT22);
unsigned long lastSend = 0;

hw_timer_t* buzzTimer = nullptr;
volatile bool buzzOn = false;
volatile bool buzzPhase = false;

// Temporizador: 2 veces por periodo, los dos pines del buzzer van en fase opuesta (~6.6 V pico a pico).
void IRAM_ATTR onBuzzTimer() {
  if (buzzOn) {
    buzzPhase = !buzzPhase;
    digitalWrite(PIN_BUZ_A, buzzPhase);
    digitalWrite(PIN_BUZ_B, !buzzPhase);
  } else {
    digitalWrite(PIN_BUZ_A, LOW);
    digitalWrite(PIN_BUZ_B, LOW);
  }
}

int readMq2Raw() {
  long sum = 0;
  for (int i = 0; i < 16; i++) sum += analogRead(PIN_MQ2);
  return sum / 16;
}

// Convierte a escala 0-100 relativa al aire limpio (la que usa el backend).
float readSmoke() {
  mq2Raw = readMq2Raw();
  float s = 100.0f * (mq2Raw - mq2Baseline) / MQ2_SPAN;
  return constrain(s, 0, 100);
}

bool readFlame() {
  return digitalRead(PIN_FLAME) == (FLAME_ACTIVE_LOW ? LOW : HIGH);
}

// 0 normal, 1 advertencia, 2 fuego
int localLevel(float t, float smoke, bool flame) {
  if (flame || t >= TEMP_FIRE || smoke >= SMOKE_FIRE) return 2;
  if (t >= TEMP_WARNING || smoke >= SMOKE_WARNING) return 1;
  return 0;
}

void showLevel(int level, bool test) {
  if (test) level = 2;
  digitalWrite(PIN_LED_G, level == 0);
  digitalWrite(PIN_LED_Y, level == 1);
  digitalWrite(PIN_LED_R, level == 2);
  // fuego: buzzer intermitente (no bloquea el loop)
  buzzOn = level == 2 && (millis() / 300) % 2 == 0;
}

void connectWifi() {
  if (WiFi.status() == WL_CONNECTED) return;
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  for (int i = 0; i < 20 && WiFi.status() != WL_CONNECTED; i++) delay(500);
}

bool timeReady() { return time(nullptr) > 1700000000; }

// Última lectura buena; la tarea de red la envía sin bloquear los sensores, los LEDs ni el buzzer.
Reading latest;
volatile bool haveReading = false;
portMUX_TYPE readingMux = portMUX_INITIALIZER_UNLOCKED;

// Conexión HTTPS que se reutiliza: el handshake TLS (2-4 s en el ESP32) se hace una sola vez.
WiFiClientSecure secureClient;
HTTPClient http;

void sendReading(const Reading& r) {
  float t = r.t, h = r.h, smoke = r.smoke;
  bool flame = r.flame;
  connectWifi();
  if (WiFi.status() != WL_CONNECTED || !timeReady()) return;

  time_t now = time(nullptr);
  struct tm utc;
  gmtime_r(&now, &utc);
  char ts[32];
  strftime(ts, sizeof ts, "%Y-%m-%dT%H:%M:%S+00:00", &utc);

  char body[256];
  snprintf(body, sizeof body,
           "{\"node_id\":\"%s\",\"temperature\":%.1f,\"humidity\":%.1f,"
           "\"smoke\":%.1f,\"flame\":%s,\"timestamp\":\"%s\"}",
           NODE_ID, t, h, smoke, flame ? "true" : "false", ts);

  secureClient.setCACert(ROOT_CA);  // valida el certificado del backend (necesita la hora NTP, ya comprobada arriba)
  http.setReuse(true);
  http.setTimeout(8000);
  http.begin(secureClient, API_URL);
  http.addHeader("Content-Type", "application/json");
  http.addHeader("X-API-Key", API_KEY);
  unsigned long t0 = millis();
  int code = http.POST(body);
  Serial.printf("POST %d (%lu ms)  %s\n", code, millis() - t0, body);
  if (code < 0) secureClient.stop();  // si falló, la próxima vez abre una conexión nueva
  http.end();
}

void sendTask(void*) {
  for (;;) {
    vTaskDelay(pdMS_TO_TICKS(SEND_EVERY_MS));
    if (!haveReading) continue;
    Reading r;
    portENTER_CRITICAL(&readingMux);
    r = latest;
    portEXIT_CRITICAL(&readingMux);
    sendReading(r);
  }
}

void setup() {
  Serial.begin(115200);
  pinMode(PIN_LED_G, OUTPUT); pinMode(PIN_LED_Y, OUTPUT); pinMode(PIN_LED_R, OUTPUT);
  pinMode(PIN_BUZ_A, OUTPUT); pinMode(PIN_BUZ_B, OUTPUT);
  buzzTimer = timerBegin(1000000);                      // 1 MHz
  timerAttachInterrupt(buzzTimer, &onBuzzTimer);
  timerAlarm(buzzTimer, 1000000 / (2 * BUZZER_HZ), true, 0);
  pinMode(PIN_BUTTON, INPUT);          // pull-down externo
  pinMode(PIN_FLAME, INPUT);
  analogSetPinAttenuation(PIN_MQ2, ADC_11db);
  dht.begin();

  // Arranque: LED amarillo encendido mientras el MQ-2 se calienta (necesita ~1-2 min; 90 s).
  digitalWrite(PIN_LED_Y, HIGH);
  connectWifi();
  Serial.printf("WiFi: %s\n", WiFi.status() == WL_CONNECTED ? "conectado" : "SIN conexion (el ESP32 solo usa 2.4 GHz)");
  configTime(0, 0, "pool.ntp.org", "time.google.com");
  delay(90000);
  mq2Baseline = readMq2Raw();
  Serial.printf("Baseline MQ-2: %d\n", mq2Baseline);
  xTaskCreatePinnedToCore(sendTask, "send", 10240, nullptr, 1, nullptr, 0);  // red en el núcleo 0
}

void loop() {
  // El DHT22 mide pulsos de microsegundos: se pausa el temporizador del buzzer durante la lectura.
  timerStop(buzzTimer);
  float t = dht.readTemperature();
  float h = dht.readHumidity();
  timerStart(buzzTimer);
  float smoke = readSmoke();
  bool flame = readFlame();
  bool test = digitalRead(PIN_BUTTON) == HIGH;

  // El nivel local se actualiza en cada vuelta, aunque no haya red.
  showLevel(isnan(t) ? (flame || smoke >= SMOKE_FIRE ? 2 : 0) : localLevel(t, smoke, flame), test);

  if (!isnan(t) && !isnan(h)) {
    portENTER_CRITICAL(&readingMux);
    latest = {t, h, smoke, flame};
    haveReading = true;
    portEXIT_CRITICAL(&readingMux);
  }

  // Diagnóstico cada segundo: mq2Raw = lectura cruda, humo = escala 0-100 que se envía.
  if (millis() - lastSend >= 1000) {
    lastSend = millis();
    Serial.printf("raw=%d base=%d humo=%.1f temp=%.1f llama=%d\n", mq2Raw, mq2Baseline, smoke, t, flame);
  }
  delay(50);
}
