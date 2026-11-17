// =====================================================================
//  SOCCER BEE · МАШИНКА · прошивка V1.0
//  Kep Robotics x C.C.C.
// ---------------------------------------------------------------------
//  ESP32-C3 SuperMini + 2× L298N (кожен пін іде на обидва драйвери)
//    GPIO5  → ENA (перед + зад)   лівий борт, ШІМ
//    GPIO6  → IN1,  GPIO7 → IN2   лівий борт, напрям
//    GPIO10 → ENB (перед + зад)   правий борт, ШІМ
//    GPIO0  → IN3,  GPIO1 → IN4   правий борт, напрям
//    GPIO3  ← дільник VBAT (2S), K ≈ 3.525, уточнюється командою cal
//    GPIO8  — синій світлодіод на платі (статус)
//  Перемички ENA/ENB на L298N зняті.
//
//  Arduino IDE: "Nologo ESP32C3 Super Mini" (або ESP32C3 Dev Module з
//  USB CDC On Boot: Enabled), ядро esp32 3.x. Serial: 115200, Newline.
//
//  Логіка: чекаємо пульт → отримуємо 50 пакетів/с (газ, поворот, ліміти)
//  → змішуємо на два борти → плавно розганяємо, гальмуємо на стоп.
//  Немає пакетів 300 мс → стоп (failsafe). 5 разів/с шлемо телеметрію:
//  напругу, заряд, флаги розряду, скільки пакетів дійшло.
// =====================================================================
#include <Arduino.h>
#include <Preferences.h>
#include <BLEDevice.h>
#include <BLEServer.h>
#include <esp_mac.h>
#include "sb_protocol.h"

const uint8_t FW_MAJOR = 1, FW_MINOR = 0;

// ---------- Піни ----------
constexpr uint8_t PIN_L_EN = 5,  PIN_L_IN1 = 6, PIN_L_IN2 = 7;
constexpr uint8_t PIN_R_EN = 10, PIN_R_IN1 = 0, PIN_R_IN2 = 1;
constexpr uint8_t PIN_VBAT = 3;
constexpr uint8_t PIN_LED  = 8;      // світиться при LOW

// ---------- Налаштування ----------
const uint32_t FAILSAFE_MS = 300;    // без пакетів довше — стоп
const uint32_t PWM_FREQ    = 1000;   // Гц; L298N повільний, вище 2–3 кГц губить момент
const float VBAT_CORR_V = 0.0f;      // поправка напруги машинки, В (показує більше → мінус)
const float VBAT_LOW  = 6.8f;        // 3.4 В на банку
const float VBAT_CRIT = 6.4f;        // 3.2 В на банку
const float VBAT_HYST = 0.15f;
const uint32_t VBAT_CONFIRM_MS = 3000;   // просідання під навантаженням не рахуємо
const esp_power_level_t BLE_POWER = ESP_PWR_LVL_P9;

const uint32_t T_MOTOR = 10;     // 100 Гц

// скільки мс минуло від t (t могли записати в колбеці BLE трохи «пізніше» за now)
static inline uint32_t ageMs(uint32_t now, uint32_t t) { return (int32_t)(now - t) < 0 ? 0 : now - t; }
const uint32_t T_VBAT  = 200;
const uint32_t T_TEL   = 200;    // телеметрія 5 разів/с

// =====================================================================
//  Налаштування у флеш (NVS)
// =====================================================================
struct Settings {
  uint8_t  minPwm = 70;      // з якого ШІМ мотор реально рушає (0..255)
  bool     invL = false;     // лівий борт їде «не туди»
  bool     invR = false;
  bool     swapLR = false;   // переплутані борти (лівий стік крутить правий)
  bool     brake = true;     // на стоп: коротке гальмо (true) чи вільний накат
  uint16_t rampMs = 150;     // розгін 0 → 100 % за стільки мс (0 = без плавності)
  uint16_t brakeMs = 150;    // скільки тримати гальмо на стопі
  float    vbatK = 3.525f;   // (R1+R2)/R2
} cfg;

Preferences prefs;

void loadSettings() {
  prefs.begin("sbcar", true);
  cfg.minPwm  = prefs.getUChar("minPwm", cfg.minPwm);
  cfg.invL    = prefs.getBool("invL", cfg.invL);
  cfg.invR    = prefs.getBool("invR", cfg.invR);
  cfg.swapLR  = prefs.getBool("swapLR", cfg.swapLR);
  cfg.brake   = prefs.getBool("brake", cfg.brake);
  cfg.rampMs  = prefs.getUShort("rampMs", cfg.rampMs);
  cfg.brakeMs = prefs.getUShort("brakeMs", cfg.brakeMs);
  cfg.vbatK   = prefs.getFloat("vbatK", cfg.vbatK);
  prefs.end();
  if (cfg.vbatK < 1.5f || cfg.vbatK > 6.0f) cfg.vbatK = 3.525f;
}

