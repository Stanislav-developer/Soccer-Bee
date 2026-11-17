// =====================================================================
//  Soccer Bee · Пульт · ЕКРАНИ (OLED 128x64, U8g2)  — дизайн v2
//  Мінімалістичний інтерфейс, читабельні шрифти (6x12 з усіма
//  українськими літерами). Тільки малювання: дані приходять у UiModel.
//  Той самий код компілюється і на пульті, і на ПК для макетів.
// =====================================================================
#pragma once
#include <U8g2lib.h>
#include <math.h>

#define SB_TX_FW "1.1"     // версія прошивки пульта: на екрані «V1.0»
#define SB_TX_HW "TX v2"   // версія плати пульта

struct UiModel {
  // зв'язок
  bool     connected = false;
  bool     pairing = false;      // шукаємо нову машинку (прив'язки нема)
  int      rssi = -127;          // дБм
  uint32_t ms = 0;               // millis() для анімацій
  const char* carName = "";      // "SoccerBee-1A2B"
  uint8_t  carFwMaj = 0, carFwMin = 0;
  const char* toast = nullptr;   // коротке повідомлення поверх екрана
  // батареї
  uint8_t  txPct = 0;    bool txLow = false;
  float    txV = 0;      int  txPinMv = 0;
  bool     carKnown = false;
  uint8_t  carPct = 0;   bool carLow = false;
  float    carV = 0;
  // керування
  int      thr = 0, steer = 0;   // -100..100, те що йде на машинку
  bool     mpuMode = false;      // false = стіки
  bool     mpuWait = false;      // MPU увімкнено, але пульт ще не вирівняли
  bool     thrOnRight = false;   // газ на правому стіку
  uint8_t  thrMax = 80, steerMax = 60;
  // меню
  uint8_t  menu = 0;             // 0 основне, 1 дебаг, 2 інфо
  uint8_t  debugCard = 0;        // картка дебагу
  int      infoLine = 0;         // прокрутка інфо, у рядках
  // дебаг
  int      j1x = 0, j1y = 0, j2x = 0, j2y = 0;   // стіки -100..100 (вже з інверсією)
  float    pitch = 0, roll = 0;
  bool     mpuOk = false;
  uint16_t txRate = 0, carRxRate = 0;
  uint32_t heapK = 0, uptimeS = 0;
  // калібрування
  uint8_t  calibStep = 0, calibPct = 0;
  int      calX[2] = {500, 500}, calY[2] = {500, 500};   // положення стіків 0..1000
  const char* fw = SB_TX_FW;
  const char* hw = SB_TX_HW;
  const char* mac = "--";
  // USB
  bool     usb = false;          // програма на ПК читає стан (stream on)
  bool     pc = false;           // ПК-режим: пульт — лише контролер для комп'ютера
};

