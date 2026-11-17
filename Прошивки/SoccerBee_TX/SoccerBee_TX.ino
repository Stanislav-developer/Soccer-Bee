// =====================================================================
//  SOCCER BEE · ПУЛЬТ (TX) · прошивка V1.0
//  Kep Robotics x C.C.C.
// ---------------------------------------------------------------------
//  Плата:   ESP32-C3 SuperMini, схема пульта TX v2
//  Arduino IDE:
//    Плата:      "Nologo ESP32C3 Super Mini" (або ESP32C3 Dev Module
//                з Tools → USB CDC On Boot: Enabled)
//    Ядро:       esp32 by Espressif 3.x (перевірено на 3.3.12)
//    Бібліотека: U8g2 by olikraus
//    Serial Monitor: 115200, "Newline"  →  h = список команд
//
//  Керування:
//    Лівий стік Y  — газ, правий стік X — поворот (за замовчуванням)
//    Клік стіка    — газ переходить на цей стік
//    B1            — меню: основне → дебаг → інфо
//    B1 3 с        — (дебаг, картка ЗВ'ЯЗОК) забути машинку, шукати нову
//    B2            — стіки / MPU
//    B2 2 с        — калібрування центрів стіків і гіроскопа
//    SW+SW 1 с     — екран швидкості: лівий стік — поворот, правий — газ
//    ПК-режим      — B1 3 с: вийти (пульт знову керує машинкою)
//
//  USB-контролер (з V1.1): пульт, підключений по USB-C до комп'ютера,
//  віддає всі свої дані JSON-рядками (протокол SBJ v1, див. розділ
//  «USB-контролер» нижче). Команди: hello, stream on [Гц], stream off,
//  state, pc on|off, beep <Гц> <мс>. Бібліотеки для ПК: soccerbee-usb.js
//  (Web Serial, Chrome/Edge) і soccerbee.py (Python + віртуальний геймпад).
//
//  Усе на millis(): loop() ніколи не чекає. Блокуючі виклики BLE
//  (пошук, підключення) живуть в окремій задачі FreeRTOS linkTask,
//  тому екран і кнопки не підвисають, поки пульт шукає машинку.
// =====================================================================
#include <Arduino.h>
#include <Wire.h>
#include <U8g2lib.h>
#include <Preferences.h>
#include <BLEDevice.h>
#include <esp_timer.h>
#include "sb_protocol.h"
#include "ui.h"

#if !ARDUINO_USB_CDC_ON_BOOT
#error "Увімкни Tools -> USB CDC On Boot -> Enabled (GPIO20/21 зайняті кнопкою B2 і джойстиком)"
#endif

// ---------- Піни (схема TX v2) ----------
constexpr uint8_t PIN_J1X   = 0;   // лівий стік X
constexpr uint8_t PIN_J1Y   = 1;   // лівий стік Y
constexpr uint8_t PIN_J2X   = 2;   // правий стік X  (GPIO2 = strapping-пін)
constexpr uint8_t PIN_J2Y   = 3;   // правий стік Y
constexpr uint8_t PIN_VBAT  = 4;   // дільник 100k/100k, до вимикача
constexpr uint8_t PIN_J1SW  = 5;   // кнопка лівого стіка
constexpr uint8_t PIN_J2SW  = 6;   // кнопка правого стіка
constexpr uint8_t PIN_B1    = 7;
constexpr uint8_t PIN_SDA   = 8;   // OLED + MPU-6050
constexpr uint8_t PIN_SCL   = 9;
constexpr uint8_t PIN_BUZ   = 10;  // бузер через NPN
constexpr uint8_t PIN_B2    = 20;
constexpr uint8_t PIN_J2GND = 21;  // «мінус» правого стіка (LOW після старту)

// ---------- Налаштування ----------
// Напрямки стіків (за результатами дебагу): + має бути вправо і вгору
const bool  INV_J1X = true;    // лівий стік: горизонталь була навпаки
const bool  INV_J1Y = false;
const bool  INV_J2X = false;
const bool  INV_J2Y = true;    // правий стік: вертикаль була навпаки
const int   STICK_DEAD  = 6;      // мертва зона стіка, %
const float STICK_EXPO  = 0.25f;  // 0 = лінійно; більше = точніше на малих швидкостях

// Режим MPU: машинка рушає лише коли нахил більший за 30°
const float MPU_DEAD_DEG  = 30.0f;
const float MPU_FULL_DEG  = 55.0f;  // при такому нахилі 100 %
const bool  MPU_INV_THR   = false;  // нахил «вперед» дає «назад»? → true
const bool  MPU_INV_STEER = false;  // нахил вправо повертає вліво? → true
const bool  MPU_SWAP_AXES = false;  // газ іде від бокового нахилу? → true (P і R міняються)

// Повороти машинки: true = знак повороту в пакеті перевернуто (і для стіків, і для MPU).
// На тестах машинка крутила не в той бік — тому true. Екран пульта показує стік як є.
const bool  STEER_INVERT = true;

const float VBAT_CORR_V = -0.10f;   // поправка напруги пульта, В: показує на 0.1 В більше → -0.10
const float VBAT_LOW  = 3.40f;      // поріг розряду пульта
const float VBAT_HYST = 0.05f;      // повертаємось у норму від 3.45 В
const uint32_t LOW_BEEP_EVERY_MS = 30000;  // нагадування про розряд

const uint8_t  SPEED_MIN = 20;      // мінімальний ліміт швидкості, %
const uint32_t COMBO_MS  = 1000;    // SW+SW: вхід/вихід з екрана швидкості
const uint32_t SPEED_IDLE_EXIT_MS = 6000;
const uint32_t B1_LONG_MS = 3000;   // забути машинку
const uint32_t B2_LONG_MS = 2000;   // калібрування
const uint32_t CLICK_MAX_MS = 600;  // довше — це вже не клік стіка
// тривалість заставки — ui::SPLASH_MS (2.5 с, задано в ui.h разом з анімацією)
const uint32_t CAL_CENTER_MS = 800;   // калібрування, крок 1: стіки відпущені
const uint32_t CAL_RANGE_MS  = 5000;  // крок 2: крутимо стіки по колу
const int      CAL_MIN_TRAVEL = 600;  // мінімальний хід осі в кожен бік, одиниць АЦП

const esp_power_level_t BLE_POWER = ESP_PWR_LVL_P9;   // +9 дБм

// ---------- Інтервали (millis) ----------
const uint32_t T_INPUT = 20;    // стіки, логіка, пакет — 50 Гц
const uint32_t T_MPU   = 10;    // 100 Гц
const uint32_t T_VBAT  = 200;
const uint32_t T_OLED  = 66;    // ~15 кадрів/с
const uint32_t T_SPLASH = 33;   // заставка ~30 кадрів/с (кадр по I2C ≈ 23 мс)
const uint8_t  OLED_CONTRAST = 0xCF;   // звичайна яскравість (як після oled.begin())
const uint32_t FADE_IN_MS = 250;       // основний екран плавно «проявляється» після заставки
const uint32_t T_LINK  = 20;    // пакети на машинку — 50 Гц

// Типи оголошено до будь-яких функцій (так Arduino IDE правильно генерує прототипи)
struct Note { uint16_t f; uint16_t ms; };   // ms = 0 → кінець
enum LinkState : uint8_t { LINK_SEARCH = 0, LINK_CONNECTING = 1, LINK_READY = 2 };
enum Screen : uint8_t { SCR_SPLASH, SCR_MAIN, SCR_DEBUG, SCR_INFO, SCR_SPEED, SCR_CALIB };
// Калібрування стіків у флеші: центр і крайні положення кожної осі (сирі значення АЦП)
struct __attribute__((packed)) StickCal {
  uint16_t magic;                 // 0x5C01 = дані є
  int16_t  c[4], mn[4], mx[4];    // порядок осей: лівий X, лівий Y, правий X, правий Y
};

// скільки мс минуло від t (t могли записати в іншій задачі трохи «пізніше» за now)
static inline uint32_t ageMs(uint32_t now, uint32_t t) { return (int32_t)(now - t) < 0 ? 0 : now - t; }

U8G2_SSD1306_128X64_NONAME_F_HW_I2C oled(U8G2_R0, U8X8_PIN_NONE);
// Якщо замість картинки «сміття» — це SH1106:
// U8G2_SH1106_128X64_NONAME_F_HW_I2C oled(U8G2_R0, U8X8_PIN_NONE);

Preferences prefs;