void saveSettings() {
  prefs.begin("sbcar", false);
  prefs.putUChar("minPwm", cfg.minPwm);
  prefs.putBool("invL", cfg.invL);
  prefs.putBool("invR", cfg.invR);
  prefs.putBool("swapLR", cfg.swapLR);
  prefs.putBool("brake", cfg.brake);
  prefs.putUShort("rampMs", cfg.rampMs);
  prefs.putUShort("brakeMs", cfg.brakeMs);
  prefs.putFloat("vbatK", cfg.vbatK);
  prefs.end();
}

// =====================================================================
//  Мотори: борт = канал L298N (EN — ШІМ, IN1/IN2 — напрям)
// =====================================================================
struct Side {
  uint8_t  en, in1, in2;
  float    cur = 0;            // поточний вихід -100..100 (після рампи)
  uint32_t brakeUntil = 0;

  void begin() {
    pinMode(in1, OUTPUT); digitalWrite(in1, LOW);
    pinMode(in2, OUTPUT); digitalWrite(in2, LOW);
    ledcAttach(en, PWM_FREQ, 8);
    ledcWrite(en, 0);
  }
  void coast() { digitalWrite(in1, LOW); digitalWrite(in2, LOW); ledcWrite(en, 0); }
  void hardBrake() { digitalWrite(in1, HIGH); digitalWrite(in2, HIGH); ledcWrite(en, 255); }
  void drive(int v, bool inv) {            // v = -100..100, без нуля
    if (inv) v = -v;
    digitalWrite(in1, v > 0 ? HIGH : LOW);
    digitalWrite(in2, v < 0 ? HIGH : LOW);
    ledcWrite(en, map(abs(v), 1, 100, cfg.minPwm, 255));
  }
  // target -100..100; dtMs — час від минулого виклику
  void update(int target, uint32_t now, uint32_t dtMs, bool inv) {
    if (target == 0) {
      if (cur != 0) {                      // щойно зупинились
        cur = 0;
        brakeUntil = cfg.brake ? now + cfg.brakeMs : 0;
      }
      if (brakeUntil && (int32_t)(now - brakeUntil) < 0) hardBrake();
      else { brakeUntil = 0; coast(); }
      return;
    }
    brakeUntil = 0;
    if (cfg.rampMs == 0) cur = target;
    else {
      float step = 100.0f * dtMs / cfg.rampMs;
      if (cur < target) cur = min(cur + step, (float)target);
      else if (cur > target) cur = max(cur - step, (float)target);
    }
    int v = lroundf(cur);
    if (v == 0) coast(); else drive(v, inv);
  }
};

Side sideL{PIN_L_EN, PIN_L_IN1, PIN_L_IN2};
Side sideR{PIN_R_EN, PIN_R_IN1, PIN_R_IN2};

// газ/поворот + ліміти → два борти
void mix(int thr, int steer, uint8_t thrMax, uint8_t steerMax, int& l, int& r) {
  int t = thr * thrMax / 100;
  int s = steer * steerMax / 100;
  l = t + s;
  r = t - s;
  int m = max(abs(l), abs(r));
  if (m > 100) { l = l * 100 / m; r = r * 100 / m; }
}

// =====================================================================
//  Батарея 2S
// =====================================================================
float   vbatV = 0;
uint8_t vbatPct = 0;
bool    vLow = false, vCrit = false;
uint32_t lowSince = 0, critSince = 0;

uint8_t liionPercent(float v) {          // на одну банку
  static const float V[] = {3.30f, 3.40f, 3.50f, 3.60f, 3.70f, 3.80f, 3.90f, 4.00f, 4.10f, 4.20f};
  static const uint8_t P[] = {0, 5, 12, 22, 38, 53, 66, 79, 90, 100};
  if (v <= V[0]) return 0;
  if (v >= V[9]) return 100;
  for (int i = 1; i < 10; i++)
    if (v < V[i]) return P[i - 1] + (uint8_t)((v - V[i - 1]) / (V[i] - V[i - 1]) * (P[i] - P[i - 1]));
  return 100;
}

float readVbatRaw() {
  uint32_t mv = 0;
  for (int i = 0; i < 16; i++) mv += analogReadMilliVolts(PIN_VBAT);
  return (mv / 16.0f) * cfg.vbatK / 1000.0f + VBAT_CORR_V;
}