namespace ui {

const uint8_t MENUS = 3;
const uint8_t DEBUG_CARDS = 5;
const uint8_t INFO_VISIBLE = 4;     // рядків інфо на екрані

// ---------- шрифти ----------
#define UI_FONT_TEXT  u8g2_font_6x12_t_cyrillic   // основний текст: є і, ї, є, ґ
#define UI_FONT_SMALL u8g2_font_5x7_t_cyrillic    // цифри в статус-рядку
#define UI_FONT_BIG   u8g2_font_helvB12_tr        // OFFLINE, назва
#define UI_FONT_NUM   u8g2_font_7x13B_tr          // великі числа

// ---------- піктограми (XBM, LSB = лівий піксель) ----------
// джойстик 7x7
static const uint8_t ICON_JOY[] PROGMEM = {0x0C, 0x0C, 0x08, 0x08, 0x3E, 0x7F, 0x3E};
// машинка 11x7 (вигляд збоку)
static const uint8_t ICON_CAR[] PROGMEM = {0xF8, 0x00, 0x04, 0x01, 0xFE, 0x03, 0xFF, 0x07, 0xFF, 0x07, 0x06, 0x03, 0x06, 0x03};

inline bool blink(uint32_t ms, uint16_t period = 400) { return (ms / period) % 2 == 0; }

template <class G>
void textC(G& g, int cx, int y, const char* s) { g.drawUTF8(cx - g.getUTF8Width(s) / 2, y, s); }
template <class G>
void textR(G& g, int rx, int y, const char* s) { g.drawUTF8(rx - g.getUTF8Width(s), y, s); }

// товста лінія: чотирикутник з двох трикутників
template <class G>
void thickLine(G& g, float x0, float y0, float x1, float y1, float t) {
  float dx = x1 - x0, dy = y1 - y0;
  float len = sqrtf(dx * dx + dy * dy);
  if (len < 0.01f) return;
  float nx = -dy / len * t * 0.5f, ny = dx / len * t * 0.5f;
  int ax = lroundf(x0 + nx), ay = lroundf(y0 + ny);
  int bx = lroundf(x1 + nx), by = lroundf(y1 + ny);
  int cx = lroundf(x1 - nx), cy = lroundf(y1 - ny);
  int ex = lroundf(x0 - nx), ey = lroundf(y0 - ny);
  g.drawTriangle(ax, ay, bx, by, cx, cy);
  g.drawTriangle(ax, ay, cx, cy, ex, ey);
  g.drawLine(lroundf(x0), lroundf(y0), lroundf(x1), lroundf(y1));   // без щілин по діагоналі
}

// ---------------------------------------------------------------------
//  Логотип «злий робот»: брови, очі, рот-шеврон (як на капоті)
//  cx, cy — центр очей; s — масштаб (1.0 ≈ 64 px завширшки)
//  brow 0..1 — наскільки «насуплені» брови; eyeR — радіус очей (0 = нема)
//  mouth 0..1 — скільки рота домальовано; eyeOpen 0..1 — моргання (0 = закриті)
// ---------------------------------------------------------------------
template <class G>
void logo(G& g, int cx, int cy, float s, float brow, float eyeR, float mouth, float eyeOpen = 1.0f,
          float browLen = 1.0f) {
  // брови: внутрішній кінець опускається від рівня зовнішнього до «злого» кута;
  // browLen 0..1 — брова «виростає» від перенісся назовні
  float outY = -17.0f * s, inY = outY + 6.0f * s * brow;
  float yOff = -2.0f * s * (1.0f - brow);                      // ще не насуплені — трохи вище
  if (browLen > 0.02f) {
    float k = browLen > 1 ? 1 : browLen;
    float ix = 8 * s, ox = ix + 23 * s * k;
    float oy = inY + (outY - inY) * k;                          // точка на лінії брови
    thickLine(g, cx - ox, cy + oy + yOff, cx - ix, cy + inY + yOff, 3.6f * s);
    thickLine(g, cx + ix, cy + inY + yOff, cx + ox, cy + oy + yOff, 3.6f * s);
  }
  // очі: eyeOpen 1 → коло, 0 → закрите (риска); між ними — сплющений еліпс
  int r = lroundf(eyeR * s);
  int ry = lroundf(eyeR * s * eyeOpen);
  if (eyeR > 0.5f && ry < 1) {
    thickLine(g, cx - 20 * s - r, cy, cx - 20 * s + r, cy, 2.0f * s);
    thickLine(g, cx + 20 * s - r, cy, cx + 20 * s + r, cy, 2.0f * s);
  } else if (eyeR > 0.5f && ry < r) {
    g.drawFilledEllipse(cx - lroundf(20 * s), cy, r, ry);
    g.drawFilledEllipse(cx + lroundf(20 * s), cy, r, ry);
  } else if (eyeR > 0.5f) {
    g.drawDisc(cx - lroundf(20 * s), cy, r);
    g.drawDisc(cx + lroundf(20 * s), cy, r);
  }
  // рот-шеврон: росте від центру в сторони
  if (mouth > 0.01f) {
    float k = mouth > 1 ? 1 : mouth;
    float px = cx, py = cy + 7.0f * s;
    float lx = cx - 18.5f * s * k, ly = cy + (7.0f + 7.5f * k) * s;
    float rx = cx + 18.5f * s * k;
    thickLine(g, lx, ly, px, py, 3.0f * s);
    thickLine(g, px, py, rx, ly, 3.0f * s);
  }
}

// ---------- батарейка 13x7 з 4 поділками; низький заряд — порожня з «!» і мигає ----------
template <class G>
void battery(G& g, int x, int y, uint8_t pct, bool low, uint32_t ms) {
  if (low && !blink(ms)) return;
  g.drawFrame(x, y, 11, 7);
  g.drawBox(x + 11, y + 2, 2, 3);
  if (low) { g.drawVLine(x + 5, y + 1, 3); g.drawPixel(x + 5, y + 5); return; }
  int seg = pct >= 88 ? 4 : pct >= 63 ? 3 : pct >= 38 ? 2 : pct >= 13 ? 1 : 0;
  for (int i = 0; i < seg; i++) g.drawBox(x + 2 + i * 2, y + 2, 1, 3);
}

// ---------- рівень сигналу: 4 стовпчики ----------
template <class G>
void signal(G& g, int x, int y, int rssi, bool connected) {
  int bars = !connected ? 0 : rssi >= -60 ? 4 : rssi >= -70 ? 3 : rssi >= -80 ? 2 : rssi >= -90 ? 1 : 0;
  for (int i = 0; i < 4; i++) {
    int h = 2 + i * 2;
    if (i < bars) g.drawBox(x + i * 3, y + 8 - h, 2, h);
    else          g.drawHLine(x + i * 3, y + 7, 2);
  }
}

// ---------- статус-рядок: пульт · машинка · сигнал ----------
template <class G>
void statusBar(G& g, const UiModel& m) {
  char s[8];
  g.setFont(UI_FONT_SMALL);
  g.drawXBMP(0, 1, 7, 7, ICON_JOY);
  battery(g, 9, 1, m.txPct, m.txLow, m.ms);
  snprintf(s, sizeof(s), "%u%%", m.txPct);
  g.drawStr(24, 8, s);

  g.drawXBMP(48, 1, 11, 7, ICON_CAR);
  if (m.connected && m.carKnown) {
    battery(g, 61, 1, m.carPct, m.carLow, m.ms);
    snprintf(s, sizeof(s), "%u%%", m.carPct);
    g.drawStr(76, 8, s);
  } else {
    g.drawStr(62, 8, "--");
  }

  if (m.connected) {
    signal(g, 98, 0, m.rssi, true);
    snprintf(s, sizeof(s), "%d", m.rssi);
    g.drawStr(128 - g.getStrWidth(s), 8, s);
  } else {
    signal(g, 98, 0, 0, false);
  }
}

// ---------- стрілка напрямку, що «наповнюється» від основи до вершини ----------
// dir: 0 вгору, 1 вниз, 2 вліво, 3 вправо; f = 0..1 (частка газу/повороту)
template <class G>
void arrowFill(G& g, int cx, int cy, uint8_t dir, float f) {
  const int HW = 10, D = 10;                  // напівширина основи і глибина
  const int GV = 9, GH = 13;                  // відступ від центру: по вертикалі / горизонталі
  int fill = (int)(f * D + 0.5f);
  if (f > 0.02f && fill < 1) fill = 1;
  for (int k = 0; k <= D; k++) {              // k — відстань від основи
    int hw = HW * (D - k) / D;
    bool full = k < fill;
    if (dir <= 1) {
      int y = dir == 0 ? cy - GV - k : cy + GV + k;
      if (full || k == 0) g.drawHLine(cx - hw, y, 2 * hw + 1);
      else { g.drawPixel(cx - hw, y); g.drawPixel(cx + hw, y); }
    } else {
      int x = dir == 2 ? cx - GH - k : cx + GH + k;
      if (full || k == 0) g.drawVLine(x, cy - hw, 2 * hw + 1);
      else { g.drawPixel(x, cy - hw); g.drawPixel(x, cy + hw); }
    }
  }
}

// позначка «газ на цьому стіку»: вертикальна стрілка ↕ 7x9
template <class G>
void thrMark(G& g, int x, int y) {
  g.drawTriangle(x, y + 3, x + 6, y + 3, x + 3, y);
  g.drawTriangle(x, y + 6, x + 6, y + 6, x + 3, y + 9);
  g.drawVLine(x + 3, y + 3, 4);
}

// =====================================================================
//  1. Основний екран
// =====================================================================
template <class G>
void drawMain(G& g, const UiModel& m) {
  statusBar(g, m);
  if (m.pc) {                                                  // ПК-режим
    g.setFont(UI_FONT_BIG);
    textC(g, 64, 33, "PC");
    g.setFont(UI_FONT_TEXT);
    textC(g, 64, 47, "USB-контролер");
    if (m.usb) {                                               // «живий» індикатор потоку
      int k = (m.ms / 120) % 6;
      for (int i = 0; i < 3; i++) g.drawBox(44 + i * 3, 25 - (i == k % 3 ? 2 : 0), 2, 2);
      for (int i = 0; i < 3; i++) g.drawBox(78 + i * 3, 25 - (i == k % 3 ? 2 : 0), 2, 2);
    }
    textC(g, 64, 63, "B1 3с: вихід");
    return;
  }
  if (!m.connected) {
    g.setFont(UI_FONT_BIG);
    textC(g, 64, 38, "OFFLINE");
    for (int i = 0; i < 3; i++) {                              // «біжучі» крапки пошуку
      int x = 58 + i * 6;
      if ((int)((m.ms / 250) % 4) > i) g.drawDisc(x, 49, 1); else g.drawPixel(x, 49);
    }
  } else {
    // 4 стрілки: наповнення = сила газу/повороту
    const int cx = 64, cy = 33;
    arrowFill(g, cx, cy, 0, m.thr > 0 ? m.thr / 100.0f : 0);
    arrowFill(g, cx, cy, 1, m.thr < 0 ? -m.thr / 100.0f : 0);
    arrowFill(g, cx, cy, 2, m.steer < 0 ? -m.steer / 100.0f : 0);
    arrowFill(g, cx, cy, 3, m.steer > 0 ? m.steer / 100.0f : 0);
    g.drawDisc(cx, cy, 2);
  }
  // низ: режим по центру, стрілка «газ» з боку стіка з газом
  g.setFont(UI_FONT_TEXT);
  if (m.mpuMode && m.mpuWait) { if (blink(m.ms, 300)) textC(g, 64, 63, "ВИРІВНЯЙ"); }
  else textC(g, 64, 63, m.mpuMode ? "MPU" : "СТІКИ");
  if (!m.mpuMode) thrMark(g, m.thrOnRight ? 121 : 0, 54);
  if (m.usb) {                                                 // дані йдуть на ПК
    g.setFont(UI_FONT_SMALL);
    g.drawStr((m.mpuMode || m.thrOnRight) ? 0 : 113, 63, "USB");
  }
}

// коротке повідомлення в рамці посередині екрана
template <class G>
void drawToast(G& g, const char* s) {
  g.setFont(UI_FONT_TEXT);
  int w = g.getUTF8Width(s) + 12;
  if (w > 126) w = 126;
  int x = 64 - w / 2;
  g.setDrawColor(0);
  g.drawBox(x - 1, 22, w + 2, 21);
  g.setDrawColor(1);
  g.drawRFrame(x, 23, w, 19, 3);
  textC(g, 64, 36, s);
}

// =====================================================================
//  Швидкість (обидві SW 1 с): лівий стік — поворот, правий — газ
// =====================================================================
template <class G>
void titleBar(G& g, const char* title, const char* right) {
  g.drawBox(0, 0, 128, 12);
  g.setDrawColor(0);
  g.setFont(UI_FONT_TEXT);
  g.drawUTF8(2, 10, title);
  if (right) textR(g, 126, 10, right);
  g.setDrawColor(1);
}

template <class G>
void speedBar(G& g, int x, int y, int w, int h, uint8_t pct) {
  g.drawFrame(x, y, w, h);
  int fh = (h - 4) * pct / 100;
  g.drawBox(x + 2, y + h - 2 - fh, w - 4, fh);
}

template <class G>
void drawSpeed(G& g, const UiModel& m) {
  titleBar(g, "ШВИДКІСТЬ", nullptr);
  speedBar(g, 6, 16, 14, 47, m.steerMax);
  speedBar(g, 108, 16, 14, 47, m.thrMax);
  char s[8];
  g.setFont(UI_FONT_NUM);
  snprintf(s, sizeof(s), "%u%%", m.steerMax);
  g.drawStr(26, 38, s);
  snprintf(s, sizeof(s), "%u%%", m.thrMax);
  g.drawStr(102 - g.getStrWidth(s), 38, s);
  g.setFont(UI_FONT_TEXT);
  g.drawUTF8(26, 54, "поворот");
  textR(g, 102, 54, "газ");
}

// «Бульбашковий рівень» для картки ГІРОСКОП: зовнішнє коло = 60°,
// внутрішнє = 30° (мертва зона MPU: поки точка всередині — машинка стоїть)
template <class G>
void mpuBubble(G& g, float pitch, float roll) {
  const int cx = 101, cy = 38, r = 23;
  const float FULL = 60.0f, DEAD = 30.0f;
  g.drawCircle(cx, cy, r);
  int rd = lroundf((r - 3) * DEAD / FULL);
  for (int a = 0; a < 360; a += 15) {                 // пунктирне коло мертвої зони
    float t = a * 0.0174533f;
    g.drawPixel(cx + lroundf(rd * cosf(t)), cy + lroundf(rd * sinf(t)));
  }
  g.drawHLine(cx - 2, cy, 5);
  g.drawVLine(cx, cy - 2, 5);
  float px = roll > FULL ? FULL : roll < -FULL ? -FULL : roll;
  float py = pitch > FULL ? FULL : pitch < -FULL ? -FULL : pitch;
  int bx = cx + lroundf(px * (r - 3) / FULL);
  int by = cy - lroundf(py * (r - 3) / FULL);
  g.drawDisc(bx, by, 3);
}

// =====================================================================
//  2. Дебаг: 5 карток по 4 рядки, перемикання правим стіком
// =====================================================================
template <class G>
void drawDebug(G& g, const UiModel& m) {
  static const char* const TITLES[DEBUG_CARDS] = {"ЗВ'ЯЗОК", "ЖИВЛЕННЯ", "СТІКИ", "ГІРОСКОП", "СИСТЕМА"};
  char l[4][48] = {{0}};   // UTF-8: кирилиця по 2 байти
  uint8_t c = m.debugCard % DEBUG_CARDS;
  switch (c) {
    case 0:
      snprintf(l[0], 48, m.connected ? "BLE: підключено" : m.pairing ? "BLE: пошук нової" : "BLE: пошук...");
      if (m.connected) snprintf(l[1], 48, "RSSI: %d дБм", m.rssi);
      else             snprintf(l[1], 48, "RSSI: --");
      if (m.connected) snprintf(l[2], 48, "TX %u/с RX %u/с", m.txRate, m.carRxRate);
      else             snprintf(l[2], 48, "TX -- RX --");
      snprintf(l[3], 48, "B1 3с: нова пара");
      break;
    case 1:
      snprintf(l[0], 48, "Пульт  %.2fВ %3u%%", m.txV, m.txPct);
      if (m.carKnown) snprintf(l[1], 48, "Машина %.2fВ %3u%%", m.carV, m.carPct);
      else            snprintf(l[1], 48, "Машина --");
      snprintf(l[2], 48, "АЦП пульта %dмВ", m.txPinMv);
      snprintf(l[3], 48, "Пороги 3.4 / 6.8В");
      break;
    case 2:
      snprintf(l[0], 48, "Лів  X%+4d Y%+4d", m.j1x, m.j1y);
      snprintf(l[1], 48, "Прав X%+4d Y%+4d", m.j2x, m.j2y);
      snprintf(l[2], 48, "Газ %+4d Пов %+4d", m.thr, m.steer);
      snprintf(l[3], 48, "Ліміт %u%% / %u%%", m.thrMax, m.steerMax);
      break;
    case 3:
      // ліворуч текст (до 12 символів), праворуч «бульбашка»
      snprintf(l[0], 48, "MPU-6050: %s", m.mpuOk ? "OK" : "немає");
      snprintf(l[1], 48, "P %+6.1f", m.pitch);
      snprintf(l[2], 48, "R %+6.1f", m.roll);
      snprintf(l[3], 48, "Режим: %s", m.mpuMode ? "MPU" : "стіки");
      break;
    default: {
      uint32_t up = m.uptimeS;
      snprintf(l[0], 48, "TX Firmware V%s", m.fw);
      if (m.carFwMaj || m.carFwMin) snprintf(l[1], 48, "Robot Firmware V%u.%u", m.carFwMaj, m.carFwMin);
      else                          snprintf(l[1], 48, "Robot Firmware --");
      snprintf(l[2], 48, "%s", m.mac);
      // вільна оперативна пам'ять (КБ) і час роботи від увімкнення
      snprintf(l[3], 48, "RAM %luK %02lu:%02lu:%02lu", (unsigned long)m.heapK, (unsigned long)(up / 3600),
               (unsigned long)(up / 60 % 60), (unsigned long)(up % 60));
    }
  }
  char idx[6];
  snprintf(idx, sizeof(idx), "%u/%u", c + 1, DEBUG_CARDS);
  titleBar(g, TITLES[c], idx);
  g.setFont(UI_FONT_TEXT);
  for (int i = 0; i < 4; i++) g.drawUTF8(1, 24 + i * 13, l[i]);
  if (c == 3 && m.mpuOk) mpuBubble(g, m.pitch, m.roll);
}

// =====================================================================
//  Калібрування стіків: 2 кроки, квадратики з положенням стіків, смуга часу
// =====================================================================
template <class G>
void drawCalib(G& g, const UiModel& m) {
  titleBar(g, "КАЛІБРУВАННЯ", m.calibStep == 1 ? "1/2" : "2/2");
  g.setFont(UI_FONT_TEXT);
  if (m.calibStep == 1) {
    g.drawUTF8(1, 25, "Відпусти стіки");
    g.drawUTF8(1, 38, "і не рухай пульт");
  } else {
    g.drawUTF8(1, 25, "Покрути стіки");
    g.drawUTF8(1, 38, "до упору по колу");
  }
  for (int k = 0; k < 2; k++) {
    int x0 = k == 0 ? 36 : 72, y0 = 41, s = 20;
    g.drawFrame(x0, y0, s, s);
    int px = x0 + 1 + (s - 5) * m.calX[k] / 1000;
    int py = y0 + 1 + (s - 5) * (1000 - m.calY[k]) / 1000;
    g.drawBox(px, py, 3, 3);
  }
  g.drawBox(0, 62, 128 * m.calibPct / 100, 2);
}

// =====================================================================
//  3. Інфо (українською): прокрутка правим стіком по рядку
// =====================================================================
static const char* const INFO_LINES[] = {
  "#SOCCER BEE",
  "Robot for",
  "Robofootball",
  "#КОМАНДА",
  "Kep Robotics",
  "x C.C.C.",
  "#КЕРУВАННЯ",
  "Лівий стік: газ",
  "Правий: поворот",
  "Клік стіка: газ",
  "переходить на нього",
  "B1: наступне меню",
  "B2: стіки / MPU",
  "B2 2с: калібрування",
  "SW+SW 1с: швидкість",
  "#РЕЖИМ MPU",
  "Нахил понад 30 град",
  "рушає машинку.",
  "Вперед-назад: газ,",
  "вбік: поворот",
  "#ПРИВ'ЯЗКА",
  "Пульт пам'ятає",
  "свою машинку.",
  "Нова: дебаг, картка",
  "ЗВ'ЯЗОК, B1 3 с",
  "#ЖИВЛЕННЯ",
  "Пульт: від 3.4 В",
  "Машинка: від 6.8 В",
  "Заряд через USB-C",
  "#USB / ПК",
  "USB-C до комп'ютера:",
  "пульт працює як",
  "контролер в іграх",
  "і веб-додатках.",
  "Вихід з ПК: B1 3 с",
  "#ВЕРСІЯ",
  "TX Firmware V" SB_TX_FW,
};
const int INFO_N = sizeof(INFO_LINES) / sizeof(INFO_LINES[0]);
inline int infoMaxLine() { return INFO_N - INFO_VISIBLE; }

template <class G>
void drawInfo(G& g, const UiModel& m) {
  titleBar(g, "ІНФО", nullptr);
  int first = constrain(m.infoLine, 0, infoMaxLine());
  g.setFont(UI_FONT_TEXT);
  for (int i = 0; i < INFO_VISIBLE; i++) {
    const char* s = INFO_LINES[first + i];
    int y = 24 + i * 13;
    if (s[0] == '#') {                              // заголовок розділу: рамка на всю ширину
      g.drawFrame(0, y - 10, 122, 13);
      textC(g, 61, y, s + 1);
    } else {
      g.drawUTF8(1, y, s);
    }
  }
  // смуга прокрутки
  int track = 64 - 14;
  int barH = max(5, track * INFO_VISIBLE / INFO_N);
  int barY = 14 + (track - barH) * first / max(1, infoMaxLine());
  for (int y = 14; y < 64; y += 2) g.drawPixel(126, y);
  g.drawBox(125, barY, 3, barH);
}

// =====================================================================
//  Заставка 2.5 с: обличчя «оживає», назва виїжджає, блік, моргання,
//  у кінці — плавне згасання. Анімація на кривих (easing), ~30 кадрів/с.
// =====================================================================
const uint32_t SPLASH_MS = 2500;

inline float clamp01(float x) { return x < 0 ? 0 : x > 1 ? 1 : x; }
inline float seg(uint32_t t, uint32_t a, uint32_t b) { return clamp01((float)((int32_t)(t - a)) / (float)(b - a)); }
inline float easeOutCubic(float x) { float k = 1 - x; return 1 - k * k * k; }
inline float easeInCubic(float x) { return x * x * x; }
inline float easeInOutCubic(float x) { return x < 0.5f ? 4 * x * x * x : 1 - powf(-2 * x + 2, 3) / 2; }
inline float easeOutBack(float x) {                       // з невеликим «перельотом»
  const float c1 = 1.70158f, c3 = c1 + 1;
  float k = x - 1;
  return 1 + c3 * k * k * k + c1 * k * k;
}

// яскравість екрана під час заставки 0..1 (на пульті — через контраст OLED)
inline float splashLevel(uint32_t t) {
  if (t < 300) return easeOutCubic(seg(t, 0, 300));
  if (t > SPLASH_MS - 320) return 1 - easeInCubic(seg(t, SPLASH_MS - 320, SPLASH_MS));
  return 1;
}

template <class G>
void drawSplash(G& g, uint32_t t) {
  // 1) очі «вистрибують» (0–450 мс)
  float eye = 7.2f * easeOutBack(seg(t, 0, 450));
  // 2) брови виростають від перенісся і насуплюються (300–800)
  float browLen = easeOutCubic(seg(t, 300, 620));
  float brow    = easeInOutCubic(seg(t, 420, 820));
  // 3) рот-шеврон розкривається (700–1050)
  float mouth = easeOutCubic(seg(t, 700, 1050));
  // 6) моргання (1800–2000): швидко закрити, плавно відкрити
  float open = 1;
  if (t >= 1800 && t < 1870) open = 1 - easeInCubic(seg(t, 1800, 1870));
  else if (t >= 1870 && t < 2010) open = easeOutCubic(seg(t, 1870, 2010));
  logo(g, 64, 20, 1.0f, brow, eye, mouth, open, browLen);

  // 4) назва виїжджає знизу (1000–1450)
  if (t >= 1000) {
    g.setFont(UI_FONT_BIG);
    float k = easeOutCubic(seg(t, 1000, 1450));
    int y = 52 + lroundf((1 - k) * 18);
    textC(g, 64, y, "Soccer Bee");
    // 7) блік: темна смуга пробігає по назві (1550–1850)
    if (t >= 1550 && t < 1850) {
      int w = g.getStrWidth("Soccer Bee");
      int x0 = 64 - w / 2 - 6;
      int bx = x0 + lroundf(easeInOutCubic(seg(t, 1550, 1850)) * (w + 12));
      g.setDrawColor(0);
      for (int i = 0; i < 13; i++) {                       // нахилена смуга 2 px
        int yy = 52 - i, xx = bx + i / 3;
        g.drawHLine(xx, yy, 2);
      }
      g.setDrawColor(1);
    }
  }
  // 5) підпис розкривається від центру (1350–1750)
  if (t >= 1350) {
    g.setFont(u8g2_font_5x7_tf);
    float k = easeOutCubic(seg(t, 1350, 1750));
    int half = lroundf(k * 64);   // вікно в межах 0..128, інакше u8g2 його відкидає
    if (half > 0) {
      g.setClipWindow(64 - half, 55, 64 + half, 64);
      textC(g, 64, 63, "Kep Robotics x C.C.C.");
      g.setMaxClipWindow();
    }
  }
}

}  // namespace ui