// =====================================================================
//  Налаштування, що зберігаються у флеш (NVS)
// =====================================================================
struct Settings {
  uint8_t thrMax = 80;
  uint8_t steerMax = 60;
  bool    buzPassive = true;     // HC-12085 — пасивний; активний: команда «buz a»
  float   vbatK = 2.0f;          // (R1+R2)/R2, уточнюється командою cal
  char    carMac[18] = "";       // прив'язана машинка, "" = будь-яка (найближча)
  char    carName[20] = "";
  StickCal stick = {};           // калібрування стіків (magic != 0x5C01 — ще не калібрували)
} cfg;

void loadSettings() {
  prefs.begin("sbtx", true);
  cfg.thrMax     = prefs.getUChar("thrMax", cfg.thrMax);
  cfg.steerMax   = prefs.getUChar("steerMax", cfg.steerMax);
  cfg.buzPassive = prefs.getBool("buzP", cfg.buzPassive);
  cfg.vbatK      = prefs.getFloat("vbatK", cfg.vbatK);
  prefs.getString("carMac", cfg.carMac, sizeof(cfg.carMac));
  prefs.getString("carName", cfg.carName, sizeof(cfg.carName));
  if (!prefs.isKey("stick") || prefs.getBytes("stick", &cfg.stick, sizeof(cfg.stick)) != sizeof(cfg.stick)) cfg.stick.magic = 0;
  prefs.end();
  cfg.thrMax   = constrain(cfg.thrMax, SPEED_MIN, 100);
  cfg.steerMax = constrain(cfg.steerMax, SPEED_MIN, 100);
  if (cfg.vbatK < 1.0f || cfg.vbatK > 4.0f) cfg.vbatK = 2.0f;
}

void saveSpeed() {
  prefs.begin("sbtx", false);
  prefs.putUChar("thrMax", cfg.thrMax);
  prefs.putUChar("steerMax", cfg.steerMax);
  prefs.end();
}
void saveStick() { prefs.begin("sbtx", false); prefs.putBytes("stick", &cfg.stick, sizeof(cfg.stick)); prefs.end(); }
void saveBuzzer() { prefs.begin("sbtx", false); prefs.putBool("buzP", cfg.buzPassive); prefs.end(); }
void saveVbatK()  { prefs.begin("sbtx", false); prefs.putFloat("vbatK", cfg.vbatK); prefs.end(); }
void saveCar() {
  prefs.begin("sbtx", false);
  prefs.putString("carMac", cfg.carMac);
  prefs.putString("carName", cfg.carName);
  prefs.end();
}

// =====================================================================
//  Кнопки з антидребезгом
// =====================================================================
struct Button {
  uint8_t  pin = 0;
  uint32_t longMs = 1000;
  bool     down = false, rawLast = false;
  uint32_t tRaw = 0, tDown = 0;
  bool     pressed = false, released = false, longFired = false, longEvent = false;
  bool     tainted = false;      // під час цього натискання був «комбо» з іншою кнопкою

  void begin(uint8_t p, uint32_t lm) { pin = p; longMs = lm; pinMode(pin, INPUT_PULLUP); }
  void update(uint32_t now) {
    pressed = released = longEvent = false;
    bool raw = digitalRead(pin) == LOW;
    if (raw != rawLast) { rawLast = raw; tRaw = now; }
    if (raw != down && now - tRaw >= 25) {
      down = raw;
      if (down) { pressed = true; tDown = now; longFired = false; tainted = false; }
      else      released = true;
    }
    if (down && !longFired && now - tDown >= longMs) { longFired = true; longEvent = true; }
  }
  uint32_t heldMs(uint32_t now) const { return down ? now - tDown : 0; }
  // коротке натискання: відпустили до «довгого» і без комбо
  bool click() const { return released && !longFired && !tainted; }
};

Button btnB1, btnB2, btnJ1, btnJ2;

// =====================================================================
//  Стіки
// =====================================================================
struct Axis {
  uint8_t pin = 0;
  bool    inv = false;
  int     raw = 2048, center = 2048;
  int     minV = 0, maxV = 4095;   // крайні положення (з калібрування; без нього — межі АЦП)
  void read() {
    uint32_t s = 0;
    for (int i = 0; i < 4; i++) s += analogRead(pin);
    raw = s / 4;
  }
  // -100..100 з мертвою зоною; 100 % трохи раніше краю (5 % запасу)
  int norm() const {
    int d = raw - center;
    long span = d >= 0 ? (maxV - center) : (center - minV);
    span = span * 95 / 100;
    if (span < 1) return 0;
    long v = (long)d * 100 / span;
    v = constrain(v, -100L, 100L);
    if (labs(v) < STICK_DEAD) v = 0;
    else v = (v > 0 ? 1 : -1) * (labs(v) - STICK_DEAD) * 100 / (100 - STICK_DEAD);
    return inv ? -(int)v : (int)v;
  }
};

Axis j1x, j1y, j2x, j2y;

Axis* const AXES[4] = {&j1x, &j1y, &j2x, &j2y};

// Старт: беремо збережене калібрування; центр уточнюємо, якщо стік відпущений
// (поточне значення поруч зі збереженим центром). Без калібрування — центр = як стоїть зараз.
void applyBootCenters() {
  bool have = cfg.stick.magic == 0x5C01;
  for (int i = 0; i < 4; i++) {
    Axis* a = AXES[i];
    uint32_t s = 0;
    for (int k = 0; k < 8; k++) { a->read(); s += a->raw; }
    int now = s / 8;
    if (have) {
      a->center = cfg.stick.c[i]; a->minV = cfg.stick.mn[i]; a->maxV = cfg.stick.mx[i];
      if (abs(now - a->center) < 250) a->center = now;
    } else {
      a->minV = 0; a->maxV = 4095;
      a->center = (now > 300 && now < 3800) ? now : 2048;
    }
  }
}

int expo(int v, float e) {
  float x = v / 100.0f;
  return lroundf(((1.0f - e) * x + e * x * x * x) * 100.0f);
}

// =====================================================================
//  MPU-6050 (сирий I2C, без бібліотек)
// =====================================================================
namespace mpu {
  const uint8_t ADDR = 0x68;
  bool ok = false;
  uint8_t who = 0;
  float gbx = 0, gby = 0;
  float pitch = 0, roll = 0;
  float axG = 0, ayG = 0, azG = 0;     // прискорення, g
  float gxD = 0, gyD = 0, gzD = 0;     // кутова швидкість, °/с (без зміщення)
  float gbz = 0;
  uint32_t lastUs = 0, errors = 0;
  bool calibrating = false;
  uint16_t calN = 0;
  float calX = 0, calY = 0, calZ = 0;

  bool write8(uint8_t reg, uint8_t val) {
    Wire.beginTransmission(ADDR); Wire.write(reg); Wire.write(val);
    return Wire.endTransmission() == 0;
  }
  bool readN(uint8_t reg, uint8_t* buf, uint8_t n) {
    Wire.beginTransmission(ADDR); Wire.write(reg);
    if (Wire.endTransmission(false) != 0) return false;
    if (Wire.requestFrom(ADDR, (size_t)n) != n) return false;
    for (uint8_t i = 0; i < n; i++) buf[i] = Wire.read();
    return true;
  }
  void startCalibration() { calibrating = true; calN = 0; calX = calY = calZ = 0; }
  bool begin() {
    ok = false;
    if (!readN(0x75, &who, 1)) return false;
    write8(0x6B, 0x01);   // вийти зі сну, тактування від гіроскопа
    write8(0x1A, 0x03);   // фільтр ~44 Гц
    write8(0x19, 0x04);   // 200 Гц
    write8(0x1B, 0x08);   // ±500 °/с
    write8(0x1C, 0x00);   // ±2 g
    ok = true;
    lastUs = micros();
    startCalibration();
    return true;
  }
  void update() {
    if (!ok) return;
    uint8_t b[14];
    if (!readN(0x3B, b, 14)) { errors++; return; }
    int16_t ax = b[0] << 8 | b[1], ay = b[2] << 8 | b[3], az = b[4] << 8 | b[5];
    int16_t gx = b[8] << 8 | b[9], gy = b[10] << 8 | b[11], gz = b[12] << 8 | b[13];
    uint32_t nowUs = micros();
    float dt = (nowUs - lastUs) * 1e-6f;
    lastUs = nowUs;
    if (dt <= 0 || dt > 0.1f) dt = 0.01f;
    float rx = gx / 65.5f, ry = gy / 65.5f, rz = gz / 65.5f;
    if (calibrating) {
      calX += rx; calY += ry; calZ += rz;
      if (++calN >= 200) { gbx = calX / calN; gby = calY / calN; gbz = calZ / calN; calibrating = false; }
    }
    rx -= gbx; ry -= gby; rz -= gbz;
    float fax = ax / 16384.0f, fay = ay / 16384.0f, faz = az / 16384.0f;
    axG = fax; ayG = fay; azG = faz; gxD = rx; gyD = ry; gzD = rz;
    float accRoll  = atan2f(fay, faz) * RAD_TO_DEG;
    float accPitch = atan2f(-fax, sqrtf(fay * fay + faz * faz)) * RAD_TO_DEG;
    if (calibrating) { roll = accRoll; pitch = accPitch; return; }
    roll  = 0.98f * (roll  + rx * dt) + 0.02f * accRoll;
    pitch = 0.98f * (pitch + ry * dt) + 0.02f * accPitch;
  }
}