void vbatUpdate(uint32_t now) {
  float v = readVbatRaw();
  vbatV = (vbatV < 1.0f) ? v : vbatV * 0.9f + v * 0.1f;     // ~2 с згладжування
  vbatPct = liionPercent(vbatV / 2.0f);
  // низький: тримається нижче порогу 3 с; норма: вище порогу + гістерезис
  if (vbatV < VBAT_LOW) { if (!lowSince) lowSince = now; if (now - lowSince >= VBAT_CONFIRM_MS) vLow = true; }
  else { lowSince = 0; if (vbatV > VBAT_LOW + VBAT_HYST) vLow = false; }
  if (vbatV < VBAT_CRIT) { if (!critSince) critSince = now; if (now - critSince >= VBAT_CONFIRM_MS) vCrit = true; }
  else { critSince = 0; if (vbatV > VBAT_CRIT + VBAT_HYST) vCrit = false; }
}

// =====================================================================
//  BLE-сервер
// =====================================================================
BLEServer* server = nullptr;
BLECharacteristic* chrCmd = nullptr;
BLECharacteristic* chrTel = nullptr;
char devName[20] = "SoccerBee";

volatile bool connected = false;
SbCmd    rxCmd = {};
uint32_t rxMs = 0;
uint32_t rxCount = 0;
portMUX_TYPE rxMux = portMUX_INITIALIZER_UNLOCKED;

class ServerCB : public BLEServerCallbacks {
  void onConnect(BLEServer*) override { connected = true; }
  void onDisconnect(BLEServer*) override { connected = false; }   // реклама відновиться сама
};

class CmdCB : public BLECharacteristicCallbacks {
  void onWrite(BLECharacteristic* c) override {
    if (c->getLength() != sizeof(SbCmd)) return;
    SbCmd p;
    memcpy(&p, c->getData(), sizeof(p));
    if (p.magic != SB_CMD_MAGIC || p.sum != sbXor(&p, sizeof(p) - 1)) return;
    uint32_t now = millis();
    portENTER_CRITICAL(&rxMux);
    rxCmd = p; rxMs = now; rxCount++;
    portEXIT_CRITICAL(&rxMux);
  }
};

void bleBegin() {
  uint8_t mac[6];
  esp_read_mac(mac, ESP_MAC_BT);
  snprintf(devName, sizeof(devName), SB_NAME_PREFIX "%02X%02X", mac[4], mac[5]);
  BLEDevice::init(devName);
  BLEDevice::setPower(BLE_POWER);
  server = BLEDevice::createServer();
  server->setCallbacks(new ServerCB());
  server->advertiseOnDisconnect(true);
  BLEService* svc = server->createService(SB_SERVICE_UUID);
  chrCmd = svc->createCharacteristic(SB_CMD_UUID, BLECharacteristic::PROPERTY_WRITE | BLECharacteristic::PROPERTY_WRITE_NR);
  chrCmd->setCallbacks(new CmdCB());
  chrTel = svc->createCharacteristic(SB_TEL_UUID, BLECharacteristic::PROPERTY_READ | BLECharacteristic::PROPERTY_NOTIFY);
  svc->start();
  BLEAdvertising* adv = BLEDevice::getAdvertising();
  adv->addServiceUUID(SB_SERVICE_UUID);
  adv->setScanResponse(true);      // ім'я машинки — у відповіді на скан
  BLEDevice::startAdvertising();
}

// =====================================================================
//  Стан
// =====================================================================
int      motL = 0, motR = 0;         // цілі для бортів, -100..100
bool     failsafe = true;
uint8_t  rxRate = 0;
uint8_t  telSeq = 0;
uint32_t manualUntil = 0;            // ручний тест із Serial
int      manualL = 0, manualR = 0;
bool     monitorOn = false;

void motorsUpdate(uint32_t now) {
  static uint32_t last = 0;
  uint32_t dt = last ? now - last : T_MOTOR;
  last = now;

  SbCmd c; uint32_t ms;
  portENTER_CRITICAL(&rxMux); c = rxCmd; ms = rxMs; portEXIT_CRITICAL(&rxMux);

  int l = 0, r = 0;
  bool fresh = connected && ms && ageMs(now, ms) < FAILSAFE_MS;
  failsafe = !fresh;
  if (fresh && (c.flags & SB_CMD_DRIVE)) {
    mix(constrain(c.thr, -100, 100), constrain(c.steer, -100, 100),
        min<uint8_t>(c.thrMax, 100), min<uint8_t>(c.steerMax, 100), l, r);
  } else if (!connected && manualUntil && (int32_t)(now - manualUntil) < 0) {
    l = manualL; r = manualR;          // тест з комп'ютера, лише без пульта
  }
  if (cfg.swapLR) { int t = l; l = r; r = t; }
  motL = l; motR = r;
  sideL.update(l, now, dt, cfg.invL);
  sideR.update(r, now, dt, cfg.invR);
}

