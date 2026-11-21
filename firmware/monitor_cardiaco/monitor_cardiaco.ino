/*
 * ============================================================================
 *  Monitor de Frequência Cardíaca IoT — ESP32 + MQTT
 *  Universidade Presbiteriana Mackenzie — Objetos Inteligentes Conectados
 * ============================================================================
 *
 *  Sensor ....: MAX30102 (modo real) ou potenciômetro + chave (modo simulado,
 *               usado no Wokwi, que não possui o MAX30102 na biblioteca).
 *  Atuadores .: buzzer (alerta sonoro) + LED vermelho que espelha o buzzer.
 *  Display ...: OLED 0,96" SSD1306 (I2C).
 *  Rede ......: Wi-Fi + MQTT (PubSubClient) em um broker público.
 *
 *  Tópicos MQTT (TOPIC_BASE = "upm/iot/hr-monitor-01" por padrão):
 *    <base>/bpm           (pub)  leitura a cada batimento detectado (JSON)
 *    <base>/status        (pub)  online/offline, retido, com Last Will
 *    <base>/buzzer/state  (pub)  estado do atuador, retido, com eco do id
 *    <base>/buzzer/set    (sub)  comandos: on | off | auto | beep
 *    <base>/config/set    (sub)  limites de alarme: {"min":50,"max":120}
 *    <base>/ping          (sub)  eco imediato em <base>/pong (mede RTT)
 *    <base>/pong          (pub)
 *
 *  Uso acadêmico/experimental. NÃO é dispositivo médico.
 * ============================================================================
 */

#include <WiFi.h>
#include <PubSubClient.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <ArduinoJson.h>
#include <time.h>
#include <sys/time.h>

// ---------------------------------------------------------------------------
// Configuração
// ---------------------------------------------------------------------------

// 1 = modo simulado (Wokwi): potenciômetro define o BPM e a chave indica
//     "dedo no sensor". 0 = MAX30102 real (biblioteca SparkFun MAX3010x).
#define USE_SIMULATED_SENSOR 1

// No Wokwi a rede "Wokwi-GUEST" (sem senha) dá acesso à internet.
const char *WIFI_SSID = "Wokwi-GUEST";
const char *WIFI_PASS = "";

const char *MQTT_HOST = "broker.hivemq.com";
const uint16_t MQTT_PORT = 1883;

// Troque por um prefixo próprio: o broker é público e compartilhado.
const char *TOPIC_BASE = "upm/iot/hr-monitor-01";

// Pinos
const int PIN_SDA = 21;
const int PIN_SCL = 22;
const int PIN_BUZZER = 25;   // buzzer (físico: via transistor BC547)
const int PIN_LED = 26;      // LED vermelho, espelha o buzzer
const int PIN_POT = 34;      // modo simulado: potenciômetro (ADC1)
const int PIN_FINGER = 27;   // modo simulado: chave "dedo presente" (LOW = presente)

// Faixa experimental (não clínica) e número de leituras seguidas fora dela
// para disparar o alarme.
int bpmMin = 50;
int bpmMax = 120;
const uint8_t ALARM_CONSECUTIVE = 3;

const uint32_t NO_FINGER_PUBLISH_MS = 2000;  // publica "sem dedo" a cada 2 s
const uint32_t OLED_REFRESH_MS = 200;
const uint32_t MQTT_RETRY_MS = 3000;
const uint32_t BEEP_MS = 300;

// ---------------------------------------------------------------------------
// Sensor
// ---------------------------------------------------------------------------
#if !USE_SIMULATED_SENSOR
#include "MAX30105.h"   // SparkFun MAX3010x (compatível com MAX30102)
#include "heartRate.h"
MAX30105 particleSensor;
const long IR_FINGER_THRESHOLD = 50000;
#endif

Adafruit_SSD1306 display(128, 64, &Wire, -1);
WiFiClient netClient;
PubSubClient mqtt(netClient);

String tBpm, tStatus, tBuzzerState, tBuzzerSet, tConfigSet, tPing, tPong;
String clientId;

// Estado da medição
const uint8_t AVG_SIZE = 4;
float bpmWindow[AVG_SIZE];
uint8_t bpmIdx = 0, bpmCount = 0;
uint32_t lastBeatMs = 0;
int bpmAvg = 0;
bool fingerPresent = false;
uint32_t seq = 0;
uint32_t lastNoFingerPub = 0;
uint32_t beatFlashUntil = 0;
int simTargetBpm = 0;
uint32_t simNextBeat = 0;

// Estado do atuador
enum BuzzerMode { MODE_AUTO, MODE_ON, MODE_OFF };
BuzzerMode buzzerMode = MODE_AUTO;
uint8_t outOfRangeCount = 0;
bool alarmActive = false;
bool buzzerOutput = false;
uint32_t beepUntil = 0;