// нахил → -100..100 з мертвою зоною MPU_DEAD_DEG
int tiltAxis(float deg, bool inv) {
  float m = fabsf(deg);
  if (m <= MPU_DEAD_DEG) return 0;
  float f = (m - MPU_DEAD_DEG) / (MPU_FULL_DEG - MPU_DEAD_DEG);
  if (f > 1.0f) f = 1.0f;
  int v = lroundf(f * 100.0f);
  if (deg < 0) v = -v;
  return inv ? -v : v;
}

// =====================================================================
//  Батарея пульта
// =====================================================================
float vbatV = 0;
int   vbatPinMv = 0;
bool  txLow = false;

void readVbat() {
  uint32_t s = 0;
  for (int i = 0; i < 16; i++) s += analogReadMilliVolts(PIN_VBAT);
  vbatPinMv = s / 16;
  float v = vbatPinMv * cfg.vbatK / 1000.0f + VBAT_CORR_V;
  vbatV = (vbatV < 0.5f) ? v : vbatV * 0.9f + v * 0.1f;     // згладжування ~2 с
  if (vbatV > 1.0f) {
    if (!txLow && vbatV < VBAT_LOW) txLow = true;
    else if (txLow && vbatV > VBAT_LOW + VBAT_HYST) txLow = false;
  }
}

uint8_t liionPercent(float v) {
  static const float V[] = {3.30f, 3.40f, 3.50f, 3.60f, 3.70f, 3.80f, 3.90f, 4.00f, 4.10f, 4.20f};
  static const uint8_t P[] = {0, 5, 12, 22, 38, 53, 66, 79, 90, 100};
  if (v <= V[0]) return 0;
  if (v >= V[9]) return 100;
  for (int i = 1; i < 10; i++)
    if (v < V[i]) return P[i - 1] + (uint8_t)((v - V[i - 1]) / (V[i] - V[i - 1]) * (P[i] - P[i - 1]));
  return 100;
}

// =====================================================================
//  Бузер: пасивний (тон через LEDC) або активний (увімк/вимк)
// =====================================================================
const Note MEL_START[] = {   // ~1.4 с: «бджола» дзижчить, потім фанфара
  {622, 30}, {659, 30}, {622, 30}, {659, 30}, {698, 30}, {659, 30}, {698, 30}, {740, 30},
  {698, 30}, {740, 30}, {784, 30}, {740, 30}, {784, 30}, {831, 30}, {0, 80},
  {1047, 110}, {0, 25}, {1319, 110}, {0, 25}, {1568, 110}, {0, 25}, {2093, 420}, {0, 0}};
const Note MEL_CONN[]  = {{1760, 70}, {0, 35}, {2349, 120}, {0, 0}};
const Note MEL_LOST[]  = {{2349, 70}, {0, 35}, {1175, 160}, {0, 0}};
const Note MEL_LOW[]   = {{2637, 70}, {0, 70}, {2637, 70}, {0, 70}, {2637, 70}, {0, 0}};
const Note MEL_TICK[]  = {{2800, 18}, {0, 0}};
const Note MEL_MODE[]  = {{1568, 50}, {0, 25}, {2093, 70}, {0, 0}};
const Note MEL_OK[]    = {{2093, 60}, {0, 30}, {2637, 90}, {0, 0}};
const Note MEL_ERR[]   = {{392, 220}, {0, 0}};

// Мелодії грає esp_timer кожні 2 мс (окрема задача), тому ноти рівні,
// навіть коли loop() зайнятий відправкою кадру на OLED.
// buzOut() викликає тільки таймер (і setup/зміна типу бузера при зупиненому таймері).
bool ledcOn = false;
const Note* volatile mel = nullptr;          // що грає зараз (пише таймер)
const Note* volatile melPending = nullptr;   // що зіграти наступним (пише loop)
volatile bool melStopReq = false;
uint8_t  melIdx = 0;
uint32_t melT = 0;
esp_timer_handle_t soundTimer = nullptr;
portMUX_TYPE sndMux = portMUX_INITIALIZER_UNLOCKED;
Note beepNote[2] = {{0, 1}, {0, 0}};          // одиночний тон за командою з ПК

void buzOut(uint16_t f) {
  if (cfg.buzPassive) ledcWriteTone(PIN_BUZ, f);   // 0 → тиша
  else                digitalWrite(PIN_BUZ, f ? HIGH : LOW);
}

void buzSetup() {
  if (ledcOn) { ledcDetach(PIN_BUZ); ledcOn = false; }
  pinMode(PIN_BUZ, OUTPUT);
  digitalWrite(PIN_BUZ, LOW);
  if (cfg.buzPassive) {
    ledcOn = ledcAttach(PIN_BUZ, 2000, 10);
    ledcWriteTone(PIN_BUZ, 0);
  }
}

void play(const Note* m) { portENTER_CRITICAL(&sndMux); melPending = m; portEXIT_CRITICAL(&sndMux); }
bool soundBusy() { return mel || melPending; }

void soundTick(void*) {
  uint32_t now = millis();
  const Note* p;
  bool stop;
  portENTER_CRITICAL(&sndMux); p = melPending; melPending = nullptr; stop = melStopReq; melStopReq = false; portEXIT_CRITICAL(&sndMux);
  if (stop && !p) { mel = nullptr; buzOut(0); return; }
  if (p) { mel = p; melIdx = 0; melT = now; buzOut(p[0].f); return; }
  const Note* m = mel;
  if (!m) return;
  if (now - melT < m[melIdx].ms) return;
  melT = now;
  melIdx++;
  if (m[melIdx].ms == 0) { buzOut(0); mel = nullptr; return; }
  buzOut(m[melIdx].f);
}

void soundStart() {
  if (!soundTimer) {
    esp_timer_create_args_t a = {};
    a.callback = soundTick;
    a.dispatch_method = ESP_TIMER_TASK;
    a.name = "sbSnd";
    a.skip_unhandled_events = true;
    esp_timer_create(&a, &soundTimer);
  }
  esp_timer_start_periodic(soundTimer, 2000);
}
void soundPause() { if (soundTimer) esp_timer_stop(soundTimer); }

// =====================================================================
//  BLE: окрема задача, спільні дані під спін-локом
// =====================================================================
struct Shared {
  // loop → linkTask
  SbCmd    cmd = {};
  bool     forget = false;          // забути прив'язку й шукати нову
  // linkTask → loop
  LinkState state = LINK_SEARCH;
  uint32_t stateMs = 0;
  int      rssi = -127;
  uint16_t txRate = 0;
  bool     haveMac = false;         // шукаємо тільки свою машинку
  bool     newPair = false;         // підключились до нової — треба зберегти
  char     mac[18] = "";
  char     name[20] = "";
  SbTel    tel = {};
  uint32_t telMs = 0;
  bool     telValid = false;
} sh;

portMUX_TYPE shMux = portMUX_INITIALIZER_UNLOCKED;
#define SH_LOCK()   portENTER_CRITICAL(&shMux)
#define SH_UNLOCK() portEXIT_CRITICAL(&shMux)

BLEClient* client = nullptr;
BLERemoteCharacteristic* chrCmd = nullptr;
BLERemoteCharacteristic* chrTel = nullptr;
BLEAddress savedAddr;

// результати пошуку
volatile bool scanFound = false;
BLEAddress    foundAddr;
uint8_t       foundType = 0;
int           foundRssi = -999;
char          foundName[20] = "";

class ScanCB : public BLEAdvertisedDeviceCallbacks {
  void onResult(BLEAdvertisedDevice d) override {
    if (!d.haveServiceUUID() || !d.isAdvertisingService(BLEUUID(SB_SERVICE_UUID))) return;
    bool haveMac;
    SH_LOCK(); haveMac = sh.haveMac; SH_UNLOCK();
    BLEAddress a = d.getAddress();
    if (haveMac) {
      if (memcmp(a.getNative(), savedAddr.getNative(), 6) != 0) return;   // чужа машинка
    } else if (scanFound && d.getRSSI() <= foundRssi) {
      return;                                                 // шукаємо найближчу
    }
    foundAddr = a;
    foundType = d.getAddressType();
    foundRssi = d.getRSSI();
    String n = d.haveName() ? d.getName() : String("SoccerBee");
    strlcpy(foundName, n.c_str(), sizeof(foundName));
    scanFound = true;
    if (haveMac) BLEDevice::getScan()->stop();               // своя — одразу підключаємось
  }
};