void telemetrySend() {
  static uint32_t lastCount = 0, lastMs = 0;
  uint32_t now = millis(), cnt;
  portENTER_CRITICAL(&rxMux); cnt = rxCount; portEXIT_CRITICAL(&rxMux);
  if (lastMs) rxRate = (uint8_t)min<uint32_t>(255, (cnt - lastCount) * 1000UL / max<uint32_t>(1, now - lastMs));
  lastCount = cnt; lastMs = now;
  if (!connected) return;
  SbTel t;
  t.magic = SB_TEL_MAGIC;
  t.seq = telSeq++;
  t.vbatMv = (uint16_t)(vbatV * 1000.0f);
  t.pct = vbatPct;
  t.flags = (vLow ? SB_TEL_LOW : 0) | (vCrit ? SB_TEL_CRITICAL : 0) | (failsafe ? SB_TEL_FAILSAFE : 0);
  t.rxRate = rxRate;
  t.motL = (int8_t)lroundf(sideL.cur);
  t.motR = (int8_t)lroundf(sideR.cur);
  t.fwMajor = FW_MAJOR; t.fwMinor = FW_MINOR;
  t.sum = sbXor(&t, sizeof(t) - 1);
  chrTel->setValue((uint8_t*)&t, sizeof(t));
  chrTel->notify();
}

// світлодіод: пошук — рідкий спалах; є пульт — горить; розряд — часто мигає
void ledUpdate(uint32_t now) {
  bool on;
  if (!connected)      on = now % 1000 < 100;
  else if (vLow)       on = now % 250 < 125;
  else if (failsafe)   on = now % 500 < 250;
  else                 on = true;
  digitalWrite(PIN_LED, on ? LOW : HIGH);
}

// =====================================================================
//  Serial-консоль
// =====================================================================
char cmdBuf[40];
uint8_t cmdLen = 0;

void printHelp() {
  Serial.println(F(
    "\n---------- SOCCER BEE · МАШИНКА ----------\n"
    "h             — ця довідка\n"
    "info          — стан і налаштування\n"
    "mon           — увімк/вимк вивід 2 рази/с\n"
    "m 60 -60      — тест бортів на 1.5 с (лише без пульта)\n"
    "minpwm 70     — з якого ШІМ рушає мотор (0..255)\n"
    "inv l | inv r — інвертувати борт\n"
    "swap          — поміняти борти місцями\n"
    "brake on|off  — гальмо на стопі чи вільний накат\n"
    "ramp 150      — розгін 0→100 % за N мс (0 = різко)\n"
    "cal 7.95      — калібрування VBAT (напруга з мультиметра)\n"
    "reset         — скинути налаштування\n"
    "Усі зміни одразу зберігаються.\n"
    "------------------------------------------"));
}

void printInfo() {
  Serial.printf("%s, Robot Firmware V%u.%u, BLE %s\n", devName, FW_MAJOR, FW_MINOR, BLEDevice::getAddress().toString().c_str());
  Serial.printf("Батарея %.2f В (%u%%)%s%s, K=%.4f\n", vbatV, vbatPct, vLow ? " НИЗЬКА" : "", vCrit ? " КРИТИЧНА" : "", cfg.vbatK);
  Serial.printf("Пульт: %s, пакетів %u/с%s\n", connected ? "підключено" : "чекаю", rxRate, failsafe ? ", FAILSAFE (стоп)" : "");
  Serial.printf("minPwm %u, invL %d, invR %d, swap %d, гальмо %s %u мс, рампа %u мс\n", cfg.minPwm, cfg.invL, cfg.invR,
                cfg.swapLR, cfg.brake ? "так" : "ні", cfg.brakeMs, cfg.rampMs);
}