uint32_t lastOled = 0;
uint32_t lastMqttTry = 0;

// ---------------------------------------------------------------------------
// Utilidades
// ---------------------------------------------------------------------------

// Hora UTC em milissegundos desde 1970 (via NTP). Retorna 0 se ainda não
// sincronizou.
uint64_t epochMs() {
  struct timeval tv;
  gettimeofday(&tv, nullptr);
  if (tv.tv_sec < 1700000000) return 0;
  return (uint64_t)tv.tv_sec * 1000ULL + tv.tv_usec / 1000;
}

const char *modeName(BuzzerMode m) {
  switch (m) {
    case MODE_ON: return "on";
    case MODE_OFF: return "off";
    default: return "auto";
  }
}

// Liga/desliga fisicamente o buzzer e o LED.
void driveBuzzer(bool on) {
  if (on == buzzerOutput) return;
  buzzerOutput = on;
#if USE_SIMULATED_SENSOR
  // O buzzer do Wokwi é piezo: precisa de onda quadrada para soar.
  if (on) tone(PIN_BUZZER, 2000);
  else noTone(PIN_BUZZER);
#else
  // Buzzer ativo real: nível alto no GPIO satura o BC547 e o buzzer apita.
  digitalWrite(PIN_BUZZER, on ? HIGH : LOW);
#endif
  digitalWrite(PIN_LED, on ? HIGH : LOW);
}

// Decide a saída do atuador a partir do modo, do alarme e de um bip pendente.
void updateActuator(uint32_t now) {
  bool want;
  if (beepUntil && (int32_t)(now - beepUntil) < 0) want = true;
  else {
    beepUntil = 0;
    if (buzzerMode == MODE_ON) want = true;
    else if (buzzerMode == MODE_OFF) want = false;
    else want = alarmActive;
  }
  driveBuzzer(want);
}

// Publica o estado do atuador (retido). id/actUs ecoam o comando recebido
// para o dashboard medir a latência.
void publishBuzzerState(const char *id, long actUs) {
  JsonDocument doc;
  doc["mode"] = modeName(buzzerMode);
  doc["output"] = buzzerOutput;
  doc["alarm"] = alarmActive;
  if (id && id[0]) doc["id"] = id;
  if (actUs >= 0) doc["act_us"] = actUs;
  doc["ts"] = epochMs();
  char buf[200];
  size_t n = serializeJson(doc, buf);
  mqtt.publish(tBuzzerState.c_str(), (const uint8_t *)buf, n, true);
}

// ---------------------------------------------------------------------------
// Leitura do sensor: retorna true quando um batimento é detectado.
// ---------------------------------------------------------------------------
bool detectBeat(uint32_t now) {
#if USE_SIMULATED_SENSOR
  fingerPresent = digitalRead(PIN_FINGER) == LOW;
  if (!fingerPresent) { simNextBeat = 0; return false; }
  simTargetBpm = map(analogRead(PIN_POT), 0, 4095, 30, 180);
  uint32_t period = 60000UL / simTargetBpm;
  if (simNextBeat == 0) { simNextBeat = now + period; return false; }
  if ((int32_t)(now - simNextBeat) >= 0) {
    long j = period / 50;  // variação natural de ±2 % entre batimentos
    simNextBeat = now + period + random(-j, j + 1);
    return true;
  }
  return false;
#else
  long ir = particleSensor.getIR();
  fingerPresent = ir > IR_FINGER_THRESHOLD;
  if (!fingerPresent) return false;
  return checkForBeat(ir);
#endif
}

void resetMeasurement() {
  bpmIdx = bpmCount = 0;
  bpmAvg = 0;
  lastBeatMs = 0;
  outOfRangeCount = 0;
  alarmActive = false;
}