void onTelemetry(BLERemoteCharacteristic*, uint8_t* data, size_t len, bool) {
  if (len != sizeof(SbTel)) return;
  SbTel t;
  memcpy(&t, data, sizeof(t));
  if (t.magic != SB_TEL_MAGIC || t.sum != sbXor(&t, sizeof(t) - 1)) return;
  uint32_t now = millis();
  SH_LOCK(); sh.tel = t; sh.telMs = now; sh.telValid = true; SH_UNLOCK();
}

void setState(LinkState s) {
  uint32_t now = millis();
  SH_LOCK(); sh.state = s; sh.stateMs = now; SH_UNLOCK();
}

bool connectFound() {
  setState(LINK_CONNECTING);
  if (!client->connect(foundAddr, foundType)) return false;
  chrCmd = chrTel = nullptr;
  client->getServices();                      // свіжий пошук сервісів (кеш міг лишитись від минулого разу)
  BLERemoteService* svc = client->getService(SB_SERVICE_UUID);
  if (svc) {
    chrCmd = svc->getCharacteristic(SB_CMD_UUID);
    chrTel = svc->getCharacteristic(SB_TEL_UUID);
  }
  if (!svc || !chrCmd || !chrTel || !chrTel->canNotify()) { client->disconnect(); return false; }
  chrTel->registerForNotify(onTelemetry);
  // інтервал 7.5–15 мс, таймаут 1 с (напряму через NimBLE: у ядрі 3.3.6 немає updateConnParams)
#if defined(CONFIG_NIMBLE_ENABLED)
  ble_gap_upd_params cp = {};
  cp.itvl_min = 6;                 // × 1.25 мс
  cp.itvl_max = 12;
  cp.latency = 0;
  cp.supervision_timeout = 100;    // × 10 мс
  cp.min_ce_len = BLE_GAP_INITIAL_CONN_MIN_CE_LEN;
  cp.max_ce_len = BLE_GAP_INITIAL_CONN_MAX_CE_LEN;
  ble_gap_update_params(client->getConnId(), &cp);
#endif
  SH_LOCK();
  strlcpy(sh.name, foundName, sizeof(sh.name));
  if (!sh.haveMac) {                          // перша прив'язка → loop збереже у флеш
    strlcpy(sh.mac, foundAddr.toString().c_str(), sizeof(sh.mac));
    sh.haveMac = true;
    sh.newPair = true;
  }
  sh.telValid = false;
  SH_UNLOCK();
  savedAddr = foundAddr;
  return true;
}

void linkTask(void*) {
  BLEScan* scan = BLEDevice::getScan();
  static ScanCB scanCb;
  scan->setAdvertisedDeviceCallbacks(&scanCb, false, true);
  scan->setActiveScan(true);       // ім'я машинки приходить у scan response
  scan->setInterval(40);
  scan->setWindow(40);
  client = BLEDevice::createClient();

  TickType_t last = xTaskGetTickCount();
  uint32_t tRssi = 0, tRate = 0, sent = 0;
  uint8_t seq = 0;

  for (;;) {
    bool forget;
    SH_LOCK(); forget = sh.forget; sh.forget = false; SH_UNLOCK();
    if (forget) {
      if (client->isConnected()) client->disconnect();
      SH_LOCK(); sh.haveMac = false; sh.mac[0] = 0; sh.name[0] = 0; SH_UNLOCK();
      vTaskDelay(pdMS_TO_TICKS(300));
    }

    if (!client->isConnected()) {
      setState(LINK_SEARCH);
      SH_LOCK(); sh.rssi = -127; sh.txRate = 0; SH_UNLOCK();
      scanFound = false;
      foundRssi = -999;
      scan->start(1, false);                    // до 1 с; своя машинка зупиняє пошук раніше
      scan->clearResults();
      if (scanFound && connectFound()) {
        setState(LINK_READY);
        last = xTaskGetTickCount();
        tRssi = tRate = millis();
        sent = 0;
      } else {
        vTaskDelay(pdMS_TO_TICKS(100));
      }
      continue;
    }

    // --- підключено: пакет керування 50 разів на секунду ---
    SbCmd c;
    SH_LOCK(); c = sh.cmd; SH_UNLOCK();
    c.magic = SB_CMD_MAGIC;
    c.seq = seq++;
    c.sum = sbXor(&c, sizeof(c) - 1);
    if (chrCmd->writeValue((uint8_t*)&c, sizeof(c), false)) sent++;

    uint32_t now = millis();
    if (now - tRssi >= 500) {
      tRssi = now;
      int r = client->getRssi();
      if (r < 0) { SH_LOCK(); sh.rssi = r; SH_UNLOCK(); }     // 0 = помилка читання
    }
    if (now - tRate >= 1000) {
      uint16_t rate = (uint16_t)(sent * 1000UL / (now - tRate));
      tRate = now; sent = 0;
      SH_LOCK(); sh.txRate = rate; SH_UNLOCK();
    }
    vTaskDelayUntil(&last, pdMS_TO_TICKS(T_LINK));
  }
}

// =====================================================================
//  Стан пульта
// =====================================================================
Screen   screen = SCR_SPLASH;
uint32_t bootMs = 0;

bool    thrOnRight = false;
bool    mpuMode = false;
bool    mpuWait = false;
uint8_t debugCard = 0;
int     infoLine = 0;
int     outThr = 0, outSteer = 0;      // що йде на машинку (до лімітів), -100..100
bool    driveOn = false;

// USB / ПК (розділ «USB-контролер»)
bool     pcMode = false;               // пульт — лише контролер для ПК: машинка стоїть, кнопки не гортають меню
bool     usbStream = false;            // шлемо стан JSON-рядками
uint16_t pressCnt[4] = {0, 0, 0, 0};   // скільки разів натиснули B1, B2, лівий SW, правий SW (з увімкнення)
void pcModeSet(bool on);

// дані від linkTask (копія на цей цикл)
LinkState linkState = LINK_SEARCH;
bool      linkReady = false, announced = false;
int       rssi = -127;
uint16_t  txRate = 0;
bool      pairing = false;
char      carName[20] = "";
SbTel     tel = {};
bool      telFresh = false;

const char* toastText = nullptr;
uint32_t    toastUntil = 0;
void toast(const char* s, uint32_t ms = 1500) { toastText = s; toastUntil = millis() + ms; }

char macStr[18] = "--";

// =====================================================================
//  Калібрування (B2 2 с):
//   крок 1 — 0.8 с стіки відпущені, пульт нерухомо: центри осей + гіроскоп;
//   крок 2 — 5 с крутимо обидва стіки до упору по колу: крайні положення.
//  Результат зберігається у флеш. B1 — скасувати.
// =====================================================================
uint8_t  calStep = 0;            // 0 нема, 1 центр, 2 межі
uint32_t calT0 = 0;
Screen   calPrev = SCR_MAIN;
int32_t  calSum[4];
int      calLo[4], calHi[4], calC[4], calMn[4], calMx[4];
uint16_t calN = 0;

void startCalib(uint32_t now) {
  calPrev = screen;
  screen = SCR_CALIB;
  calStep = 1; calT0 = now; calN = 0;
  for (int i = 0; i < 4; i++) { calSum[i] = 0; calLo[i] = 4095; calHi[i] = 0; }
  mpu::startCalibration();
  play(MEL_MODE);
}

void endCalib(const char* msg, bool ok) {
  calStep = 0;
  screen = calPrev;
  toast(msg, 2000);
  play(ok ? MEL_OK : MEL_ERR);
}