void handleCommand(char* line) {
  char* arg = strchr(line, ' ');
  if (arg) { *arg++ = 0; while (*arg == ' ') arg++; }
  bool changed = false;
  if (!strcmp(line, "h") || !strcmp(line, "?")) printHelp();
  else if (!strcmp(line, "info")) printInfo();
  else if (!strcmp(line, "mon")) { monitorOn = !monitorOn; Serial.printf("Моніторинг: %s\n", monitorOn ? "ON" : "OFF"); }
  else if (!strcmp(line, "m") && arg) {
    int l = 0, r = 0;
    if (connected) Serial.println("Спершу вимкни пульт: тест працює лише без нього");
    else if (sscanf(arg, "%d %d", &l, &r) == 2) {
      manualL = constrain(l, -100, 100); manualR = constrain(r, -100, 100);
      manualUntil = millis() + 1500;
      Serial.printf("Тест: лівий %d%%, правий %d%% на 1.5 с\n", manualL, manualR);
    } else Serial.println("Формат: m 60 -60");
  }
  else if (!strcmp(line, "minpwm") && arg) { cfg.minPwm = constrain(atoi(arg), 0, 255); changed = true; }
  else if (!strcmp(line, "inv") && arg && *arg == 'l') { cfg.invL = !cfg.invL; changed = true; }
  else if (!strcmp(line, "inv") && arg && *arg == 'r') { cfg.invR = !cfg.invR; changed = true; }
  else if (!strcmp(line, "swap")) { cfg.swapLR = !cfg.swapLR; changed = true; }
  else if (!strcmp(line, "brake") && arg) { cfg.brake = !strcmp(arg, "on"); changed = true; }
  else if (!strcmp(line, "ramp") && arg) { cfg.rampMs = constrain(atoi(arg), 0, 2000); changed = true; }
  else if (!strcmp(line, "cal") && arg) {
    float real = atof(arg), shown = readVbatRaw();
    if (real > 4.0f && real < 9.0f && shown > 1.0f) {
      cfg.vbatK *= (real - VBAT_CORR_V) / (shown - VBAT_CORR_V);
      vbatV = real; changed = true;
    }
    else Serial.println("Формат: cal 7.95");
  }
  else if (!strcmp(line, "reset")) {
    prefs.begin("sbcar", false); prefs.clear(); prefs.end();
    Serial.println("Налаштування скинуто, перезапуск...");
    Serial.flush();
    ESP.restart();
  }
  else Serial.println("Невідома команда, h — довідка");
  if (changed) { saveSettings(); printInfo(); }
}

void pollSerial() {
  while (Serial.available()) {
    char c = Serial.read();
    if (c == '\r') continue;
    if (c == '\n') {
      cmdBuf[cmdLen] = 0;
      if (cmdLen) handleCommand(cmdBuf);
      cmdLen = 0;
    } else if (cmdLen < sizeof(cmdBuf) - 1) cmdBuf[cmdLen++] = c;
  }
}

// =====================================================================
void setup() {
  // мотори — у безпечний стан ще до всього іншого
  sideL.begin();
  sideR.begin();
  pinMode(PIN_LED, OUTPUT);
  digitalWrite(PIN_LED, HIGH);

  Serial.begin(115200);
  loadSettings();
  analogReadResolution(12);
  analogSetPinAttenuation(PIN_VBAT, ADC_11db);
  vbatUpdate(millis());

  bleBegin();
}

void loop() {
  const uint32_t now = millis();
  static uint32_t tMotor = 0, tVbat = 0, tTel = 0, tMon = 0;
  static bool welcomed = false, wasConnected = false;

  pollSerial();
  if (now - tMotor >= T_MOTOR) { tMotor = now; motorsUpdate(now); }
  if (now - tVbat >= T_VBAT)   { tVbat = now;  vbatUpdate(now); }
  if (now - tTel >= T_TEL)     { tTel = now;   telemetrySend(); }
  ledUpdate(now);

  if (connected != wasConnected) {
    wasConnected = connected;
    Serial.println(connected ? ">>> Пульт підключився" : ">>> Пульт відключився — стоп, чекаю знову");
  }
  if (!welcomed && now > 1500) {
    welcomed = true;
    Serial.printf("\n=== SOCCER BEE · МАШИНКА · Robot Firmware V%u.%u ===\n", FW_MAJOR, FW_MINOR);
    printInfo();
    Serial.println("h — список команд");
  }
  if (monitorOn && now - tMon >= 500) {
    tMon = now;
    SbCmd c;
    portENTER_CRITICAL(&rxMux); c = rxCmd; portEXIT_CRITICAL(&rxMux);
    Serial.printf("пульт %s %u/с | газ %+4d пов %+4d ліміт %u/%u %s | борти L%+4d R%+4d | %.2f В %u%%\n",
                  connected ? "є" : "--", rxRate, c.thr, c.steer, c.thrMax, c.steerMax,
                  failsafe ? "FAILSAFE" : (c.flags & SB_CMD_DRIVE) ? "DRIVE" : "stop",
                  (int)lroundf(sideL.cur), (int)lroundf(sideR.cur), vbatV, vbatPct);
  }
  delay(1);   // віддаємо процесор задачам BLE і idle; логіка на таймерах millis()
}