// Trata um batimento: calcula BPM (média móvel de 4), avalia alarme e publica.
void onBeat(uint32_t now, uint32_t detUs) {
  beatFlashUntil = now + 150;
  if (lastBeatMs == 0) { lastBeatMs = now; return; }
  uint32_t delta = now - lastBeatMs;
  lastBeatMs = now;
  float instant = 60000.0f / delta;
  if (instant < 20 || instant > 250) return;  // descarta leitura espúria

  bpmWindow[bpmIdx] = instant;
  bpmIdx = (bpmIdx + 1) % AVG_SIZE;
  if (bpmCount < AVG_SIZE) bpmCount++;
  float sum = 0;
  for (uint8_t i = 0; i < bpmCount; i++) sum += bpmWindow[i];
  bpmAvg = (int)(sum / bpmCount + 0.5f);

  // Alarme: só depois de 3 leituras seguidas fora da faixa (evita falso alarme)
  bool out = bpmCount >= AVG_SIZE && (bpmAvg < bpmMin || bpmAvg > bpmMax);
  if (!out) outOfRangeCount = 0;
  else if (outOfRangeCount < 255) outOfRangeCount++;
  bool newAlarm = outOfRangeCount >= ALARM_CONSECUTIVE;
  if (newAlarm != alarmActive) {
    alarmActive = newAlarm;
    updateActuator(now);
    if (mqtt.connected()) publishBuzzerState("", -1);
  }

  if (!mqtt.connected()) return;
  JsonDocument doc;
  doc["bpm"] = bpmAvg;
  doc["inst"] = (int)(instant + 0.5f);
  doc["finger"] = true;
  doc["alarm"] = alarmActive;
  doc["seq"] = ++seq;
  doc["ts"] = epochMs();
  doc["proc_us"] = micros() - detUs;  // detecção -> publicação (no ESP32)
  char buf[200];
  size_t n = serializeJson(doc, buf);
  mqtt.publish(tBpm.c_str(), (const uint8_t *)buf, n, false);
}

// ---------------------------------------------------------------------------
// MQTT
// ---------------------------------------------------------------------------
void onMqttMessage(char *topic, byte *payload, unsigned int len) {
  uint32_t rxUs = micros();
  String t(topic);

  if (t == tPing) {  // eco imediato, sem processamento
    // O payload aponta para o buffer interno do PubSubClient, que é
    // reutilizado pelo publish(): copia antes de responder.
    uint8_t copy[128];
    unsigned int n = len < sizeof(copy) ? len : sizeof(copy);
    memcpy(copy, payload, n);
    mqtt.publish(tPong.c_str(), copy, n, false);
    return;
  }

  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, payload, len);

  if (t == tBuzzerSet) {
    // Aceita JSON {"state":"on","id":"x"} ou texto puro "on".
    String state, id;
    if (!err && doc["state"].is<const char *>()) {
      state = doc["state"].as<const char *>();
      id = doc["id"] | "";
    } else {
      for (unsigned int i = 0; i < len; i++) state += (char)payload[i];
    }
    state.trim();
    state.toLowerCase();
    uint32_t now = millis();
    if (state == "on") buzzerMode = MODE_ON;
    else if (state == "off") buzzerMode = MODE_OFF;
    else if (state == "auto") buzzerMode = MODE_AUTO;
    else if (state == "beep") beepUntil = now + BEEP_MS;
    else { Serial.printf("[MQTT] comando desconhecido: %s\n", state.c_str()); return; }

    updateActuator(now);                // aciona o atuador imediatamente
    long actUs = (long)(micros() - rxUs);
    publishBuzzerState(id.c_str(), actUs);
    Serial.printf("[MQTT] buzzer <- %s (id=%s) acionado em %ld us\n",
                  state.c_str(), id.c_str(), actUs);
  } else if (t == tConfigSet && !err) {
    bpmMin = doc["min"] | bpmMin;
    bpmMax = doc["max"] | bpmMax;
    Serial.printf("[MQTT] limites: %d-%d BPM\n", bpmMin, bpmMax);
  }
}

void mqttConnect() {
  Serial.printf("[MQTT] conectando a %s:%u como %s...\n", MQTT_HOST, MQTT_PORT,
                clientId.c_str());
  // Last Will: se o ESP32 cair, o broker publica "offline" no lugar dele.
  const char *will = "{\"online\":false}";
  if (mqtt.connect(clientId.c_str(), tStatus.c_str(), 1, true, will)) {
    Serial.println("[MQTT] conectado");
    char buf[160];
    snprintf(buf, sizeof(buf),
             "{\"online\":true,\"ip\":\"%s\",\"sensor\":\"%s\",\"min\":%d,\"max\":%d}",
             WiFi.localIP().toString().c_str(),
             USE_SIMULATED_SENSOR ? "simulado" : "max30102", bpmMin, bpmMax);
    mqtt.publish(tStatus.c_str(), buf, true);
    mqtt.subscribe(tBuzzerSet.c_str(), 1);
    mqtt.subscribe(tConfigSet.c_str(), 1);
    mqtt.subscribe(tPing.c_str(), 0);
    publishBuzzerState("", -1);
  } else {
    Serial.printf("[MQTT] falhou, rc=%d\n", mqtt.state());
  }
}