void calibUpdate(uint32_t now) {
  if (calStep == 1) {
    for (int i = 0; i < 4; i++) {
      int r = AXES[i]->raw;
      calSum[i] += r;
      if (r < calLo[i]) calLo[i] = r;
      if (r > calHi[i]) calHi[i] = r;
    }
    calN++;
    if (now - calT0 < CAL_CENTER_MS) return;
    for (int i = 0; i < 4; i++) {
      calC[i] = calSum[i] / max<int>(1, calN);
      // тремтить — стік тримають; біля краю — стік відхилений
      if (calHi[i] - calLo[i] > 150 || calC[i] < 300 || calC[i] > 3800) { endCalib("Відпусти стіки!", false); return; }
      calMn[i] = calMx[i] = calC[i];
    }
    calStep = 2; calT0 = now;
    play(MEL_TICK);
  } else if (calStep == 2) {
    for (int i = 0; i < 4; i++) {
      int r = AXES[i]->raw;
      if (r < calMn[i]) calMn[i] = r;
      if (r > calMx[i]) calMx[i] = r;
    }
    if (now - calT0 < CAL_RANGE_MS) return;
    for (int i = 0; i < 4; i++)
      if (calMx[i] - calC[i] < CAL_MIN_TRAVEL || calC[i] - calMn[i] < CAL_MIN_TRAVEL) { endCalib("Мало руху, ще раз", false); return; }
    cfg.stick.magic = 0x5C01;
    for (int i = 0; i < 4; i++) {
      cfg.stick.c[i] = calC[i]; cfg.stick.mn[i] = calMn[i]; cfg.stick.mx[i] = calMx[i];
      AXES[i]->center = calC[i]; AXES[i]->minV = calMn[i]; AXES[i]->maxV = calMx[i];
    }
    saveStick();
    endCalib("Збережено", true);
  }
}

void forgetCar() {
  cfg.carMac[0] = 0;
  cfg.carName[0] = 0;
  saveCar();
  SH_LOCK(); sh.forget = true; SH_UNLOCK();
  toast("Пошук нової машинки", 2000);
  play(MEL_MODE);
}

// =====================================================================
//  Логіка (50 Гц)
// =====================================================================
uint32_t comboStart = 0;
bool     comboFired = false;

// «флік» стіка для меню: спрацьовує один раз при відхиленні > 60 %
struct Flick {
  bool armed = true;
  uint32_t tRep = 0;
  // повертає -1 / 0 / +1; repeatMs > 0 — автоповтор, поки тримаєш
  int update(int v, uint32_t now, uint32_t repeatMs = 0) {
    if (abs(v) < 30) { armed = true; return 0; }
    if (abs(v) < 60) return 0;
    if (armed) { armed = false; tRep = now + 350; return v > 0 ? 1 : -1; }
    if (repeatMs && (int32_t)(now - tRep) >= 0) { tRep = now + repeatMs; return v > 0 ? 1 : -1; }
    return 0;
  }
};
Flick flickX, flickY;

// зміна ліміту швидкості стіком: що сильніше відхилив — то швидше
struct SpeedAdj {
  uint32_t tNext = 0;
  bool step(int v, uint32_t now, uint8_t& val) {
    if (abs(v) < 40) { tNext = 0; return false; }
    if (tNext && (int32_t)(now - tNext) < 0) return false;
    tNext = now + (abs(v) > 85 ? 45 : 140);
    int nv = constrain((int)val + (v > 0 ? 1 : -1), (int)SPEED_MIN, 100);
    if (nv == val) return false;
    val = nv;
    return true;
  }
};
SpeedAdj adjL, adjR;
uint32_t speedTouched = 0;
bool     speedChanged = false;

void enterSpeed(uint32_t now) { screen = SCR_SPEED; speedTouched = now; speedChanged = false; play(MEL_MODE); }
void exitSpeed() {
  screen = SCR_MAIN;
  if (speedChanged) { saveSpeed(); toast("Збережено"); }
  play(MEL_OK);
}

void logicUpdate(uint32_t now) {
  j1x.read(); j1y.read(); j2x.read(); j2y.read();
  int lx = j1x.norm(), ly = j1y.norm(), rx = j2x.norm(), ry = j2y.norm();

  // ПК-режим: усі кнопки й стіки йдуть тільки в комп'ютер; вихід — B1 3 с
  if (pcMode && screen != SCR_CALIB) {
    if (btnB1.longEvent) pcModeSet(false);
    driveOn = false;
    outThr = outSteer = 0;
    SbCmd c = {};
    c.thrMax = cfg.thrMax; c.steerMax = cfg.steerMax;
    SH_LOCK(); sh.cmd = c; SH_UNLOCK();
    return;
  }

  // --- комбо SW+SW ---
  if (btnJ1.down && btnJ2.down) {
    btnJ1.tainted = btnJ2.tainted = true;
    if (!comboStart) comboStart = now;
    if (!comboFired && now - comboStart >= COMBO_MS && screen != SCR_SPLASH) {
      comboFired = true;
      if (screen == SCR_SPEED) exitSpeed(); else enterSpeed(now);
    }
  } else {
    comboStart = 0;
    if (!btnJ1.down && !btnJ2.down) comboFired = false;
  }

  // --- B2: стіки/MPU, довго — калібрування (працює в усіх меню) ---
  if (screen != SCR_SPLASH && screen != SCR_CALIB) {
    if (btnB2.longEvent && screen != SCR_SPEED) startCalib(now);
    else if (btnB2.click()) {
      if (!mpuMode && !mpu::ok) { toast("MPU не знайдено"); play(MEL_ERR); }
      else {
        mpuMode = !mpuMode;
        mpuWait = mpuMode;                // спершу пульт треба вирівняти
        play(MEL_MODE);
      }
    }
  }

  // --- екрани ---
  switch (screen) {
    case SCR_MAIN:
      if (btnB1.click()) { screen = SCR_DEBUG; play(MEL_TICK); }
      if (btnJ1.click() && thrOnRight)  { thrOnRight = false; play(MEL_TICK); }
      if (btnJ2.click() && !thrOnRight) { thrOnRight = true;  play(MEL_TICK); }
      break;
    case SCR_DEBUG: {
      if (btnB1.longEvent && debugCard == 0) forgetCar();
      else if (btnB1.click()) { screen = SCR_INFO; infoLine = 0; play(MEL_TICK); }
      int d = flickX.update(rx, now);
      if (d) { debugCard = (debugCard + ui::DEBUG_CARDS + d) % ui::DEBUG_CARDS; play(MEL_TICK); }
      break;
    }
    case SCR_INFO: {
      if (btnB1.click()) { screen = SCR_MAIN; play(MEL_TICK); }
      int d = flickY.update(ry, now, 160);
      if (d) infoLine = constrain(infoLine - d, 0, ui::infoMaxLine());   // стік вгору — текст вгору
      break;
    }
    case SCR_CALIB:
      if (btnB1.click()) { endCalib("Скасовано", false); break; }
      calibUpdate(now);
      break;
    case SCR_SPEED:
      if (btnB1.click()) { exitSpeed(); break; }
      if (adjL.step(ly, now, cfg.steerMax)) { speedChanged = true; speedTouched = now; }
      if (adjR.step(ry, now, cfg.thrMax))   { speedChanged = true; speedTouched = now; }
      if (abs(ly) > 40 || abs(ry) > 40) speedTouched = now;
      if (now - speedTouched > SPEED_IDLE_EXIT_MS) exitSpeed();
      break;
    default: break;
  }

  // --- керування ---
  int thr = 0, steer = 0;
  if (mpuMode) {
    if (mpu::calibrating) mpuWait = true;
    if (mpuWait && fabsf(mpu::pitch) < MPU_DEAD_DEG && fabsf(mpu::roll) < MPU_DEAD_DEG && !mpu::calibrating) mpuWait = false;
    if (!mpuWait) {
      float aThr   = MPU_SWAP_AXES ? mpu::roll : mpu::pitch;
      float aSteer = MPU_SWAP_AXES ? mpu::pitch : mpu::roll;
      thr   = tiltAxis(aThr, MPU_INV_THR);
      steer = tiltAxis(aSteer, MPU_INV_STEER);
    }
  } else {
    thr   = expo(thrOnRight ? ry : ly, STICK_EXPO);
    steer = expo(thrOnRight ? lx : rx, STICK_EXPO);
  }
  driveOn = linkReady && screen == SCR_MAIN;
  outThr   = driveOn ? thr : 0;
  outSteer = driveOn ? steer : 0;

  SbCmd c = {};
  c.thr = (int8_t)outThr;
  c.steer = (int8_t)(STEER_INVERT ? -outSteer : outSteer);
  c.thrMax = cfg.thrMax;
  c.steerMax = cfg.steerMax;
  c.flags = driveOn ? SB_CMD_DRIVE : 0;
  SH_LOCK(); sh.cmd = c; SH_UNLOCK();
}

// =====================================================================
//  Зв'язок: забираємо стан із linkTask, звуки підключення
// =====================================================================
uint32_t connectWatchMs = 0;

