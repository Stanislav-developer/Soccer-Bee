// =====================================================================
//  Soccer Bee · протокол пульт ↔ машинка (BLE)
//  ЦЕЙ ФАЙЛ ОДНАКОВИЙ у папках SoccerBee_TX і SoccerBee_Car.
//  Змінив тут — скопіюй і туди, інакше вони не зрозуміють одне одного.
// =====================================================================
#pragma once
#include <stdint.h>

#define SB_SERVICE_UUID "7a1c1000-2b5e-4c3a-9d10-5f6e7a8b9c01"
#define SB_CMD_UUID     "7a1c1001-2b5e-4c3a-9d10-5f6e7a8b9c01"   // пульт → машинка, write without response
#define SB_TEL_UUID     "7a1c1002-2b5e-4c3a-9d10-5f6e7a8b9c01"   // машинка → пульт, notify
#define SB_NAME_PREFIX  "SoccerBee-"                              // + 4 hex-цифри MAC машинки

const uint8_t SB_CMD_MAGIC = 0xB5;
const uint8_t SB_TEL_MAGIC = 0xB6;

// флаги команди
const uint8_t SB_CMD_DRIVE = 0x01;   // 1 = можна їхати; 0 = стоп (пульт не в основному меню тощо)

// флаги телеметрії
const uint8_t SB_TEL_LOW      = 0x01;   // акумулятор машинки розряджений (≤ 6.8 В)
const uint8_t SB_TEL_CRITICAL = 0x02;   // дуже розряджений (≤ 6.4 В)
const uint8_t SB_TEL_FAILSAFE = 0x04;   // машинка стоїть через відсутність команд

// Пакет керування, 8 байт, 50 разів на секунду
struct __attribute__((packed)) SbCmd {
  uint8_t magic;      // SB_CMD_MAGIC
  uint8_t seq;        // лічильник пакетів
  int8_t  thr;        // газ -100..100 (+ вперед)
  int8_t  steer;      // поворот -100..100 (+ вправо)
  uint8_t thrMax;     // ліміт газу, % (0..100)
  uint8_t steerMax;   // ліміт повороту, %
  uint8_t flags;      // SB_CMD_*
  uint8_t sum;        // XOR попередніх 7 байт
};

// Телеметрія, 12 байт, 5 разів на секунду
struct __attribute__((packed)) SbTel {
  uint8_t  magic;     // SB_TEL_MAGIC
  uint8_t  seq;
  uint16_t vbatMv;    // напруга акумулятора машинки, мВ
  uint8_t  pct;       // заряд, %
  uint8_t  flags;     // SB_TEL_*
  uint8_t  rxRate;    // скільки команд отримано за останню секунду
  int8_t   motL;      // фактичний вихід на лівий борт -100..100
  int8_t   motR;      // правий борт
  uint8_t  fwMajor;   // версія прошивки машинки
  uint8_t  fwMinor;
  uint8_t  sum;       // XOR попередніх 11 байт
};

static_assert(sizeof(SbCmd) == 8, "SbCmd must be 8 bytes");
static_assert(sizeof(SbTel) == 12, "SbTel must be 12 bytes");

inline uint8_t sbXor(const void* p, uint8_t n) {
  const uint8_t* b = (const uint8_t*)p;
  uint8_t x = 0;
  for (uint8_t i = 0; i < n; i++) x ^= b[i];
  return x;
}