// ---------------------------------------------------------------------------
// Display
// ---------------------------------------------------------------------------
void drawOled(uint32_t now) {
  display.clearDisplay();
  display.setTextColor(SSD1306_WHITE);
  display.setTextSize(1);
  display.setCursor(0, 0);
  display.print(WiFi.status() == WL_CONNECTED ? "WiFi:OK " : "WiFi:-- ");
  display.print(mqtt.connected() ? "MQTT:OK" : "MQTT:--");

  if (!fingerPresent) {
    display.setCursor(0, 26);
    display.print("Coloque o dedo");
    display.setCursor(0, 38);
    display.print("no sensor...");
  } else {
    display.setTextSize(3);
    display.setCursor(0, 18);
    if (bpmCount < AVG_SIZE) display.print("--");
    else display.print(bpmAvg);
    display.setTextSize(1);
    display.setCursor(64, 32);
    display.print("BPM");
    if ((int32_t)(now - beatFlashUntil) < 0) display.fillCircle(110, 28, 7, SSD1306_WHITE);
    else display.drawCircle(110, 28, 7, SSD1306_WHITE);
  }

  display.setCursor(0, 56);
  if (alarmActive) display.print("! ALARME ");
  else display.print("faixa ");
  display.printf("%d-%d", bpmMin, bpmMax);
  display.setCursor(104, 56);
  display.print(modeName(buzzerMode));
  display.display();
}

// ---------------------------------------------------------------------------
// setup / loop
// ---------------------------------------------------------------------------
void setup() {
  Serial.begin(115200);
  pinMode(PIN_BUZZER, OUTPUT);
  pinMode(PIN_LED, OUTPUT);
  digitalWrite(PIN_BUZZER, LOW);
  digitalWrite(PIN_LED, LOW);

  Wire.begin(PIN_SDA, PIN_SCL);
  Wire.setClock(400000);  // I2C rápido: OLED atualiza sem atrasar o sensor
  if (!display.begin(SSD1306_SWITCHCAPVCC, 0x3C)) Serial.println("[OLED] falha");
  display.clearDisplay();
  display.setTextColor(SSD1306_WHITE);
  display.setCursor(0, 0);
  display.println("Monitor Cardiaco IoT");
  display.println("Conectando WiFi...");
  display.display();

#if USE_SIMULATED_SENSOR
  pinMode(PIN_FINGER, INPUT_PULLUP);
  randomSeed(esp_random());
#else
  if (!particleSensor.begin(Wire, I2C_SPEED_FAST)) {
    Serial.println("[MAX30102] nao encontrado, verifique a ligacao");
  }
  particleSensor.setup();                     // configuração padrão SparkFun
  particleSensor.setPulseAmplitudeRed(0x0A);  // LED vermelho baixo: indica operação
  particleSensor.setPulseAmplitudeGreen(0);
#endif

  // Tópicos
  String base(TOPIC_BASE);
  tBpm = base + "/bpm";
  tStatus = base + "/status";
  tBuzzerState = base + "/buzzer/state";
  tBuzzerSet = base + "/buzzer/set";
  tConfigSet = base + "/config/set";
  tPing = base + "/ping";
  tPong = base + "/pong";
  clientId = "esp32-hr-" + String((uint32_t)ESP.getEfuseMac(), HEX);

  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  Serial.print("[WiFi] conectando");
  uint32_t t0 = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - t0 < 20000) {
    delay(250);
    Serial.print(".");
  }
  Serial.println(WiFi.status() == WL_CONNECTED ? " OK" : " falhou (tenta de novo no loop)");

  configTime(0, 0, "pool.ntp.org", "time.google.com");  // timestamps em UTC

  mqtt.setServer(MQTT_HOST, MQTT_PORT);
  mqtt.setCallback(onMqttMessage);
  mqtt.setBufferSize(512);
  mqtt.setKeepAlive(60);  // folga para o simulador, que pode rodar abaixo de 100 %
}

void loop() {
  uint32_t now = millis();

  if (WiFi.status() != WL_CONNECTED) {
    if (now - lastMqttTry > MQTT_RETRY_MS) { lastMqttTry = now; WiFi.reconnect(); }
  } else if (!mqtt.connected()) {
    if (now - lastMqttTry > MQTT_RETRY_MS) { lastMqttTry = now; mqttConnect(); }
  }
  mqtt.loop();

  bool wasFinger = fingerPresent;
  uint32_t detUs = micros();
  if (detectBeat(now)) onBeat(now, detUs);

  if (!fingerPresent) {
    if (wasFinger) resetMeasurement();
    if (now - lastNoFingerPub > NO_FINGER_PUBLISH_MS && mqtt.connected()) {
      lastNoFingerPub = now;
      char buf[100];
      snprintf(buf, sizeof(buf), "{\"bpm\":0,\"finger\":false,\"alarm\":false,\"ts\":%llu}",
               (unsigned long long)epochMs());
      mqtt.publish(tBpm.c_str(), buf, false);
    }
  }

  updateActuator(now);

  if (now - lastOled > OLED_REFRESH_MS) {
    lastOled = now;
    drawOled(now);
  }
}