void linkPoll(uint32_t now) {
  bool newPair = false;
  char macCopy[18], nameCopy[20];
  uint32_t telMs, stMs;
  bool telValid, haveMac;
  SH_LOCK();
  linkState = sh.state; stMs = sh.stateMs;
  rssi = sh.rssi; txRate = sh.txRate;
  haveMac = sh.haveMac;
  if (sh.newPair) { newPair = true; sh.newPair = false; }
  strlcpy(macCopy, sh.mac, sizeof(macCopy));
  strlcpy(nameCopy, sh.name, sizeof(nameCopy));
  tel = sh.tel; telMs = sh.telMs; telValid = sh.telValid;
  SH_UNLOCK();

  linkReady = linkState == LINK_READY;
  pairing = !haveMac;
  if (nameCopy[0]) strlcpy(carName, nameCopy, sizeof(carName));
  else if (!haveMac) carName[0] = 0;
  telFresh = linkReady && telValid && ageMs(now, telMs) < 1500;

  if (newPair) {                         // перша прив'язка — у флеш
    strlcpy(cfg.carMac, macCopy, sizeof(cfg.carMac));
    strlcpy(cfg.carName, nameCopy, sizeof(cfg.carName));
    saveCar();
    toast("Машинку прив'язано");
  }

  // підключення зависло (машинку вимкнули саме тоді) — скасовуємо через 3 с
  if (linkState == LINK_CONNECTING && ageMs(now, stMs) > 3000 && ageMs(now, connectWatchMs) > 3000) {
    connectWatchMs = now;
#if defined(CONFIG_NIMBLE_ENABLED)
    ble_gap_conn_cancel();
#endif
  }

  // пік «є зв'язок» / «зв'язок втрачено» (після заставки, щоб не перебити її мелодію)
  if (screen != SCR_SPLASH) {
    if (linkReady && !announced)      { play(MEL_CONN); announced = true; }
    else if (!linkReady && announced) { play(MEL_LOST); announced = false; }
  }
}

// =====================================================================
//  Розряд: значок мигає (робить ui), а бузер нагадує раз на 30 с
// =====================================================================
uint32_t lastLowBeep = 0;
bool     wasLow = false;

void alertsUpdate(uint32_t now) {
  bool carLow = telFresh && (tel.flags & (SB_TEL_LOW | SB_TEL_CRITICAL));
  bool low = txLow || carLow;
  if (screen == SCR_SPLASH) return;
  if (low && (!wasLow || now - lastLowBeep >= LOW_BEEP_EVERY_MS)) {
    if (!soundBusy()) { play(MEL_LOW); lastLowBeep = now; wasLow = true; }
  }
  if (!low) wasLow = false;
}

// =====================================================================
//  Екран
// =====================================================================
uint8_t oledContrastNow = OLED_CONTRAST;
void setOledLevel(float k) {             // 0..1, квадрат — щоб на око згасало рівно
  uint8_t c = (uint8_t)lroundf(OLED_CONTRAST * k * k);
  if (c != oledContrastNow) { oledContrastNow = c; oled.setContrast(c); }
}

void drawScreen(uint32_t now) {
  uint32_t ts = now - bootMs;
  if (screen == SCR_SPLASH) setOledLevel(ui::splashLevel(ts));
  else if (ts < ui::SPLASH_MS + FADE_IN_MS) setOledLevel(ui::easeOutCubic(ui::seg(ts, ui::SPLASH_MS, ui::SPLASH_MS + FADE_IN_MS)));
  else setOledLevel(1.0f);

  UiModel m;
  m.ms = now;
  m.connected = linkReady;
  m.pairing = pairing;
  m.rssi = rssi;
  m.carName = carName;
  m.txV = vbatV;  m.txPinMv = vbatPinMv;
  m.txPct = liionPercent(vbatV);
  m.txLow = txLow;
  m.carKnown = telFresh;
  if (telFresh) {
    m.carV = tel.vbatMv / 1000.0f;
    m.carPct = tel.pct;
    m.carLow = tel.flags & (SB_TEL_LOW | SB_TEL_CRITICAL);
    m.carRxRate = tel.rxRate;
    m.carFwMaj = tel.fwMajor; m.carFwMin = tel.fwMinor;
  }
  m.thr = outThr * cfg.thrMax / 100;
  m.steer = outSteer * cfg.steerMax / 100;
  m.mpuMode = mpuMode;  m.mpuWait = mpuWait;
  m.thrOnRight = thrOnRight;
  m.thrMax = cfg.thrMax;  m.steerMax = cfg.steerMax;
  m.debugCard = debugCard;
  m.infoLine = infoLine;
  m.j1x = j1x.norm(); m.j1y = j1y.norm(); m.j2x = j2x.norm(); m.j2y = j2y.norm();
  m.pitch = mpu::pitch; m.roll = mpu::roll; m.mpuOk = mpu::ok;
  m.txRate = linkReady ? txRate : 0;
  m.heapK = ESP.getFreeHeap() / 1024;
  m.uptimeS = now / 1000;
  m.mac = macStr;
  m.usb = usbStream;
  m.pc = pcMode;

  oled.clearBuffer();
  switch (screen) {
    case SCR_SPLASH: ui::drawSplash(oled, ts); break;
    case SCR_MAIN:   m.menu = 0; ui::drawMain(oled, m); break;
    case SCR_DEBUG:  m.menu = 1; ui::drawDebug(oled, m); break;
    case SCR_INFO:   m.menu = 2; ui::drawInfo(oled, m); break;
    case SCR_SPEED:  ui::drawSpeed(oled, m); break;
    case SCR_CALIB: {
      m.calibStep = calStep;
      uint32_t dur = calStep == 1 ? CAL_CENTER_MS : CAL_RANGE_MS;
      m.calibPct = (uint8_t)min<uint32_t>(100, (now - calT0) * 100 / dur);
      // положення стіків 0..1000 для квадратиків (вгору і вправо — з урахуванням інверсій)
      for (int k = 0; k < 2; k++) {
        const Axis* ax = AXES[k * 2];
        const Axis* ay = AXES[k * 2 + 1];
        int px = ax->raw * 1000L / 4095, py = ay->raw * 1000L / 4095;
        m.calX[k] = ax->inv ? 1000 - px : px;
        m.calY[k] = ay->inv ? 1000 - py : py;
      }
      ui::drawCalib(oled, m);
      break;
    }
  }
  if (toastText && (int32_t)(now - toastUntil) < 0 && screen != SCR_SPLASH) ui::drawToast(oled, toastText);
  else toastText = nullptr;
  oled.sendBuffer();
}

// =====================================================================
//  Serial-консоль (без блокування)
// =====================================================================
bool monitorOn = false;
char cmdBuf[40];
uint8_t cmdLen = 0;

// =====================================================================
//  USB-контролер · протокол SBJ v1
// ---------------------------------------------------------------------
//  Звичайний USB CDC (COM-порт), швидкість будь-яка (115200 ок).
//  Комп'ютер → пульт: текстові команди, кожна закінчується \n
//    hello            → {"type":"hello",...}  хто я, версія
//    stream on [Гц]   → безперервно шле {"type":"state",...} (10..100, типово 50)
//    stream off       → зупинити
//    state            → один рядок state
//    pc on | pc off   → ПК-режим: машинка стоїть, кнопки й стіки — тільки для ПК
//    beep <Гц> <мс>   → пікнути бузером (зворотний зв'язок у грі)
//  Пульт → комп'ютер: JSON, один об'єкт на рядок. Рядки, що не починаються
//  з «{», — звичайний текст консолі, їх треба пропускати.
//  Якщо комп'ютер 3 с не забирає дані (програму закрили) — стрім і ПК-режим
//  вимикаються самі. Запис неблокуючий: пульт ніколи не чекає на USB.
// =====================================================================
const uint8_t  SBJ_PROTO = 1;
uint16_t usbHz = 50;
uint32_t usbNext = 0, usbFailSince = 0, usbSeq = 0;
char     jbuf[768];

bool usbWrite(const char* s, int n) {
  if (n <= 0 || n >= (int)sizeof(jbuf)) return false;
  if (Serial.availableForWrite() < n) return false;
  return Serial.write((const uint8_t*)s, n) == (size_t)n;
}

const char* screenName() {
  switch (screen) {
    case SCR_SPLASH: return "splash";
    case SCR_MAIN:   return "main";
    case SCR_DEBUG:  return "debug";
    case SCR_INFO:   return "info";
    case SCR_SPEED:  return "speed";
    case SCR_CALIB:  return "calib";
  }
  return "?";
}
const char* linkName() { return linkReady ? "ready" : linkState == LINK_CONNECTING ? "connecting" : "search"; }

// назва машинки без лапок і керуючих символів — щоб JSON лишався валідним
void jsonSafe(char* dst, const char* src, size_t n) {
  size_t i = 0;
  for (; src[i] && i < n - 1; i++) {
    char c = src[i];
    dst[i] = (c == '"' || c == '\\' || (uint8_t)c < 0x20) ? '_' : c;
  }
  dst[i] = 0;
}
float finite(float v) { return isfinite(v) ? v : 0.0f; }

int buildState(uint32_t now) {
  char car[160], name[20];
  if (telFresh)
    snprintf(car, sizeof(car), "{\"v\":%.2f,\"pct\":%u,\"low\":%d,\"flags\":%u,\"rx\":%u,\"l\":%d,\"r\":%d,\"fw\":\"%u.%u\"}",
             tel.vbatMv / 1000.0f, tel.pct, (tel.flags & (SB_TEL_LOW | SB_TEL_CRITICAL)) ? 1 : 0, tel.flags,
             tel.rxRate, tel.motL, tel.motR, tel.fwMajor, tel.fwMinor);
  else strcpy(car, "null");
  jsonSafe(name, carName, sizeof(name));
  return snprintf(jbuf, sizeof(jbuf),
    "{\"type\":\"state\",\"seq\":%lu,\"t\":%lu,"
    "\"lx\":%d,\"ly\":%d,\"rx\":%d,\"ry\":%d,\"raw\":[%d,%d,%d,%d],"
    "\"b1\":%d,\"b2\":%d,\"ls\":%d,\"rs\":%d,\"presses\":[%u,%u,%u,%u],"
    "\"mpu\":{\"ok\":%d,\"pitch\":%.1f,\"roll\":%.1f,\"ax\":%.3f,\"ay\":%.3f,\"az\":%.3f,\"gx\":%.1f,\"gy\":%.1f,\"gz\":%.1f},"
    "\"mode\":\"%s\",\"thrRight\":%d,\"screen\":\"%s\",\"pc\":%d,"
    "\"out\":{\"thr\":%d,\"steer\":%d,\"drive\":%d,\"thrMax\":%u,\"steerMax\":%u},"
    "\"bat\":{\"v\":%.2f,\"pct\":%u,\"low\":%d},"
    "\"link\":{\"state\":\"%s\",\"rssi\":%d,\"rate\":%u,\"car\":\"%s\"},"
    "\"car\":%s}\n",
    (unsigned long)usbSeq++, (unsigned long)now,
    j1x.norm(), j1y.norm(), j2x.norm(), j2y.norm(), j1x.raw, j1y.raw, j2x.raw, j2y.raw,
    btnB1.down, btnB2.down, btnJ1.down, btnJ2.down, pressCnt[0], pressCnt[1], pressCnt[2], pressCnt[3],
    mpu::ok, finite(mpu::pitch), finite(mpu::roll), finite(mpu::axG), finite(mpu::ayG), finite(mpu::azG),
    finite(mpu::gxD), finite(mpu::gyD), finite(mpu::gzD),
    mpuMode ? "mpu" : "sticks", thrOnRight, screenName(), pcMode,
    outThr, outSteer, driveOn, cfg.thrMax, cfg.steerMax,
    vbatV, liionPercent(vbatV), txLow,
    linkName(), linkReady ? rssi : 0, linkReady ? txRate : 0, name,
    car);
}

void usbHello() {
  int n = snprintf(jbuf, sizeof(jbuf),
    "{\"type\":\"hello\",\"dev\":\"SoccerBee-TX\",\"proto\":%u,\"fw\":\"%s\",\"hw\":\"%s\",\"mac\":\"%s\",\"hz\":%u,\"stream\":%d,\"pc\":%d}\n",
    SBJ_PROTO, SB_TX_FW, SB_TX_HW, macStr, usbHz, usbStream, pcMode);
  usbWrite(jbuf, n);
}
void usbAck(const char* cmd, bool ok) {
  int n = snprintf(jbuf, sizeof(jbuf), "{\"type\":\"ack\",\"cmd\":\"%s\",\"ok\":%d,\"stream\":%d,\"hz\":%u,\"pc\":%d}\n",
                   cmd, ok, usbStream, usbHz, pcMode);
  usbWrite(jbuf, n);
}

void pcModeSet(bool on) {
  if (pcMode == on) return;
  pcMode = on;
  if (on) {
    if (screen == SCR_SPEED) exitSpeed();
    if (screen == SCR_DEBUG || screen == SCR_INFO) screen = SCR_MAIN;
    toast("ПК-режим", 1200);
    play(MEL_MODE);
  } else {
    toast("Пульт керує машинкою", 1500);
    play(MEL_OK);
  }
}

void usbStreamSet(bool on, uint32_t now) {
  usbStream = on;
  usbNext = now;
  usbFailSince = 0;
  if (on) monitorOn = false;               // текстовий монітор заважав би JSON
  else pcModeSet(false);                   // програма відключилась — пульт знову для машинки
}

void usbUpdate(uint32_t now) {
  if (!usbStream || (int32_t)(now - usbNext) < 0) return;
  uint32_t period = 1000 / usbHz;
  usbNext += period;
  if ((int32_t)(now - usbNext) > (int32_t)period) usbNext = now + period;   // відстали — без «наздоганяння»
  int n = buildState(now);
  if (usbWrite(jbuf, n)) { usbFailSince = 0; return; }
  if (!usbFailSince) usbFailSince = now;
  else if (now - usbFailSince > 3000) usbStreamSet(false, now);           // ніхто не читає
}

// команди SBJ; true — команду впізнано
bool usbCommand(const char* line, const char* arg, uint32_t now) {
  if (!strcmp(line, "hello")) { usbHello(); return true; }
  if (!strcmp(line, "state")) { usbWrite(jbuf, buildState(now)); return true; }
  if (!strcmp(line, "stream")) {
    if (arg && !strncmp(arg, "off", 3)) { usbStreamSet(false, now); usbAck("stream", true); return true; }
    if (arg && !strncmp(arg, "on", 2)) {
      int hz = atoi(arg + 2);
      if (hz > 0) usbHz = constrain(hz, 10, 100);
      usbStreamSet(true, now);
      usbHello();
      return true;
    }
    usbAck("stream", false);
    return true;
  }
  if (!strcmp(line, "pc")) {
    bool ok = arg && (!strcmp(arg, "on") || !strcmp(arg, "off"));
    if (ok) pcModeSet(!strcmp(arg, "on"));
    usbAck("pc", ok);
    return true;
  }
  if (!strcmp(line, "beep")) {
    int f = 0, ms = 0;
    bool ok = arg && sscanf(arg, "%d %d", &f, &ms) == 2 && ms > 0;
    if (ok) {
      beepNote[0].f = constrain(f, 0, 8000);
      beepNote[0].ms = constrain(ms, 1, 2000);
      play(beepNote);
    }
    usbAck("beep", ok);
    return true;
  }
  return false;
}


void printHelp() {
  Serial.println(F(
    "\n----------- SOCCER BEE · ПУЛЬТ -----------\n"
    "h            — ця довідка\n"
    "info         — стан пульта і зв'язку\n"
    "mon          — увімк/вимк вивід 2 рази/с\n"
    "buz p | buz a — тип бузера: пасивний / активний\n"
    "cal 3.95     — калібрування VBAT (напруга з мультиметра)\n"
    "speed 80 60  — ліміти: газ поворот, %\n"
    "sticks       — калібрування стіків (центр і межі кожної осі)\n"
    "unpair       — забути машинку, шукати нову\n"
    "reset        — скинути всі налаштування\n"
    "--- USB-контролер (JSON, протокол SBJ v1) ---\n"
    "hello | state | stream on [Гц] | stream off\n"
    "pc on | pc off | beep <Гц> <мс>\n"
    "------------------------------------------"));
}

void printInfo() {
  Serial.printf("TX Firmware V%s, MAC пульта %s (лише для довідки, вводити нікуди не треба)\n", SB_TX_FW, macStr);
  Serial.printf("Батарея %.2f В (%u%%)%s, K=%.4f, пін %d мВ\n", vbatV, liionPercent(vbatV), txLow ? " НИЗЬКА" : "", cfg.vbatK, vbatPinMv);
  Serial.printf("Зв'язок: %s, RSSI %d, TX %u/с, машинка \"%s\" %s\n",
                linkReady ? "є" : (linkState == LINK_CONNECTING ? "підключаюсь" : "пошук"), rssi, txRate,
                carName, cfg.carMac[0] ? cfg.carMac : "(будь-яка)");
  if (telFresh)
    Serial.printf("Машинка: %.2f В (%u%%) flags 0x%02X, RX %u/с, мотори L%d R%d, fw %u.%u\n",
                  tel.vbatMv / 1000.0f, tel.pct, tel.flags, tel.rxRate, tel.motL, tel.motR, tel.fwMajor, tel.fwMinor);
  Serial.printf("Ліміти: газ %u%%, поворот %u%%; режим %s; бузер %s; MPU %s\n", cfg.thrMax, cfg.steerMax,
                mpuMode ? "MPU" : "стіки", cfg.buzPassive ? "пасивний" : "активний", mpu::ok ? "OK" : "немає");
}

void handleCommand(char* line) {
  char* arg = strchr(line, ' ');
  if (arg) { *arg++ = 0; while (*arg == ' ') arg++; }
  if (usbCommand(line, arg, millis())) return;
  if (!strcmp(line, "h") || !strcmp(line, "?")) printHelp();
  else if (!strcmp(line, "info")) printInfo();
  else if (!strcmp(line, "mon") && !usbStream) { monitorOn = !monitorOn; Serial.printf("Моніторинг: %s\n", monitorOn ? "ON" : "OFF"); }
  else if (!strcmp(line, "buz") && arg && (*arg == 'p' || *arg == 'a')) {
    soundPause();
    buzOut(0);
    mel = nullptr; melPending = nullptr;
    cfg.buzPassive = *arg == 'p';
    saveBuzzer();
    buzSetup();
    soundStart();
    play(MEL_OK);
    Serial.printf("Бузер: %s (збережено)\n", cfg.buzPassive ? "пасивний" : "активний");
  }
  else if (!strcmp(line, "cal") && arg) {
    float real = atof(arg);
    float shown = vbatPinMv * cfg.vbatK / 1000.0f;            // без поправки VBAT_CORR_V
    if (real > 2.5f && real < 4.5f && shown > 1.0f) {
      cfg.vbatK *= (real - VBAT_CORR_V) / shown;
      vbatV = real;
      saveVbatK();
      Serial.printf("VBAT_K = %.4f (збережено)\n", cfg.vbatK);
    } else Serial.println("Формат: cal 3.95");
  }
  else if (!strcmp(line, "speed") && arg) {
    int t = 0, s = 0;
    if (sscanf(arg, "%d %d", &t, &s) == 2) {
      cfg.thrMax = constrain(t, SPEED_MIN, 100);
      cfg.steerMax = constrain(s, SPEED_MIN, 100);
      saveSpeed();
      Serial.printf("Ліміти: газ %u%%, поворот %u%% (збережено)\n", cfg.thrMax, cfg.steerMax);
    } else Serial.println("Формат: speed 80 60");
  }
  else if (!strcmp(line, "sticks")) {
    static const char* const N[4] = {"лівий X", "лівий Y", "правий X", "правий Y"};
    Serial.printf("Калібрування стіків: %s\n", cfg.stick.magic == 0x5C01 ? "збережене" : "немає (центр з увімкнення, межі 0..4095)");
    for (int i = 0; i < 4; i++)
      Serial.printf("  %-9s зараз %4d | мін %4d  центр %4d  макс %4d | %+4d%%\n", N[i], AXES[i]->raw, AXES[i]->minV, AXES[i]->center, AXES[i]->maxV, AXES[i]->norm());
  }
  else if (!strcmp(line, "unpair")) { forgetCar(); Serial.println("Прив'язку скинуто, шукаю найближчу машинку"); }
  else if (!strcmp(line, "reset")) {
    prefs.begin("sbtx", false); prefs.clear(); prefs.end();
    Serial.println("Налаштування скинуто, перезапуск...");
    Serial.flush();
    ESP.restart();
  }
  else Serial.println("Невідома команда, h — довідка");
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
  // першим ділом «мінус» правого стіка і бузер — у LOW
  pinMode(PIN_J2GND, OUTPUT);
  digitalWrite(PIN_J2GND, LOW);
  pinMode(PIN_BUZ, OUTPUT);
  digitalWrite(PIN_BUZ, LOW);

  Serial.setTxBufferSize(4096);       // вміщує кілька JSON-рядків, запис ніколи не блокує
  Serial.begin(115200);
  bootMs = millis();
  loadSettings();

  btnB1.begin(PIN_B1, B1_LONG_MS);
  btnB2.begin(PIN_B2, B2_LONG_MS);
  btnJ1.begin(PIN_J1SW, CLICK_MAX_MS);   // довше 0.6 с — уже не клік
  btnJ2.begin(PIN_J2SW, CLICK_MAX_MS);
  j1x.pin = PIN_J1X; j1x.inv = INV_J1X;
  j1y.pin = PIN_J1Y; j1y.inv = INV_J1Y;
  j2x.pin = PIN_J2X; j2x.inv = INV_J2X;
  j2y.pin = PIN_J2Y; j2y.inv = INV_J2Y;

  analogReadResolution(12);
  analogSetAttenuation(ADC_11db);

  Wire.begin(PIN_SDA, PIN_SCL, 400000);
  Wire.beginTransmission(0x3C);
  if (Wire.endTransmission() != 0) oled.setI2CAddress(0x3D * 2);
  oled.setBusClock(400000);
  oled.begin();
  mpu::begin();                  // калібрування гіроскопа йде, поки грає заставка
  applyBootCenters();            // збережене калібрування + уточнення центрів
  readVbat();

  oled.setContrast(0);           // заставка «проявляється» з темряви
  oledContrastNow = 0;
  buzSetup();
  soundStart();
  play(MEL_START);

  uint64_t e = ESP.getEfuseMac();
  snprintf(macStr, sizeof(macStr), "%02X:%02X:%02X:%02X:%02X:%02X", (uint8_t)e, (uint8_t)(e >> 8),
           (uint8_t)(e >> 16), (uint8_t)(e >> 24), (uint8_t)(e >> 32), (uint8_t)(e >> 40));

  BLEDevice::init("SoccerBee-TX");
  BLEDevice::setPower(BLE_POWER);
  if (cfg.carMac[0]) {
    savedAddr = BLEAddress(String(cfg.carMac));
    sh.haveMac = true;
    strlcpy(sh.mac, cfg.carMac, sizeof(sh.mac));
    strlcpy(sh.name, cfg.carName, sizeof(sh.name));
    strlcpy(carName, cfg.carName, sizeof(carName));
  }
  xTaskCreate(linkTask, "sbLink", 8192, nullptr, 1, nullptr);
}

void loop() {
  const uint32_t now = millis();
  static uint32_t tInput = 0, tMpu = 0, tVbat = 0, tOled = 0, tMon = 0;
  static bool welcomed = false;

  pollSerial();

  if (screen == SCR_SPLASH && now - bootMs >= ui::SPLASH_MS) screen = SCR_MAIN;

  if (now - tMpu >= T_MPU)     { tMpu = now;   mpu::update(); }
  if (now - tInput >= T_INPUT) {
    tInput = now;
    // кнопки опитуємо в тому ж такті, що й логіку: подія «клік» живе рівно один такт
    btnB1.update(now); btnB2.update(now); btnJ1.update(now); btnJ2.update(now);
    if (btnB1.pressed) pressCnt[0]++;
    if (btnB2.pressed) pressCnt[1]++;
    if (btnJ1.pressed) pressCnt[2]++;
    if (btnJ2.pressed) pressCnt[3]++;
    linkPoll(now);
    logicUpdate(now);
    alertsUpdate(now);
  }
  if (now - tVbat >= T_VBAT)   { tVbat = now;  readVbat(); }
  if (now - tOled >= (screen == SCR_SPLASH ? T_SPLASH : T_OLED)) { tOled = now; drawScreen(now); }
  usbUpdate(now);

  if (!welcomed && now - bootMs > 1500) {      // USB CDC піднімається не одразу
    welcomed = true;
    if (!usbStream) {                           // програма на ПК уже читає JSON — текст не шлемо
      Serial.printf("\n=== SOCCER BEE · ПУЛЬТ · TX Firmware V%s ===\n", SB_TX_FW);
      printInfo();
      Serial.println("h — список команд");
    }
  }
  if (monitorOn && now - tMon >= 500) {
    tMon = now;
    Serial.printf("L(%+4d,%+4d) R(%+4d,%+4d) P%+6.1f R%+6.1f | газ %+4d пов %+4d %s | RSSI %d TX %u/с | пульт %.2fВ",
                  j1x.norm(), j1y.norm(), j2x.norm(), j2y.norm(), mpu::pitch, mpu::roll, outThr, outSteer,
                  driveOn ? "DRIVE" : "stop", rssi, txRate, vbatV);
    if (telFresh) Serial.printf(" | машинка %.2fВ RX %u/с L%+d R%+d", tel.vbatMv / 1000.0f, tel.rxRate, tel.motL, tel.motR);
    Serial.println();
  }
  delay(1);   // віддаємо процесор задачам BLE і idle; уся логіка все одно на таймерах millis()
}
