#include <Arduino.h>
#include <Wire.h>
#include <math.h>
#include <algorithm>
#include <U8g2lib.h>
#include "Arduino_GFX_Library.h"
#include <TinyGPSPlus.h>
#include "TouchDrvCSTXXX.hpp"
#include <BLEDevice.h>
#include <BLEServer.h>
#include <BLEUtils.h>
#include <esp_heap_caps.h>
#include <Update.h>
#include <ESP_I2S.h>
#define XPOWERS_CHIP_AXP2101
#include "XPowersLib.h"
#include <host/ble_hs.h>
#include <host/ble_gap.h>
#include <host/ble_gatt.h>

#ifndef FW_VERSION
#define FW_VERSION 0
#endif

#define LCD_SDIO0 4
#define LCD_SDIO1 5
#define LCD_SDIO2 6
#define LCD_SDIO3 7
#define LCD_SCLK 38
#define LCD_CS 12
#define LCD_RESET 39
#define LCD_W 466
#define LCD_H 466
#define IIC_SDA 15
#define IIC_SCL 14
#define TP_INT 11
#define TP_RESET 40
#define BOOT_BTN 0

#define GPS_ADDR_W 0x50
#define GPS_ADDR_R 0x54
#define GPS_CHUNK 2048
#define TZ_OFFSET_H -3

#define DEVICE_NAME "MotoNav"
#define SERVICE_UUID "6e0a0001-7a3b-4c2d-9e1f-0b5a4d3c2b1a"
#define NAV_CHAR_UUID "6e0a0002-7a3b-4c2d-9e1f-0b5a4d3c2b1a"
#define MAP_CHAR_UUID "6e0a0003-7a3b-4c2d-9e1f-0b5a4d3c2b1a"
#define INFO_CHAR_UUID "6e0a0004-7a3b-4c2d-9e1f-0b5a4d3c2b1a"
#define OTA_CHAR_UUID "6e0a0005-7a3b-4c2d-9e1f-0b5a4d3c2b1a"
#define SLEEP_AFTER_MS 30000

#define CX (LCD_W / 2)
#define NAV_TIMEOUT_MS 20000
#define MAP_CY 330
#define CHUNK_PTS 32

#define C_BG RGB565_BLACK
#define C_FG RGB565_WHITE
#define C_DIM 0x8410
#define C_ACCENT 0x07FF
#define C_GREEN 0x47E8
#define C_ORANGE 0xFD20
#define C_MAP_BG (nightMode ? 0x0000 : 0x0841)
#define C_RED 0xF800
#define C_YELLOW 0xFFE0
#define C_PANEL 0x0000
#define C_ROUTE (nightMode ? 0x1A54 : 0x2D7F)
#define C_ROUTE_EDGE 0x0010

enum Maneuver : uint8_t {
  M_NONE = 0,
  M_STRAIGHT = 1,
  M_SLIGHT_RIGHT = 2,
  M_RIGHT = 3,
  M_SHARP_RIGHT = 4,
  M_SLIGHT_LEFT = 5,
  M_LEFT = 6,
  M_SHARP_LEFT = 7,
  M_UTURN = 8,
  M_ROUNDABOUT = 9,
  M_ARRIVE = 10,
  M_DEPART = 11,
  M_KEEP_RIGHT = 12,
  M_KEEP_LEFT = 13
};

struct NavData {
  bool active;
  uint8_t maneuver;
  uint8_t exitNumber;
  int16_t angle;
  uint32_t distM;
  uint32_t remainM;
  uint16_t remainMin;
  uint8_t speedKmh;
  uint8_t flags;
  char text[128];
};

struct GpsData {
  bool timeValid;
  uint8_t hour;
  uint8_t minute;
  bool fix;
  float kmph;
  int sats;
};

struct PosTarget {
  bool valid;
  uint16_t mapId;
  float x;
  float y;
  float headingDeg;
  float speed;
  float along;
  uint32_t ms;
};

struct AlertData {
  uint8_t type;
  int16_t angle;
  uint16_t dist;
  uint32_t ms;
};

struct StatusData {
  uint8_t limit;
  uint16_t radarDist;
  uint32_t ms;
  char street[64];
};

struct Toast {
  bool active;
  bool call;
  uint32_t uid;
  uint32_t until;
  char app[24];
  char title[64];
  char msg[128];
};

struct Music {
  bool available;
  bool playing;
  char title[64];
  char artist[64];
};

struct Chunk {
  int32_t minX, minY, maxX, maxY;
  uint32_t start;
  uint16_t count;
  uint8_t kind;
};

struct Lines {
  int32_t *px;
  int32_t *py;
  uint32_t nPts;
  Chunk *chunks;
  uint32_t nChunks;
  uint32_t kindStart[6];
  int32_t minX, minY, maxX, maxY;
};

struct RouteData {
  Lines lines;
  float *rx;
  float *ry;
  float *rcum;
  uint32_t n;
};

#define MAX_TILES 48

struct Tile {
  bool used;
  int16_t tx;
  int16_t ty;
  Lines lines;
};

struct World {
  bool hasSession;
  uint16_t session;
  RouteData route;
  Tile tiles[MAX_TILES];
};

struct View {
  float px, py;
  float cosH, sinH;
  float mpp;
};

struct Tap {
  bool tapped;
  bool button;
  int16_t x;
  int16_t y;
};

struct GattCtx {
  int status;
  uint16_t start, end;
  int nWant;
  ble_uuid128_t want[4];
  uint16_t found[4];
  uint16_t cccd;
};

struct RxItem {
  uint16_t len;
  uint8_t data[250];
};

Arduino_DataBus *bus = new Arduino_ESP32QSPI(LCD_CS, LCD_SCLK, LCD_SDIO0, LCD_SDIO1, LCD_SDIO2, LCD_SDIO3);
Arduino_CO5300 *panel = new Arduino_CO5300(bus, LCD_RESET, 0, LCD_W, LCD_H, 6, 0, 0, 0);
Arduino_Canvas *gfx = new Arduino_Canvas(LCD_W, LCD_H, panel);
TinyGPSPlus gps;
TouchDrvCST92xx touch;
bool touchOk = false;

portMUX_TYPE navMux = portMUX_INITIALIZER_UNLOCKED;
portMUX_TYPE gpsMux = portMUX_INITIALIZER_UNLOCKED;
portMUX_TYPE posMux = portMUX_INITIALIZER_UNLOCKED;
portMUX_TYPE alertMux = portMUX_INITIALIZER_UNLOCKED;
SemaphoreHandle_t i2cMutex;
SemaphoreHandle_t mapMutex;
QueueHandle_t rxQueue;

NavData navShared = {};
GpsData gpsShared = {};
PosTarget posShared = {};
AlertData alertShared = {};
bool approaching = false;
uint32_t approachStart = 0;
World world = {};
volatile uint32_t lastNavMs = 0;
volatile bool navDirty = true;
volatile bool bleConnected = false;
volatile int mapProgress = -1;
volatile bool touchPending = false;
bool nightMode = false;

enum : uint8_t { SCR_MAP = 0, SCR_ARROW = 1, SCR_MUSIC = 2, SCR_IDLE = 3 };
uint8_t screenIdx = 0;
uint8_t lastMode = 255;

XPowersPMU pmu;
bool pmuOk = false;
volatile int batteryPct = -1;
volatile bool charging = false;

portMUX_TYPE statusMux = portMUX_INITIALIZER_UNLOCKED;
portMUX_TYPE toastMux = portMUX_INITIALIZER_UNLOCKED;
portMUX_TYPE musicMux = portMUX_INITIALIZER_UNLOCKED;
StatusData statusShared = {};
Toast toastShared = {};
Music musicShared = {};
uint8_t setBrightnessVal = 255;
uint8_t setNight = 0;
uint8_t setNotify = 1;
volatile uint32_t settingsMs = 0;
uint8_t appliedBrightness = 0;

BLECharacteristic *infoChar = nullptr;
volatile uint16_t connHandle = 0xFFFF;
volatile bool linkEncrypted = false;
SemaphoreHandle_t gattSem;
SemaphoreHandle_t encSem;

volatile bool otaActive = false;
volatile int otaProgress = 0;
uint32_t otaSize = 0;
uint32_t otaGot = 0;

uint32_t lastActivity = 0;
bool screenOff = false;

uint8_t *rxBuf = nullptr;
uint32_t rxTotal = 0;
uint32_t rxGot = 0;
uint16_t rxId = 0;

bool i2cWrite(uint8_t addr, const uint8_t *data, size_t len) {
  Wire.beginTransmission(addr);
  Wire.write(data, len);
  return Wire.endTransmission() == 0;
}

bool i2cRead(uint8_t addr, uint8_t *data, size_t len) {
  if (Wire.requestFrom(addr, len) != len) return false;
  for (size_t i = 0; i < len; i++) data[i] = Wire.read();
  return true;
}

bool gpsStep(const uint8_t *cmd, size_t cmdLen, uint8_t *out, size_t outLen) {
  xSemaphoreTake(i2cMutex, portMAX_DELAY);
  bool ok = i2cWrite(GPS_ADDR_W, cmd, cmdLen);
  xSemaphoreGive(i2cMutex);
  if (!ok) return false;
  vTaskDelay(pdMS_TO_TICKS(100));
  if (!out) return true;
  xSemaphoreTake(i2cMutex, portMAX_DELAY);
  ok = i2cRead(GPS_ADDR_R, out, outLen);
  xSemaphoreGive(i2cMutex);
  return ok;
}

void readGps() {
  const uint8_t askLen[] = { 0x08, 0x00, 0x51, 0xAA, 0x04, 0x00, 0x00, 0x00 };
  uint8_t lenBuf[4] = { 0 };
  if (!gpsStep(askLen, sizeof(askLen), lenBuf, sizeof(lenBuf))) return;
  uint32_t len = lenBuf[0] | (lenBuf[1] << 8) | (lenBuf[2] << 16) | ((uint32_t)lenBuf[3] << 24);
  if (len == 0) return;
  if (len > GPS_CHUNK) len = GPS_CHUNK;
  const uint8_t askData[] = { 0x00, 0x20, 0x51, 0xAA, (uint8_t)(len & 0xFF), (uint8_t)((len >> 8) & 0xFF), 0x00, 0x00 };
  static uint8_t data[GPS_CHUNK];
  if (!gpsStep(askData, sizeof(askData), data, len)) return;
  for (uint32_t i = 0; i < len; i++) gps.encode(data[i]);
}

void readBattery() {
  if (!pmuOk) return;
  xSemaphoreTake(i2cMutex, portMAX_DELAY);
  bool conn = pmu.isBatteryConnect();
  int pct = conn ? pmu.getBatteryPercent() : -1;
  bool chg = pmu.isCharging();
  xSemaphoreGive(i2cMutex);
  batteryPct = pct;
  charging = chg;
}

void gpsTask(void *) {
  uint32_t lastBatt = 0;
  for (;;) {
    if (millis() - lastBatt > 30000 || lastBatt == 0) {
      readBattery();
      lastBatt = millis() | 1;
    }
    readGps();
    GpsData g;
    g.timeValid = gps.time.isValid();
    g.hour = (gps.time.hour() + 24 + TZ_OFFSET_H) % 24;
    g.minute = gps.time.minute();
    g.fix = gps.location.isValid() && gps.location.age() < 3000;
    g.kmph = gps.speed.isValid() ? gps.speed.kmph() : 0;
    g.sats = gps.satellites.isValid() ? gps.satellites.value() : 0;
    portENTER_CRITICAL(&gpsMux);
    gpsShared = g;
    portEXIT_CRITICAL(&gpsMux);
    vTaskDelay(pdMS_TO_TICKS(200));
  }
}

static inline uint16_t rd16(const uint8_t *p) {
  return p[0] | (p[1] << 8);
}

static inline uint32_t rd32(const uint8_t *p) {
  return p[0] | (p[1] << 8) | (p[2] << 16) | ((uint32_t)p[3] << 24);
}

void freeLines(Lines &l) {
  if (l.px) heap_caps_free(l.px);
  if (l.py) heap_caps_free(l.py);
  if (l.chunks) heap_caps_free(l.chunks);
  l = {};
}

void freeRoute(RouteData &r) {
  freeLines(r.lines);
  if (r.rx) heap_caps_free(r.rx);
  if (r.ry) heap_caps_free(r.ry);
  if (r.rcum) heap_caps_free(r.rcum);
  r = {};
}

void *psAlloc(size_t bytes) {
  return heap_caps_malloc(bytes > 0 ? bytes : 4, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
}

bool parseLines(const uint8_t *b, uint32_t len, uint32_t off, Lines &out, RouteData *route) {
  if (off + 2 > len) return false;
  uint16_t count = rd16(b + off);
  off += 2;
  uint32_t start = off;
  uint32_t totalPts = 0, totalChunks = 0, routePts = 0;
  for (uint16_t i = 0; i < count; i++) {
    if (off + 11 > len) return false;
    uint8_t kind = b[off];
    uint16_t n = rd16(b + off + 1);
    uint32_t need = 11 + (uint32_t)(n > 0 ? n - 1 : 0) * 4;
    if (n == 0 || off + need > len || kind > 4) return false;
    totalPts += n;
    if (n >= 2) totalChunks += (n - 2) / (CHUNK_PTS - 1) + 1;
    if (kind == 0 && routePts == 0) routePts = n;
    off += need;
  }
  Lines r = {};
  r.px = (int32_t *)psAlloc(totalPts * sizeof(int32_t));
  r.py = (int32_t *)psAlloc(totalPts * sizeof(int32_t));
  r.chunks = (Chunk *)psAlloc(totalChunks * sizeof(Chunk));
  if (!r.px || !r.py || !r.chunks) {
    freeLines(r);
    return false;
  }
  if (route) {
    route->rx = (float *)psAlloc(routePts * sizeof(float));
    route->ry = (float *)psAlloc(routePts * sizeof(float));
    route->rcum = (float *)psAlloc(routePts * sizeof(float));
    if (!route->rx || !route->ry || !route->rcum) {
      freeLines(r);
      freeRoute(*route);
      return false;
    }
  }
  r.minX = r.minY = INT32_MAX;
  r.maxX = r.maxY = INT32_MIN;
  off = start;
  uint32_t pi = 0, ci = 0;
  bool routeDone = false;
  for (uint16_t i = 0; i < count; i++) {
    uint8_t kind = b[off];
    uint16_t n = rd16(b + off + 1);
    int32_t x = (int32_t)rd32(b + off + 3);
    int32_t y = (int32_t)rd32(b + off + 7);
    off += 11;
    uint32_t base = pi;
    for (uint16_t k = 0; k < n; k++) {
      if (k > 0) {
        x += (int16_t)rd16(b + off);
        y += (int16_t)rd16(b + off + 2);
        off += 4;
      }
      r.px[pi] = x;
      r.py[pi] = y;
      if (x < r.minX) r.minX = x;
      if (x > r.maxX) r.maxX = x;
      if (y < r.minY) r.minY = y;
      if (y > r.maxY) r.maxY = y;
      pi++;
    }
    if (route && kind == 0 && !routeDone) {
      routeDone = true;
      route->n = n;
      float cum = 0;
      for (uint16_t k = 0; k < n; k++) {
        route->rx[k] = r.px[base + k];
        route->ry[k] = r.py[base + k];
        if (k > 0) {
          float dx = route->rx[k] - route->rx[k - 1], dy = route->ry[k] - route->ry[k - 1];
          cum += sqrtf(dx * dx + dy * dy);
        }
        route->rcum[k] = cum;
      }
    }
    if (n < 2) continue;
    for (uint32_t st = 0; st + 1 < n; st += CHUNK_PTS - 1) {
      uint32_t cnt = min<uint32_t>(CHUNK_PTS, n - st);
      Chunk &c = r.chunks[ci++];
      c.start = base + st;
      c.count = cnt;
      c.kind = kind;
      c.minX = c.maxX = r.px[c.start];
      c.minY = c.maxY = r.py[c.start];
      for (uint32_t q = 1; q < cnt; q++) {
        int32_t qx = r.px[c.start + q], qy = r.py[c.start + q];
        if (qx < c.minX) c.minX = qx;
        if (qx > c.maxX) c.maxX = qx;
        if (qy < c.minY) c.minY = qy;
        if (qy > c.maxY) c.maxY = qy;
      }
    }
  }
  r.nPts = pi;
  r.nChunks = ci;
  std::stable_sort(r.chunks, r.chunks + r.nChunks, [](const Chunk &a, const Chunk &b) {
    return a.kind < b.kind;
  });
  uint32_t idx = 0;
  for (uint8_t k = 0; k < 6; k++) {
    while (idx < r.nChunks && r.chunks[idx].kind < k) idx++;
    r.kindStart[k] = idx;
  }
  r.kindStart[5] = r.nChunks;
  out = r;
  return true;
}

void clearWorldLocked() {
  freeRoute(world.route);
  for (int i = 0; i < MAX_TILES; i++) {
    if (world.tiles[i].used) freeLines(world.tiles[i].lines);
    world.tiles[i] = {};
  }
  world.hasSession = false;
}

struct Camera {
  bool init;
  float x, y, along, heading, mpp;
  uint32_t lastMs;
} cam = {};

void applyBlob(const uint8_t *b, uint32_t len) {
  if (len < 8) return;
  bool isRoute = memcmp(b, "MNR2", 4) == 0;
  bool isTile = memcmp(b, "MNT2", 4) == 0;
  if (!isRoute && !isTile) return;
  uint16_t session = rd16(b + 4);
  if (isRoute) {
    RouteData fresh = {};
    if (!parseLines(b, len, 6, fresh.lines, &fresh)) return;
    xSemaphoreTake(mapMutex, portMAX_DELAY);
    if (!world.hasSession || world.session != session) {
      clearWorldLocked();
      world.hasSession = true;
      world.session = session;
      cam.init = false;
    }
    RouteData old = world.route;
    world.route = fresh;
    xSemaphoreGive(mapMutex);
    freeRoute(old);
    return;
  }
  if (len < 12) return;
  int16_t tx = (int16_t)rd16(b + 6);
  int16_t ty = (int16_t)rd16(b + 8);
  Lines fresh = {};
  if (!parseLines(b, len, 10, fresh, nullptr)) return;
  xSemaphoreTake(mapMutex, portMAX_DELAY);
  if (!world.hasSession || world.session != session) {
    clearWorldLocked();
    world.hasSession = true;
    world.session = session;
    cam.init = false;
  }
  int slot = -1;
  for (int i = 0; i < MAX_TILES; i++) {
    if (world.tiles[i].used && world.tiles[i].tx == tx && world.tiles[i].ty == ty) slot = i;
  }
  if (slot < 0) {
    for (int i = 0; i < MAX_TILES && slot < 0; i++) {
      if (!world.tiles[i].used) slot = i;
    }
  }
  if (slot < 0) {
    float best = -1;
    for (int i = 0; i < MAX_TILES; i++) {
      const Lines &l = world.tiles[i].lines;
      float cx = (l.minX + l.maxX) * 0.5f - cam.x, cy = (l.minY + l.maxY) * 0.5f - cam.y;
      float d = cx * cx + cy * cy;
      if (d > best) {
        best = d;
        slot = i;
      }
    }
  }
  Lines old = world.tiles[slot].used ? world.tiles[slot].lines : Lines{};
  world.tiles[slot].used = true;
  world.tiles[slot].tx = tx;
  world.tiles[slot].ty = ty;
  world.tiles[slot].lines = fresh;
  xSemaphoreGive(mapMutex);
  freeLines(old);
}

void handleNavPacket(const uint8_t *p, size_t n) {
  if (n < 18) return;
  NavData d = {};
  d.active = p[1] == 1;
  d.maneuver = p[2];
  d.exitNumber = p[3];
  d.angle = (int16_t)rd16(p + 4);
  d.distM = rd32(p + 6);
  d.remainM = rd32(p + 10);
  d.remainMin = rd16(p + 14);
  d.speedKmh = p[16];
  d.flags = p[17];
  size_t tl = n - 18;
  if (tl > sizeof(d.text) - 1) tl = sizeof(d.text) - 1;
  memcpy(d.text, p + 18, tl);
  d.text[tl] = 0;
  portENTER_CRITICAL(&navMux);
  navShared = d;
  portEXIT_CRITICAL(&navMux);
  lastNavMs = millis();
  navDirty = true;
}

void handlePosPacket(const uint8_t *p, size_t n) {
  if (n < 19) return;
  PosTarget t;
  t.valid = true;
  t.mapId = rd16(p + 1);
  t.x = (int32_t)rd32(p + 3) / 10.0f;
  t.y = (int32_t)rd32(p + 7) / 10.0f;
  uint16_t h = rd16(p + 11);
  t.headingDeg = h == 0xFFFF ? -1 : h / 100.0f;
  t.speed = rd16(p + 13) / 100.0f;
  int32_t a = (int32_t)rd32(p + 15);
  t.along = a < 0 ? -1 : a / 10.0f;
  t.ms = millis();
  portENTER_CRITICAL(&posMux);
  posShared = t;
  portEXIT_CRITICAL(&posMux);
}

void handleAlertPacket(const uint8_t *p, size_t n) {
  if (n < 6) return;
  AlertData a;
  a.type = p[1];
  a.angle = (int16_t)rd16(p + 2);
  a.dist = rd16(p + 4);
  a.ms = millis();
  portENTER_CRITICAL(&alertMux);
  alertShared = a;
  portEXIT_CRITICAL(&alertMux);
}

void handleStatusPacket(const uint8_t *p, size_t n) {
  if (n < 5) return;
  StatusData st = {};
  st.limit = p[1];
  st.radarDist = rd16(p + 2);
  size_t tl = n - 5;
  if (tl > sizeof(st.street) - 1) tl = sizeof(st.street) - 1;
  memcpy(st.street, p + 5, tl);
  st.street[tl] = 0;
  st.ms = millis();
  portENTER_CRITICAL(&statusMux);
  statusShared = st;
  portEXIT_CRITICAL(&statusMux);
}

void handleSettingsPacket(const uint8_t *p, size_t n) {
  if (n < 4) return;
  setBrightnessVal = max<uint8_t>(20, p[1]);
  setNight = p[2];
  setNotify = p[3];
  settingsMs = millis() ? millis() : 1;
  navDirty = true;
}

void setInfo(const char *s, bool notify) {
  if (!infoChar) return;
  infoChar->setValue((uint8_t *)s, strlen(s));
  if (notify && bleConnected) infoChar->notify();
}

uint8_t *otaBuf = nullptr;
volatile bool otaWriting = false;

void otaFail(const char *what, int code) {
  char msg[48];
  snprintf(msg, sizeof(msg), "ota=err:%s:%d", what, code);
  if (otaBuf) {
    heap_caps_free(otaBuf);
    otaBuf = nullptr;
  }
  otaActive = false;
  otaWriting = false;
  navDirty = true;
  setInfo(msg, true);
}

void handleOtaPacket(const uint8_t *p, size_t n) {
  char msg[40];
  if (p[0] == 0x20 && n >= 5) {
    otaSize = rd32(p + 1);
    otaGot = 0;
    if (otaBuf) heap_caps_free(otaBuf);
    otaBuf = (otaSize > 0 && otaSize < 3000000) ? (uint8_t *)psAlloc(otaSize) : nullptr;
    if (!otaBuf) {
      otaFail("mem", (int)otaSize);
      return;
    }
    otaActive = true;
    otaWriting = false;
    otaProgress = 0;
    navDirty = true;
    setInfo("ota=p:0", true);
  } else if (p[0] == 0x21 && n >= 5) {
    if (!otaActive || !otaBuf) return;
    uint32_t off = rd32(p + 1);
    size_t dl = n - 5;
    if (off != otaGot || off + dl > otaSize) {
      otaFail("offset", (int)off);
      return;
    }
    memcpy(otaBuf + off, p + 5, dl);
    otaGot += dl;
    int pr = otaSize ? (int)((uint64_t)otaGot * 100 / otaSize) : 0;
    if (pr != otaProgress) {
      otaProgress = pr;
      navDirty = true;
      if (pr % 10 == 0) {
        snprintf(msg, sizeof(msg), "ota=p:%d", pr);
        setInfo(msg, true);
      }
    }
  } else if (p[0] == 0x22) {
    if (!otaActive || !otaBuf) return;
    if (otaGot != otaSize) {
      otaFail("size", (int)otaGot);
      return;
    }
    if (otaBuf[0] != 0xE9) {
      otaFail("image", otaBuf[0]);
      return;
    }
    otaWriting = true;
    navDirty = true;
    vTaskDelay(pdMS_TO_TICKS(300));
    if (Update.isRunning()) Update.abort();
    if (!Update.begin(otaSize, U_FLASH)) {
      otaFail("begin", Update.getError());
      return;
    }
    for (uint32_t off = 0; off < otaSize; off += 4096) {
      size_t len = min<uint32_t>(4096, otaSize - off);
      if (Update.write(otaBuf + off, len) != len) {
        Update.abort();
        otaFail("write", Update.getError());
        return;
      }
      vTaskDelay(1);
    }
    if (!Update.end(true)) {
      otaFail("end", Update.getError());
      return;
    }
    heap_caps_free(otaBuf);
    otaBuf = nullptr;
    setInfo("ota=ok", true);
    vTaskDelay(pdMS_TO_TICKS(1500));
    ESP.restart();
  }
}

void handleMapPacket(const uint8_t *p, size_t n) {
  if (p[0] == 0x10 && n >= 7) {
    uint16_t id = rd16(p + 1);
    uint32_t total = rd32(p + 3);
    if (rxBuf) heap_caps_free(rxBuf);
    rxBuf = (uint8_t *)psAlloc(total);
    rxTotal = rxBuf ? total : 0;
    rxGot = 0;
    rxId = id;
    mapProgress = rxBuf ? 0 : -1;
  } else if (p[0] == 0x11 && n >= 7) {
    uint16_t id = rd16(p + 1);
    uint32_t off = rd32(p + 3);
    uint32_t dl = n - 7;
    if (!rxBuf || id != rxId || off + dl > rxTotal) return;
    memcpy(rxBuf + off, p + 7, dl);
    rxGot += dl;
    mapProgress = rxTotal ? (int)((uint64_t)rxGot * 100 / rxTotal) : 0;
  } else if (p[0] == 0x12 && n >= 3) {
    uint16_t id = rd16(p + 1);
    if (!rxBuf || id != rxId || rxGot != rxTotal) {
      mapProgress = -1;
      return;
    }
    applyBlob(rxBuf, rxTotal);
    heap_caps_free(rxBuf);
    rxBuf = nullptr;
    mapProgress = 100;
  }
}

struct IosHandles {
  uint16_t nsVal, nsCccd;
  uint16_t cpVal;
  uint16_t dsVal, dsCccd;
  uint16_t rcVal;
  uint16_t euVal, euCccd;
};
IosHandles ios = {};
uint8_t dsBuf[600];
uint16_t dsLen = 0;
uint32_t pendingUid = 0;
uint8_t pendingCat = 0;

bool uuidFromStr(const char *str, ble_uuid128_t &u) {
  uint8_t bytes[16];
  int bi = 0;
  for (const char *c = str; *c && bi < 32; c++) {
    if (*c == '-') continue;
    char h[3] = { c[0], c[1], 0 };
    if (!c[1]) return false;
    bytes[bi / 2] = (uint8_t)strtol(h, nullptr, 16);
    bi += 2;
    c++;
  }
  if (bi != 32) return false;
  u.u.type = BLE_UUID_TYPE_128;
  for (int i = 0; i < 16; i++) u.value[i] = bytes[15 - i];
  return true;
}

int svcCb(uint16_t conn, const ble_gatt_error *err, const ble_gatt_svc *svc, void *arg) {
  GattCtx *c = (GattCtx *)arg;
  if (err->status == 0 && svc) {
    c->start = svc->start_handle;
    c->end = svc->end_handle;
  } else {
    if (err->status != BLE_HS_EDONE) c->status = err->status;
    xSemaphoreGive(gattSem);
  }
  return 0;
}

int chrCb(uint16_t conn, const ble_gatt_error *err, const ble_gatt_chr *chr, void *arg) {
  GattCtx *c = (GattCtx *)arg;
  if (err->status == 0 && chr) {
    for (int i = 0; i < c->nWant; i++) {
      if (ble_uuid_cmp(&chr->uuid.u, &c->want[i].u) == 0) c->found[i] = chr->val_handle;
    }
  } else {
    if (err->status != BLE_HS_EDONE) c->status = err->status;
    xSemaphoreGive(gattSem);
  }
  return 0;
}

int dscCb(uint16_t conn, const ble_gatt_error *err, uint16_t chrVal, const ble_gatt_dsc *dsc, void *arg) {
  GattCtx *c = (GattCtx *)arg;
  if (err->status == 0 && dsc) {
    if (c->cccd == 0 && ble_uuid_u16(&dsc->uuid.u) == 0x2902) c->cccd = dsc->handle;
  } else {
    if (err->status != BLE_HS_EDONE) c->status = err->status;
    xSemaphoreGive(gattSem);
  }
  return 0;
}

int writeCb(uint16_t conn, const ble_gatt_error *err, ble_gatt_attr *attr, void *arg) {
  GattCtx *c = (GattCtx *)arg;
  c->status = err->status;
  xSemaphoreGive(gattSem);
  return 0;
}

bool gattWait() {
  return xSemaphoreTake(gattSem, pdMS_TO_TICKS(8000)) == pdTRUE;
}

bool findService(uint16_t conn, const char *uuid, GattCtx &c) {
  ble_uuid128_t u;
  if (!uuidFromStr(uuid, u)) return false;
  c = {};
  if (ble_gattc_disc_svc_by_uuid(conn, &u.u, svcCb, &c) != 0) return false;
  return gattWait() && c.status == 0 && c.start != 0;
}

bool findChars(uint16_t conn, GattCtx &c, const char *const *uuids, int n) {
  c.nWant = n;
  for (int i = 0; i < n; i++) {
    uuidFromStr(uuids[i], c.want[i]);
    c.found[i] = 0;
  }
  c.status = 0;
  if (ble_gattc_disc_all_chrs(conn, c.start, c.end, chrCb, &c) != 0) return false;
  return gattWait() && c.status == 0;
}

uint16_t findCccd(uint16_t conn, uint16_t valHandle, uint16_t end) {
  GattCtx c = {};
  if (valHandle == 0 || valHandle >= end) return 0;
  if (ble_gattc_disc_all_dscs(conn, valHandle, end, dscCb, &c) != 0) return 0;
  if (!gattWait()) return 0;
  return c.cccd;
}

bool gattWrite(uint16_t conn, uint16_t handle, const uint8_t *data, uint16_t len) {
  GattCtx c = {};
  if (ble_gattc_write_flat(conn, handle, data, len, writeCb, &c) != 0) return false;
  return gattWait() && c.status == 0;
}

void setupAncs(uint16_t conn) {
  static const char *const chars[3] = {
    "9FBF120D-6301-42D9-8C58-25E699A21DBD",
    "69D1D8F3-45E1-49A8-9821-9BBDFDAAD9D9",
    "22EAC6E9-24D6-4BB5-BE44-B36ACE7C7BFB"
  };
  GattCtx c;
  if (!findService(conn, "7905F431-B5CE-4E99-A40F-4B1E122D00D0", c)) return;
  if (!findChars(conn, c, chars, 3)) return;
  ios.nsVal = c.found[0];
  ios.cpVal = c.found[1];
  ios.dsVal = c.found[2];
  ios.dsCccd = findCccd(conn, ios.dsVal, c.end);
  ios.nsCccd = findCccd(conn, ios.nsVal, c.end);
  const uint8_t on[2] = { 1, 0 };
  if (ios.dsCccd) gattWrite(conn, ios.dsCccd, on, 2);
  if (ios.nsCccd) gattWrite(conn, ios.nsCccd, on, 2);
}

void setupAms(uint16_t conn) {
  static const char *const chars[2] = {
    "9B3C81D8-57B1-4A8A-B8DF-0E56F7CA51C2",
    "2F7CABCE-808D-411F-9A0C-BB92BA96C102"
  };
  GattCtx c;
  if (!findService(conn, "89D3502B-0F36-433A-8EF4-C502AD55F8DC", c)) return;
  if (!findChars(conn, c, chars, 2)) return;
  ios.rcVal = c.found[0];
  ios.euVal = c.found[1];
  ios.euCccd = findCccd(conn, ios.euVal, c.end);
  const uint8_t on[2] = { 1, 0 };
  if (!ios.euCccd || !gattWrite(conn, ios.euCccd, on, 2)) return;
  const uint8_t track[3] = { 2, 0, 2 };
  const uint8_t player[2] = { 0, 1 };
  gattWrite(conn, ios.euVal, track, 3);
  gattWrite(conn, ios.euVal, player, 2);
  portENTER_CRITICAL(&musicMux);
  musicShared.available = true;
  portEXIT_CRITICAL(&musicMux);
}

int gapListener(ble_gap_event *ev, void *arg) {
  switch (ev->type) {
    case BLE_GAP_EVENT_ENC_CHANGE:
      if (ev->enc_change.status == 0 && ev->enc_change.conn_handle == connHandle) {
        linkEncrypted = true;
        xSemaphoreGive(encSem);
      }
      break;
    case BLE_GAP_EVENT_NOTIFY_RX: {
      if (ev->notify_rx.conn_handle != connHandle || !ev->notify_rx.om) break;
      RxItem it;
      uint16_t len = OS_MBUF_PKTLEN(ev->notify_rx.om);
      if (len > sizeof(it.data) - 3) len = sizeof(it.data) - 3;
      it.data[0] = 0xA0;
      it.data[1] = ev->notify_rx.attr_handle & 0xFF;
      it.data[2] = ev->notify_rx.attr_handle >> 8;
      os_mbuf_copydata(ev->notify_rx.om, 0, len, it.data + 3);
      it.len = len + 3;
      xQueueSend(rxQueue, &it, 0);
      break;
    }
    case BLE_GAP_EVENT_DISCONNECT:
      linkEncrypted = false;
      break;
    default:
      break;
  }
  return 0;
}

ble_gap_event_listener gapListenerObj;

void iosTask(void *) {
  for (;;) {
    while (connHandle == 0xFFFF) vTaskDelay(pdMS_TO_TICKS(300));
    uint16_t conn = connHandle;
    vTaskDelay(pdMS_TO_TICKS(2500));
    if (connHandle != conn) continue;
    ios = {};
    dsLen = 0;
    xSemaphoreTake(encSem, 0);
    if (!linkEncrypted) {
      ble_gap_security_initiate(conn);
      xSemaphoreTake(encSem, pdMS_TO_TICKS(40000));
    }
    if (linkEncrypted && connHandle == conn) {
      setupAncs(conn);
      setupAms(conn);
    }
    while (connHandle == conn) vTaskDelay(pdMS_TO_TICKS(500));
    portENTER_CRITICAL(&musicMux);
    musicShared = {};
    portEXIT_CRITICAL(&musicMux);
    ios = {};
  }
}

const char *appName(const char *id) {
  static const char *const map[][2] = {
    { "net.whatsapp.WhatsApp", "WhatsApp" },
    { "net.whatsapp.WhatsAppSMB", "WhatsApp Business" },
    { "com.apple.MobileSMS", "Mensagens" },
    { "com.apple.mobilephone", "Telefone" },
    { "com.apple.facetime", "FaceTime" },
    { "ph.telegra.Telegraph", "Telegram" },
    { "com.burbn.instagram", "Instagram" },
    { "com.google.Gmail", "Gmail" },
    { "com.apple.mobilemail", "Mail" },
    { "com.microsoft.skype.teams", "Teams" },
    { "com.tinyspeck.chatlyio", "Slack" }
  };
  for (auto &m : map) {
    if (strcmp(id, m[0]) == 0) return m[1];
  }
  const char *dot = strrchr(id, '.');
  return dot ? dot + 1 : id;
}

void requestAttributes(uint32_t uid) {
  if (!ios.cpVal || connHandle == 0xFFFF) return;
  uint8_t cmd[] = { 0, (uint8_t)uid, (uint8_t)(uid >> 8), (uint8_t)(uid >> 16), (uint8_t)(uid >> 24), 0, 1, 48, 0, 3, 120, 0 };
  dsLen = 0;
  ble_gattc_write_flat(connHandle, ios.cpVal, cmd, sizeof(cmd), nullptr, nullptr);
}

void handleNotificationSource(const uint8_t *p, uint16_t n) {
  if (n < 8) return;
  uint8_t ev = p[0], flags = p[1], cat = p[2];
  uint32_t uid = rd32(p + 4);
  if (ev == 2) {
    portENTER_CRITICAL(&toastMux);
    if (toastShared.active && toastShared.uid == uid && toastShared.call) toastShared.active = false;
    portEXIT_CRITICAL(&toastMux);
    navDirty = true;
    return;
  }
  if (ev != 0 || (flags & 0x04) || !setNotify) return;
  if (cat != 1 && cat != 2 && cat != 4) return;
  pendingUid = uid;
  pendingCat = cat;
  requestAttributes(uid);
}

void handleDataSource(const uint8_t *p, uint16_t n) {
  if (dsLen + n > sizeof(dsBuf)) dsLen = 0;
  memcpy(dsBuf + dsLen, p, n);
  dsLen += n;
  if (dsLen < 5 || dsBuf[0] != 0) return;
  uint32_t uid = rd32(dsBuf + 1);
  uint16_t off = 5;
  char vals[3][128] = {};
  int got = 0;
  while (got < 3) {
    if (off + 3 > dsLen) return;
    uint8_t id = dsBuf[off];
    uint16_t len = rd16(dsBuf + off + 1);
    if (off + 3 + len > dsLen) return;
    int slot = id == 0 ? 0 : (id == 1 ? 1 : 2);
    uint16_t cl = min<uint16_t>(len, 127);
    memcpy(vals[slot], dsBuf + off + 3, cl);
    vals[slot][cl] = 0;
    off += 3 + len;
    got++;
  }
  dsLen = 0;
  if (uid != pendingUid) return;
  Toast t = {};
  t.active = true;
  t.call = pendingCat == 1;
  t.uid = uid;
  t.until = millis() + (t.call ? 30000 : 8000);
  strncpy(t.app, appName(vals[0]), sizeof(t.app) - 1);
  strncpy(t.title, vals[1], sizeof(t.title) - 1);
  strncpy(t.msg, vals[2], sizeof(t.msg) - 1);
  if (pendingCat == 2) strncpy(t.msg, "Ligação perdida", sizeof(t.msg) - 1);
  portENTER_CRITICAL(&toastMux);
  toastShared = t;
  portEXIT_CRITICAL(&toastMux);
  lastActivity = millis();
  navDirty = true;
}

void handleEntityUpdate(const uint8_t *p, uint16_t n) {
  if (n < 3) return;
  uint8_t entity = p[0], attr = p[1];
  char v[64];
  uint16_t l = min<uint16_t>(n - 3, sizeof(v) - 1);
  memcpy(v, p + 3, l);
  v[l] = 0;
  portENTER_CRITICAL(&musicMux);
  if (entity == 2 && attr == 0) strncpy(musicShared.artist, v, sizeof(musicShared.artist) - 1);
  if (entity == 2 && attr == 2) strncpy(musicShared.title, v, sizeof(musicShared.title) - 1);
  if (entity == 0 && attr == 1) musicShared.playing = v[0] == '1';
  portEXIT_CRITICAL(&musicMux);
  navDirty = true;
}

void handleIosNotify(const uint8_t *p, uint16_t n) {
  if (n < 3) return;
  uint16_t h = p[1] | (p[2] << 8);
  const uint8_t *d = p + 3;
  uint16_t dl = n - 3;
  if (h == ios.nsVal) handleNotificationSource(d, dl);
  else if (h == ios.dsVal) handleDataSource(d, dl);
  else if (h == ios.euVal) handleEntityUpdate(d, dl);
}

void musicCommand(uint8_t cmd) {
  if (!ios.rcVal || connHandle == 0xFFFF) return;
  ble_gattc_write_flat(connHandle, ios.rcVal, &cmd, 1, nullptr, nullptr);
}

#define ES7210_ADDR 0x40
#define MIC_MCLK 42
#define MIC_BCLK 9
#define MIC_LRCK 45
#define MIC_DOUT 8
#define MIC_DIN 10
#define VOICE_RATE 16000
#define VOICE_FRAME 320
#define VOICE_MAX_SAMPLES (VOICE_RATE * 8)
#define VOICE_BLOCK 200

I2SClass micI2S;
volatile bool voiceRequested = false;
volatile uint32_t voiceAnswerAt = 0;
bool micReady = false;
uint8_t voiceSession = 0;

bool es7210Write(uint8_t r, uint8_t v) {
  xSemaphoreTake(i2cMutex, portMAX_DELAY);
  Wire.beginTransmission(ES7210_ADDR);
  Wire.write(r);
  Wire.write(v);
  bool ok = Wire.endTransmission() == 0;
  xSemaphoreGive(i2cMutex);
  return ok;
}

int es7210Read(uint8_t r) {
  xSemaphoreTake(i2cMutex, portMAX_DELAY);
  Wire.beginTransmission(ES7210_ADDR);
  Wire.write(r);
  int v = -1;
  if (Wire.endTransmission(false) == 0 && Wire.requestFrom((uint8_t)ES7210_ADDR, (uint8_t)1) == 1) v = Wire.read();
  xSemaphoreGive(i2cMutex);
  return v;
}

bool micInit() {
  if (micReady) return true;
  micI2S.setPins(MIC_BCLK, MIC_LRCK, MIC_DOUT, MIC_DIN, MIC_MCLK);
  if (!micI2S.begin(I2S_MODE_STD, VOICE_RATE, I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO, I2S_STD_SLOT_BOTH)) return false;
  delay(30);
  bool ok = true;
  ok &= es7210Write(0x00, 0xFF);
  ok &= es7210Write(0x00, 0x32);
  ok &= es7210Write(0x01, 0x3F);
  ok &= es7210Write(0x09, 0x30);
  ok &= es7210Write(0x0A, 0x30);
  ok &= es7210Write(0x23, 0x2A);
  ok &= es7210Write(0x22, 0x0A);
  ok &= es7210Write(0x20, 0x0A);
  ok &= es7210Write(0x21, 0x2A);
  int m = es7210Read(0x08);
  ok &= es7210Write(0x08, m < 0 ? 0x10 : (m & ~0x01));
  ok &= es7210Write(0x40, 0xC3);
  ok &= es7210Write(0x41, 0x70);
  ok &= es7210Write(0x42, 0x70);
  ok &= es7210Write(0x11, 0x60);
  ok &= es7210Write(0x12, 0x00);
  ok &= es7210Write(0x02, 0xC1);
  ok &= es7210Write(0x07, 0x20);
  ok &= es7210Write(0x04, 0x01);
  ok &= es7210Write(0x05, 0x00);
  ok &= es7210Write(0x43, 0x1A);
  ok &= es7210Write(0x44, 0x1A);
  ok &= es7210Write(0x45, 0x1A);
  ok &= es7210Write(0x46, 0x1A);
  ok &= es7210Write(0x47, 0x08);
  ok &= es7210Write(0x48, 0x08);
  ok &= es7210Write(0x49, 0x08);
  ok &= es7210Write(0x4A, 0x08);
  ok &= es7210Write(0x06, 0x04);
  ok &= es7210Write(0x01, 0x00);
  ok &= es7210Write(0x4B, 0x00);
  ok &= es7210Write(0x4C, 0x00);
  ok &= es7210Write(0x00, 0x71);
  ok &= es7210Write(0x00, 0x41);
  Serial.printf("es7210 id=%02X%02X ok=%d\n", es7210Read(0xFD), es7210Read(0xFE), ok);
  micReady = ok;
  return ok;
}

const int8_t imaIdx[16] = {-1, -1, -1, -1, 2, 4, 6, 8, -1, -1, -1, -1, 2, 4, 6, 8};
const int16_t imaStep[89] = {7, 8, 9, 10, 11, 12, 13, 14, 16, 17, 19, 21, 23, 25, 28, 31, 34, 37, 41, 45, 50, 55, 60, 66, 73, 80, 88, 97, 107, 118, 130, 143, 157, 173, 190, 209, 230, 253, 279, 307, 337, 371, 408, 449, 494, 544, 598, 658, 724, 796, 876, 963, 1060, 1166, 1282, 1411, 1552, 1707, 1878, 2066, 2272, 2499, 2749, 3024, 3327, 3660, 4026, 4428, 4871, 5358, 5894, 6484, 7132, 7845, 8630, 9493, 10442, 11487, 12635, 13899, 15289, 16818, 18500, 20350, 22385, 24623, 27086, 29794, 32767};

uint8_t imaEncode(int16_t sample, int &pred, int &idx) {
  int step = imaStep[idx];
  int diff = sample - pred;
  uint8_t code = 0;
  if (diff < 0) {
    code = 8;
    diff = -diff;
  }
  int vp = step >> 3;
  if (diff >= step) {
    code |= 4;
    diff -= step;
    vp += step;
  }
  step >>= 1;
  if (diff >= step) {
    code |= 2;
    diff -= step;
    vp += step;
  }
  step >>= 1;
  if (diff >= step) {
    code |= 1;
    vp += step;
  }
  pred += (code & 8) ? -vp : vp;
  if (pred > 32767) pred = 32767;
  if (pred < -32768) pred = -32768;
  idx += imaIdx[code];
  if (idx < 0) idx = 0;
  if (idx > 88) idx = 88;
  return code;
}

void setVoiceToast(const char *title, const char *msg, uint32_t ms) {
  Toast t = {};
  t.active = true;
  strncpy(t.app, "Voz", sizeof(t.app) - 1);
  strncpy(t.title, title, sizeof(t.title) - 1);
  strncpy(t.msg, msg, sizeof(t.msg) - 1);
  t.until = millis() + ms;
  portENTER_CRITICAL(&toastMux);
  toastShared = t;
  portEXIT_CRITICAL(&toastMux);
  lastActivity = millis();
  screenOff = false;
  navDirty = true;
}

void audioNotify(const uint8_t *d, size_t n) {
  if (!infoChar || !bleConnected) return;
  infoChar->setValue((uint8_t *)d, n);
  infoChar->notify();
}

void voiceSend(const int16_t *pcm, size_t total) {
  uint16_t blocks = (total + VOICE_BLOCK - 1) / VOICE_BLOCK;
  voiceSession++;
  uint8_t head[8] = {0xB0, voiceSession, (uint8_t)(VOICE_RATE & 255), (uint8_t)(VOICE_RATE >> 8), (uint8_t)(blocks & 255), (uint8_t)(blocks >> 8)};
  audioNotify(head, 6);
  vTaskDelay(pdMS_TO_TICKS(30));
  int pred = 0, idx = 0;
  uint8_t pk[7 + VOICE_BLOCK / 2];
  for (uint16_t b = 0; b < blocks; b++) {
    pk[0] = 0xB1;
    pk[1] = voiceSession;
    pk[2] = b & 255;
    pk[3] = b >> 8;
    pk[4] = (uint8_t)(pred & 255);
    pk[5] = (uint8_t)((pred >> 8) & 255);
    pk[6] = idx;
    for (int i = 0; i < VOICE_BLOCK; i += 2) {
      size_t i0 = (size_t)b * VOICE_BLOCK + i;
      int16_t a = i0 < total ? pcm[i0] : 0;
      int16_t c = i0 + 1 < total ? pcm[i0 + 1] : 0;
      uint8_t lo = imaEncode(a, pred, idx);
      uint8_t hi = imaEncode(c, pred, idx);
      pk[7 + i / 2] = lo | (hi << 4);
    }
    audioNotify(pk, sizeof(pk));
    vTaskDelay(pdMS_TO_TICKS(9));
  }
  uint8_t tail[4] = {0xB2, voiceSession, (uint8_t)(blocks & 255), (uint8_t)(blocks >> 8)};
  vTaskDelay(pdMS_TO_TICKS(30));
  audioNotify(tail, 4);
  vTaskDelay(pdMS_TO_TICKS(30));
  char info[32];
  snprintf(info, sizeof(info), "fw=%d", FW_VERSION);
  infoChar->setValue((uint8_t *)info, strlen(info));
}

void voiceTask(void *) {
  int16_t *buf = (int16_t *)heap_caps_malloc(VOICE_MAX_SAMPLES * sizeof(int16_t), MALLOC_CAP_SPIRAM);
  int16_t *stereo = (int16_t *)heap_caps_malloc(VOICE_FRAME * 2 * sizeof(int16_t), MALLOC_CAP_INTERNAL);
  for (;;) {
    if (!voiceRequested) {
      if (voiceAnswerAt && millis() - voiceAnswerAt > 9000) {
        voiceAnswerAt = 0;
        setVoiceToast("Sem resposta", "O app não respondeu", 4000);
      }
      vTaskDelay(pdMS_TO_TICKS(40));
      continue;
    }
    voiceRequested = false;
    if (!buf || !stereo || otaActive) continue;
    if (!bleConnected) {
      setVoiceToast("Sem conexão", "iPhone desconectado", 3000);
      continue;
    }
    setVoiceToast("Preparando microfone", "", 4000);
    if (!micInit()) {
      setVoiceToast("Microfone indisponível", "Falha ao iniciar", 4000);
      continue;
    }
    micI2S.readBytes((char *)stereo, VOICE_FRAME * 4);
    setVoiceToast("Ouvindo...", "Fale agora", 12000);
    size_t total = 0;
    int frames = 0, quiet = 0;
    bool speech = false;
    float floorRms = 0, peak = 0;
    while (total + VOICE_FRAME <= VOICE_MAX_SAMPLES) {
      size_t got = micI2S.readBytes((char *)stereo, VOICE_FRAME * 4);
      if (got < VOICE_FRAME * 4) break;
      double e = 0;
      for (int i = 0; i < VOICE_FRAME; i++) {
        int v = stereo[2 * i] + stereo[2 * i + 1];
        if (v > 32767) v = 32767;
        if (v < -32768) v = -32768;
        buf[total + i] = v;
        e += (double)v * v;
      }
      float rms = sqrt(e / VOICE_FRAME);
      if (rms > peak) peak = rms;
      frames++;
      total += VOICE_FRAME;
      if (frames >= 10 && frames < 20) floorRms = max(floorRms, rms);
      if (frames >= 20) {
        float thr = max(700.0f, floorRms * 2.5f);
        if (rms > thr) {
          speech = true;
          quiet = 0;
        } else if (speech) {
          quiet++;
        }
        if (speech && quiet >= 45) break;
        if (!speech && total > (size_t)VOICE_RATE * 5) break;
      }
    }
    Serial.printf("voice: samples=%u speech=%d floor=%.0f peak=%.0f\n", (unsigned)total, speech, floorRms, peak);
    if (!speech) {
      char m[48];
      snprintf(m, sizeof(m), "Nível %d", (int)peak);
      setVoiceToast("Não ouvi nada", m, 3500);
      continue;
    }
    size_t trim = (size_t)quiet > 20 ? (quiet - 20) * VOICE_FRAME : 0;
    size_t sendN = total > trim ? total - trim : total;
    setVoiceToast("Enviando...", "", 10000);
    voiceSend(buf, sendN);
    setVoiceToast("Entendendo...", "", 12000);
    voiceAnswerAt = millis();
  }
}

void handleVoiceResult(const uint8_t *p, size_t n) {
  if (n < 2) return;
  char t[96];
  size_t l = min<size_t>(n - 2, sizeof(t) - 1);
  memcpy(t, p + 2, l);
  t[l] = 0;
  voiceAnswerAt = 0;
  setVoiceToast(p[1] == 1 ? "Ok" : "Não entendi", t, 5000);
}

void rxTask(void *) {
  RxItem it;
  for (;;) {
    if (xQueueReceive(rxQueue, &it, portMAX_DELAY) != pdTRUE) continue;
    if (it.len == 0) continue;
    switch (it.data[0]) {
      case 0x01: handleNavPacket(it.data, it.len); break;
      case 0x02: handlePosPacket(it.data, it.len); break;
      case 0x03: handleAlertPacket(it.data, it.len); break;
      case 0x04: handleStatusPacket(it.data, it.len); break;
      case 0x05: handleSettingsPacket(it.data, it.len); break;
      case 0x06: handleVoiceResult(it.data, it.len); break;
      case 0x20:
      case 0x21:
      case 0x22: handleOtaPacket(it.data, it.len); break;
      case 0xA0: handleIosNotify(it.data, it.len); break;
      case 0x10:
      case 0x11:
      case 0x12: handleMapPacket(it.data, it.len); break;
      default: break;
    }
  }
}

class ServerCallbacks : public BLEServerCallbacks {
  void onConnect(BLEServer *s, ble_gap_conn_desc *desc) override {
    bleConnected = true;
    connHandle = desc->conn_handle;
    linkEncrypted = desc->sec_state.encrypted;
    lastActivity = millis();
    navDirty = true;
  }
  void onDisconnect(BLEServer *s, ble_gap_conn_desc *desc) override {
    bleConnected = false;
    connHandle = 0xFFFF;
    linkEncrypted = false;
    navDirty = true;
    BLEDevice::startAdvertising();
  }
};

class RxCallbacks : public BLECharacteristicCallbacks {
  void onWrite(BLECharacteristic *c) override {
    RxItem it;
    size_t n = c->getLength();
    if (n == 0) return;
    if (n > sizeof(it.data)) n = sizeof(it.data);
    it.len = n;
    memcpy(it.data, c->getData(), n);
    xQueueSend(rxQueue, &it, pdMS_TO_TICKS(500));
  }
};

void bleInit() {
  BLEDevice::init(DEVICE_NAME);
  BLEDevice::setMTU(247);
  BLEServer *server = BLEDevice::createServer();
  server->setCallbacks(new ServerCallbacks());
  BLEService *svc = server->createService(SERVICE_UUID);
  RxCallbacks *cb = new RxCallbacks();
  BLECharacteristic *nav = svc->createCharacteristic(NAV_CHAR_UUID, BLECharacteristic::PROPERTY_WRITE | BLECharacteristic::PROPERTY_WRITE_NR);
  nav->setCallbacks(cb);
  BLECharacteristic *map = svc->createCharacteristic(MAP_CHAR_UUID, BLECharacteristic::PROPERTY_WRITE | BLECharacteristic::PROPERTY_WRITE_NR);
  map->setCallbacks(cb);
  BLECharacteristic *ota = svc->createCharacteristic(OTA_CHAR_UUID, BLECharacteristic::PROPERTY_WRITE | BLECharacteristic::PROPERTY_WRITE_NR);
  ota->setCallbacks(cb);
  infoChar = svc->createCharacteristic(INFO_CHAR_UUID, BLECharacteristic::PROPERTY_READ | BLECharacteristic::PROPERTY_NOTIFY);
  char info[32];
  snprintf(info, sizeof(info), "fw=%d", FW_VERSION);
  infoChar->setValue((uint8_t *)info, strlen(info));
  svc->start();
  BLEAdvertising *adv = BLEDevice::getAdvertising();
  adv->addServiceUUID(SERVICE_UUID);
  adv->setScanResponse(true);
  BLEDevice::startAdvertising();
  ble_hs_cfg.sm_bonding = 1;
  ble_hs_cfg.sm_mitm = 0;
  ble_hs_cfg.sm_sc = 1;
  ble_hs_cfg.sm_io_cap = BLE_HS_IO_NO_INPUT_OUTPUT;
  ble_hs_cfg.sm_our_key_dist = BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID;
  ble_hs_cfg.sm_their_key_dist = BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID;
  ble_gap_event_listener_register(&gapListenerObj, gapListener, nullptr);
}

int textWidth(const char *s) {
  gfx->setTextWrap(false);
  gfx->setCursor(0, -1000);
  gfx->print(s);
  return gfx->getCursorX();
}

void textAt(const char *s, int x, int baseline, const uint8_t *font, uint16_t color) {
  gfx->setFont(font);
  gfx->setTextColor(color);
  gfx->setCursor(x, baseline);
  gfx->print(s);
}

void textCentered(const char *s, int baseline, const uint8_t *font, uint16_t color) {
  gfx->setFont(font);
  gfx->setTextColor(color);
  int w = textWidth(s);
  gfx->setCursor(CX - w / 2, baseline);
  gfx->print(s);
}

void fitText(char *s, int maxW) {
  if (textWidth(s) <= maxW) return;
  size_t L = strlen(s);
  char test[170];
  while (L > 0) {
    L--;
    while (L > 0 && ((uint8_t)s[L] & 0xC0) == 0x80) L--;
    s[L] = 0;
    snprintf(test, sizeof(test), "%s...", s);
    if (textWidth(test) <= maxW) {
      strcpy(s, test);
      return;
    }
  }
}

void wrapCentered(const char *s, int baseline, int lineGap, int maxW, int maxLines, const uint8_t *font, uint16_t color) {
  gfx->setFont(font);
  gfx->setTextColor(color);
  char buf[128];
  strncpy(buf, s, sizeof(buf) - 1);
  buf[sizeof(buf) - 1] = 0;
  char *words[32];
  int nw = 0;
  for (char *tok = strtok(buf, " "); tok && nw < 32; tok = strtok(nullptr, " ")) words[nw++] = tok;
  char lines[3][160] = {};
  int li = 0;
  int wi = 0;
  while (wi < nw && li < maxLines) {
    strncpy(lines[li], words[wi++], sizeof(lines[li]) - 1);
    while (wi < nw) {
      char test[160];
      snprintf(test, sizeof(test), "%s %s", lines[li], words[wi]);
      if (textWidth(test) > maxW) break;
      strcpy(lines[li], test);
      wi++;
    }
    li++;
  }
  if (li > 0 && (wi < nw || textWidth(lines[li - 1]) > maxW)) {
    char *last = lines[li - 1];
    if (wi < nw) strncat(last, " ...", sizeof(lines[0]) - strlen(last) - 1);
    fitText(last, maxW);
  }
  int startY = baseline - ((li - 1) * lineGap) / 2;
  for (int i = 0; i < li; i++) {
    int w = textWidth(lines[i]);
    gfx->setCursor(CX - w / 2, startY + i * lineGap);
    gfx->print(lines[i]);
  }
}

void thickLine(float x0, float y0, float x1, float y1, float r, uint16_t c) {
  float dx = x1 - x0, dy = y1 - y0;
  float len = sqrtf(dx * dx + dy * dy);
  int n = max(1, (int)(len / max(1.0f, r * 0.3f)));
  for (int i = 0; i <= n; i++) {
    float t = (float)i / n;
    gfx->fillCircle(x0 + dx * t, y0 + dy * t, max(1, (int)r), c);
  }
}

void arrowHead(float x, float y, float ang, float size, uint16_t c) {
  float dx = sinf(ang), dy = -cosf(ang);
  float px = cosf(ang), py = sinf(ang);
  float tx = x + dx * size, ty = y + dy * size;
  float bx = x - dx * size * 0.15f, by = y - dy * size * 0.15f;
  gfx->fillTriangle(tx, ty, bx + px * size * 0.8f, by + py * size * 0.8f, bx - px * size * 0.8f, by - py * size * 0.8f, c);
}

void drawTurn(float cx, float cy, float deg, float s, uint16_t c) {
  float r = 15 * s;
  float a = deg * DEG_TO_RAD;
  if (fabsf(deg) < 10) {
    thickLine(cx, cy + 80 * s, cx, cy - 30 * s, r, c);
    arrowHead(cx, cy - 30 * s, 0, 60 * s, c);
    return;
  }
  float sx = cx - sinf(a) * 40 * s;
  float kneeY = cy + 15 * s;
  thickLine(sx, cy + 80 * s, sx, kneeY, r, c);
  float ex = sx + sinf(a) * 75 * s, ey = kneeY - cosf(a) * 75 * s;
  thickLine(sx, kneeY, ex, ey, r, c);
  arrowHead(ex, ey, a, 55 * s, c);
}

void drawUTurn(float cx, float cy, float s, uint16_t c) {
  float r = 15 * s;
  float rad = 40 * s;
  float topY = cy - 20 * s;
  thickLine(cx + rad, cy + 80 * s, cx + rad, topY, r, c);
  float px = cx + rad, py = topY;
  for (int i = 1; i <= 24; i++) {
    float t = PI * i / 24.0f;
    float nx = cx + rad * cosf(t), ny = topY - rad * sinf(t);
    thickLine(px, py, nx, ny, r, c);
    px = nx;
    py = ny;
  }
  thickLine(cx - rad, topY, cx - rad, cy + 30 * s, r, c);
  arrowHead(cx - rad, cy + 30 * s, PI, 55 * s, c);
}

void drawRoundabout(float cx, float cy, float deg, uint8_t exitNo, float s, uint16_t c) {
  float ringR = 42 * s;
  int ringT = max(2, (int)(13 * s));
  float rcY = cy + 15 * s;
  thickLine(cx, cy + 95 * s, cx, rcY + ringR, 15 * s, c);
  for (int t = -ringT / 2; t <= ringT / 2 + 1; t++) gfx->drawCircle(cx, rcY, ringR + t, c);
  float a = constrain(deg, -170.0f, 170.0f) * DEG_TO_RAD;
  float sx = cx + sinf(a) * ringR, sy = rcY - cosf(a) * ringR;
  float ex = cx + sinf(a) * (ringR + 30 * s), ey = rcY - cosf(a) * (ringR + 30 * s);
  thickLine(sx, sy, ex, ey, 15 * s, c);
  arrowHead(ex, ey, a, 38 * s, c);
  if (exitNo > 0 && s >= 0.6f) {
    char b[4];
    snprintf(b, sizeof(b), "%u", exitNo);
    gfx->setFont(u8g2_font_helvB24_tf);
    int w = textWidth(b);
    textAt(b, cx - w / 2, rcY + 12, u8g2_font_helvB24_tf, C_ACCENT);
  }
}

void drawFlag(float cx, float cy, float s, uint16_t c, uint16_t bg) {
  float px = cx - 55 * s;
  thickLine(px, cy + 80 * s, px, cy - 85 * s, 8 * s, c);
  float fx = px + 6 * s, fy = cy - 85 * s;
  int cw = max(2, (int)(30 * s)), chh = max(2, (int)(26 * s));
  for (int r = 0; r < 3; r++) {
    for (int k = 0; k < 4; k++) {
      uint16_t col = ((r + k) % 2 == 0) ? c : bg;
      gfx->fillRect(fx + k * cw, fy + r * chh, cw, chh, col);
    }
  }
  gfx->drawRect(fx, fy, cw * 4, chh * 3, c);
}

void drawManeuver(const NavData &n, float cx, float cy, float s, uint16_t bg) {
  switch (n.maneuver) {
    case M_UTURN: drawUTurn(cx, cy, s, C_FG); break;
    case M_ROUNDABOUT: drawRoundabout(cx, cy, n.angle, n.exitNumber, s, C_FG); break;
    case M_ARRIVE: drawFlag(cx, cy, s, C_FG, bg); break;
    case M_STRAIGHT:
    case M_DEPART:
    case M_NONE: drawTurn(cx, cy, 0, s, C_FG); break;
    case M_KEEP_RIGHT: drawTurn(cx, cy, 30, s, C_FG); break;
    case M_KEEP_LEFT: drawTurn(cx, cy, -30, s, C_FG); break;
    case M_SLIGHT_RIGHT: drawTurn(cx, cy, 45, s, C_FG); break;
    case M_SLIGHT_LEFT: drawTurn(cx, cy, -45, s, C_FG); break;
    case M_RIGHT: drawTurn(cx, cy, 90, s, C_FG); break;
    case M_LEFT: drawTurn(cx, cy, -90, s, C_FG); break;
    case M_SHARP_RIGHT: drawTurn(cx, cy, 135, s, C_FG); break;
    case M_SHARP_LEFT: drawTurn(cx, cy, -135, s, C_FG); break;
    default: drawTurn(cx, cy, constrain((float)n.angle, -150.0f, 150.0f), s, C_FG); break;
  }
}

void formatDistance(uint32_t m, char *out, size_t sz) {
  if (m < 1000) {
    uint32_t r = m < 100 ? (m / 5) * 5 : (m / 10) * 10;
    snprintf(out, sz, "%lu m", (unsigned long)r);
  } else if (m < 10000) {
    uint32_t d = (m + 50) / 100;
    snprintf(out, sz, "%lu,%lu km", (unsigned long)(d / 10), (unsigned long)(d % 10));
  } else {
    snprintf(out, sz, "%lu km", (unsigned long)((m + 500) / 1000));
  }
}

void formatMinutes(uint16_t min, char *out, size_t sz) {
  if (min < 60) snprintf(out, sz, "%u min", min);
  else snprintf(out, sz, "%uh%02u", min / 60, min % 60);
}

void fillRing(int cx, int cy, int rIn, int rOut, uint16_t c) {
  for (int dy = -rOut; dy <= rOut; dy++) {
    int xo = (int)sqrtf((float)(rOut * rOut - dy * dy));
    int xi = abs(dy) < rIn ? (int)sqrtf((float)(rIn * rIn - dy * dy)) : 0;
    if (xo <= xi) continue;
    gfx->drawFastHLine(cx - xo, cy + dy, xo - xi, c);
    gfx->drawFastHLine(cx + xi, cy + dy, xo - xi, c);
  }
}

bool isTurn(uint8_t m) {
  return m != M_NONE && m != M_STRAIGHT && m != M_DEPART;
}

bool blinkOn() {
  uint32_t now = millis();
  if (now - approachStart > 3000) return true;
  return ((now - approachStart) / 300) % 2 == 0;
}

void drawAlertRing() {
  if (!approaching || !blinkOn()) return;
  fillRing(CX, LCD_H / 2, 225, 233, C_ORANGE);
}

void drawCurveBadge(int x, int y, int16_t angle, uint16_t dist) {
  gfx->fillCircle(x, y, 36, 0xFFE0);
  float dir = angle >= 0 ? 1.0f : -1.0f;
  bool sharp = abs(angle) >= 90;
  float px = x, py = y + 20;
  float ex = x + dir * (sharp ? 16 : 14), ey = sharp ? y - 2 : y - 16;
  float cx = x, cy = y - 16;
  for (int i = 1; i <= 12; i++) {
    float t = i / 12.0f;
    float bx = (1 - t) * (1 - t) * x + 2 * (1 - t) * t * cx + t * t * ex;
    float by = (1 - t) * (1 - t) * (y + 20) + 2 * (1 - t) * t * cy + t * t * ey;
    thickLine(px, py, bx, by, 4, RGB565_BLACK);
    px = bx;
    py = by;
  }
  float ang = atan2f(ex - cx, -(ey - cy));
  arrowHead(ex, ey, ang, 12, RGB565_BLACK);
  char b[16];
  snprintf(b, sizeof(b), "%u m", (unsigned)((dist / 10) * 10));
  gfx->setFont(u8g2_font_helvB18_tf);
  int w = textWidth(b);
  textAt(b, x - w / 2, y + 60, u8g2_font_helvB18_tf, 0xFFE0);
}

bool curveActive(AlertData &a) {
  portENTER_CRITICAL(&alertMux);
  a = alertShared;
  portEXIT_CRITICAL(&alertMux);
  return a.type == 1 && millis() - a.ms < 5000 && a.dist <= 300;
}

bool statusFresh(StatusData &st) {
  portENTER_CRITICAL(&statusMux);
  st = statusShared;
  portEXIT_CRITICAL(&statusMux);
  return st.ms != 0 && millis() - st.ms < 10000;
}

bool overSpeed(float kmh) {
  StatusData st;
  return statusFresh(st) && st.limit > 0 && kmh > st.limit + 3;
}

void drawLimitSign(int x, int y, uint8_t limit, bool over) {
  bool flash = over && (millis() / 400) % 2 == 0;
  gfx->fillCircle(x, y, 31, flash ? C_YELLOW : C_RED);
  gfx->fillCircle(x, y, 24, RGB565_WHITE);
  char b[6];
  snprintf(b, sizeof(b), "%u", limit);
  const uint8_t *f = limit >= 100 ? u8g2_font_helvB18_tf : u8g2_font_helvB24_tf;
  gfx->setFont(f);
  int w = textWidth(b);
  textAt(b, x - w / 2, y + (limit >= 100 ? 7 : 9), f, RGB565_BLACK);
}

void drawRadarBadge(int x, int y, uint16_t dist) {
  bool close = dist <= 200;
  gfx->fillCircle(x, y, 40, C_RED);
  gfx->fillCircle(x, y, 33, RGB565_WHITE);
  gfx->fillRoundRect(x - 18, y - 13, 36, 24, 5, RGB565_BLACK);
  gfx->fillRect(x - 7, y - 18, 14, 6, RGB565_BLACK);
  gfx->fillCircle(x, y - 1, 9, RGB565_WHITE);
  gfx->fillCircle(x, y - 1, 6, RGB565_BLACK);
  gfx->fillCircle(x + 12, y - 8, 2, RGB565_WHITE);
  char b[16];
  snprintf(b, sizeof(b), "%u m", (unsigned)((dist / 10) * 10));
  gfx->setFont(u8g2_font_helvB18_tf);
  int w = textWidth(b);
  gfx->fillRoundRect(x - w / 2 - 10, y + 44, w + 20, 28, 14, close ? C_RED : 0x2104);
  textAt(b, x - w / 2, y + 65, u8g2_font_helvB18_tf, RGB565_WHITE);
}

uint32_t radarFlashStart = 0;
uint16_t lastRadarDist = 0xFFFF;

void updateRadarFlash(uint32_t now) {
  StatusData st;
  uint16_t d = statusFresh(st) ? st.radarDist : 0xFFFF;
  if (d != 0xFFFF && (lastRadarDist == 0xFFFF || d > lastRadarDist + 150)) {
    radarFlashStart = now ? now : 1;
    lastActivity = now;
    navDirty = true;
  }
  lastRadarDist = d;
}

bool radarFlashing(uint32_t now) {
  return radarFlashStart != 0 && now - radarFlashStart < 2200;
}

void drawRadarFlash(uint32_t now) {
  StatusData st;
  bool close = statusFresh(st) && st.radarDist != 0xFFFF;
  if (radarFlashing(now)) {
    uint16_t c = ((now - radarFlashStart) / 250) % 2 == 0 ? C_RED : RGB565_WHITE;
    fillRing(CX, LCD_H / 2, 208, 233, c);
  } else if (close) {
    fillRing(CX, LCD_H / 2, 224, 233, C_RED);
  }
}

void drawBatteryIcon() {
  int bp = batteryPct;
  if (bp < 0) return;
  bool chg = charging;
  uint16_t col = chg ? C_GREEN : (bp < 15 ? C_RED : (bp < 30 ? C_ORANGE : C_FG));
  char b[8];
  snprintf(b, sizeof(b), "%d%%", bp);
  gfx->setFont(u8g2_font_helvB14_tf);
  int tw = textWidth(b);
  int total = 26 + 3 + 5 + tw;
  int x = CX - total / 2, y = 10;
  gfx->drawRect(x, y, 26, 13, col);
  gfx->fillRect(x + 26, y + 4, 3, 5, col);
  int fill = max(1, min(22, bp * 22 / 100));
  gfx->fillRect(x + 2, y + 2, fill, 9, col);
  textAt(b, x + 34, y + 12, u8g2_font_helvB14_tf, col);
}

void drawStatusBadges(int limitX, int limitY, int radarX, int radarY, float kmh) {
  StatusData st;
  if (!statusFresh(st)) return;
  if (st.limit > 0) drawLimitSign(limitX, limitY, st.limit, kmh > st.limit + 3);
  if (st.radarDist != 0xFFFF) drawRadarBadge(radarX, radarY, st.radarDist);
}

void drawToast() {
  Toast t;
  portENTER_CRITICAL(&toastMux);
  if (toastShared.active && (int32_t)(millis() - toastShared.until) > 0) toastShared.active = false;
  t = toastShared;
  portEXIT_CRITICAL(&toastMux);
  if (!t.active) return;
  gfx->fillRoundRect(38, 250, LCD_W - 76, 170, 26, 0x18E3);
  gfx->drawRoundRect(38, 250, LCD_W - 76, 170, 26, t.call ? C_GREEN : C_ACCENT);
  char b[140];
  if (t.call) {
    textCentered("Ligação", 290, u8g2_font_helvB18_tf, C_GREEN);
    strncpy(b, t.title, sizeof(b) - 1);
    b[sizeof(b) - 1] = 0;
    gfx->setFont(u8g2_font_helvB24_tf);
    fitText(b, 330);
    textCentered(b, 335, u8g2_font_helvB24_tf, C_FG);
    textCentered(t.app, 375, u8g2_font_helvB14_tf, C_DIM);
    return;
  }
  textCentered(t.app, 282, u8g2_font_helvB14_tf, C_ACCENT);
  strncpy(b, t.title, sizeof(b) - 1);
  b[sizeof(b) - 1] = 0;
  gfx->setFont(u8g2_font_helvB18_tf);
  fitText(b, 340);
  textCentered(b, 314, u8g2_font_helvB18_tf, C_FG);
  wrapCentered(t.msg, 358, 26, 340, 2, u8g2_font_helvB18_tf, C_DIM);
}

void drawPlayIcon(int x, int y, bool playing, uint16_t c) {
  if (playing) {
    gfx->fillRect(x - 13, y - 16, 9, 32, c);
    gfx->fillRect(x + 4, y - 16, 9, 32, c);
  } else {
    gfx->fillTriangle(x - 10, y - 17, x - 10, y + 17, x + 16, y, c);
  }
}

void drawSkipIcon(int x, int y, bool next, uint16_t c) {
  int d = next ? 1 : -1;
  gfx->fillTriangle(x - 12 * d, y - 13, x - 12 * d, y + 13, x + 4 * d, y, c);
  gfx->fillTriangle(x - 2 * d, y - 13, x - 2 * d, y + 13, x + 14 * d, y, c);
  gfx->fillRect(d > 0 ? x + 14 : x - 17, y - 13, 3, 26, c);
}

void drawMusicScreen() {
  gfx->fillScreen(C_BG);
  Music m;
  portENTER_CRITICAL(&musicMux);
  m = musicShared;
  portEXIT_CRITICAL(&musicMux);
  textCentered("Música", 70, u8g2_font_helvB18_tf, C_DIM);
  if (!m.available) {
    wrapCentered("Aceite o pareamento no iPhone para controlar a música", 210, 30, 320, 3, u8g2_font_helvB18_tf, C_DIM);
  } else if (m.title[0] == 0) {
    textCentered("Nada tocando", 200, u8g2_font_helvB24_tf, C_DIM);
  } else {
    wrapCentered(m.title, 175, 32, 340, 2, u8g2_font_helvB24_tf, C_FG);
    char a[64];
    strncpy(a, m.artist, sizeof(a) - 1);
    a[sizeof(a) - 1] = 0;
    gfx->setFont(u8g2_font_helvB18_tf);
    fitText(a, 320);
    textCentered(a, 245, u8g2_font_helvB18_tf, C_ACCENT);
  }
  uint16_t c = m.available ? C_FG : 0x4208;
  gfx->drawCircle(CX - 115, 345, 38, 0x4208);
  gfx->fillCircle(CX, 345, 44, m.available ? C_ACCENT : 0x4208);
  gfx->drawCircle(CX + 115, 345, 38, 0x4208);
  drawSkipIcon(CX - 115, 345, false, c);
  drawPlayIcon(CX, 345, m.playing, RGB565_BLACK);
  drawSkipIcon(CX + 115, 345, true, c);
}

void drawOtaScreen() {
  gfx->fillScreen(C_BG);
  textCentered(otaWriting ? "Gravando..." : "Recebendo atualização", 170, u8g2_font_helvB24_tf, C_FG);
  char b[16];
  snprintf(b, sizeof(b), "%d%%", (int)otaProgress);
  textCentered(b, 260, u8g2_font_logisoso42_tr, C_ACCENT);
  gfx->drawRoundRect(83, 300, 300, 22, 11, C_DIM);
  gfx->fillRoundRect(85, 302, max(12, (int)(296 * otaProgress / 100)), 18, 9, C_ACCENT);
  textCentered("Não desligue", 370, u8g2_font_helvB18_tf, C_DIM);
}

void drawNavScreen(const NavData &n) {
  gfx->fillScreen(C_BG);
  char b[48];
  snprintf(b, sizeof(b), "%u km/h", n.speedKmh);
  textCentered(b, 62, u8g2_font_helvB18_tf, overSpeed(n.speedKmh) ? C_RED : C_DIM);
  if (n.maneuver == M_ARRIVE && n.distM < 30) {
    textCentered("Chegou", 120, u8g2_font_logisoso42_tr, C_GREEN);
  } else {
    formatDistance(n.distM, b, sizeof(b));
    textCentered(b, 120, u8g2_font_logisoso42_tr, approaching ? C_ORANGE : C_FG);
  }
  drawManeuver(n, CX, 220, 1.0f, C_BG);
  AlertData a;
  if (curveActive(a)) drawCurveBadge(CX + 150, 220, a.angle, a.dist);
  drawStatusBadges(CX - 150, 150, CX - 145, 250, n.speedKmh);
  if (n.flags & 0x01) {
    textCentered("Recalculando...", 372, u8g2_font_helvB24_tf, C_ORANGE);
  } else {
    wrapCentered(n.text, 372, 32, 330, 2, u8g2_font_helvB24_tf, C_ACCENT);
  }
  int mp = mapProgress;
  if (mp >= 0 && mp < 100) {
    snprintf(b, sizeof(b), "Recebendo mapa %d%%", mp);
    textCentered(b, 432, u8g2_font_helvB18_tf, C_ORANGE);
  } else {
    char d[16], t[16];
    formatDistance(n.remainM, d, sizeof(d));
    formatMinutes(n.remainMin, t, sizeof(t));
    snprintf(b, sizeof(b), "%s · %s", d, t);
    textCentered(b, 432, u8g2_font_helvB18_tf, C_DIM);
  }
}

void drawIdleScreen(const GpsData &g, bool connected) {
  gfx->fillScreen(C_BG);
  char b[32];
  if (g.timeValid) {
    snprintf(b, sizeof(b), "%02u:%02u", g.hour, g.minute);
  } else {
    snprintf(b, sizeof(b), "--:--");
  }
  textCentered(b, 80, u8g2_font_helvB24_tf, C_FG);
  if (g.fix) snprintf(b, sizeof(b), "%d", (int)roundf(g.kmph));
  else snprintf(b, sizeof(b), "0");
  textCentered(b, 280, u8g2_font_logisoso92_tn, g.fix ? C_FG : C_DIM);
  textCentered("km/h", 325, u8g2_font_helvB18_tf, C_DIM);
  textCentered(connected ? "iPhone conectado" : "Aguardando iPhone", 380, u8g2_font_helvB18_tf, connected ? C_GREEN : C_DIM);
  if (g.fix) snprintf(b, sizeof(b), "GPS %d sat", g.sats);
  else snprintf(b, sizeof(b), "GPS buscando...");
  textCentered(b, 418, u8g2_font_helvB14_tf, C_DIM);
}

static inline void toScreen(const View &v, float wx, float wy, float &sx, float &sy) {
  float dx = wx - v.px, dy = wy - v.py;
  sx = CX + (dx * v.cosH - dy * v.sinH) / v.mpp;
  sy = MAP_CY - (dx * v.sinH + dy * v.cosH) / v.mpp;
}

bool clipSegment(float &x0, float &y0, float &x1, float &y1, float lo, float hi) {
  float t0 = 0, t1 = 1;
  float dx = x1 - x0, dy = y1 - y0;
  float p[4] = { -dx, dx, -dy, dy };
  float q[4] = { x0 - lo, hi - x0, y0 - lo, hi - y0 };
  for (int i = 0; i < 4; i++) {
    if (p[i] == 0) {
      if (q[i] < 0) return false;
    } else {
      float r = q[i] / p[i];
      if (p[i] < 0) {
        if (r > t1) return false;
        if (r > t0) t0 = r;
      } else {
        if (r < t0) return false;
        if (r < t1) t1 = r;
      }
    }
  }
  float nx0 = x0 + t0 * dx, ny0 = y0 + t0 * dy;
  float nx1 = x0 + t1 * dx, ny1 = y0 + t1 * dy;
  x0 = nx0;
  y0 = ny0;
  x1 = nx1;
  y1 = ny1;
  return true;
}

void wideSeg(float x0, float y0, float x1, float y1, float w, uint16_t c, bool caps) {
  if (!clipSegment(x0, y0, x1, y1, -40, LCD_W + 40)) return;
  float dx = x1 - x0, dy = y1 - y0;
  float L = sqrtf(dx * dx + dy * dy);
  if (L < 0.5f) return;
  if (w <= 1.5f) {
    gfx->drawLine(x0, y0, x1, y1, c);
    return;
  }
  float nx = -dy / L * w * 0.5f, ny = dx / L * w * 0.5f;
  gfx->fillTriangle(x0 + nx, y0 + ny, x1 + nx, y1 + ny, x1 - nx, y1 - ny, c);
  gfx->fillTriangle(x0 + nx, y0 + ny, x1 - nx, y1 - ny, x0 - nx, y0 - ny, c);
  if (caps) gfx->fillCircle(x1, y1, (int)(w * 0.5f), c);
}

void roadStyle(uint8_t kind, float mpp, float &w, uint16_t &c) {
  float z = constrain(1.2f / mpp, 0.5f, 1.4f);
  switch (kind) {
    case 1: w = 9 * z; c = nightMode ? 0x5AEB : 0x9CD3; break;
    case 2: w = 7 * z; c = nightMode ? 0x4A49 : 0x7BEF; break;
    case 3: w = 5 * z; c = nightMode ? 0x31A6 : 0x5AEB; break;
    default: w = 4 * z; c = nightMode ? 0x2104 : 0x4208; break;
  }
}

void drawPositionMarker() {
  int x = CX, y = MAP_CY;
  gfx->fillCircle(x, y, 22, C_ROUTE_EDGE);
  gfx->fillTriangle(x, y - 26, x - 17, y + 16, x + 17, y + 16, C_FG);
  gfx->fillTriangle(x - 17, y + 16, x, y + 6, x + 17, y + 16, C_ROUTE_EDGE);
}

void drawMapOverlay(const NavData &n, float speedKmh) {
  bool cruise = n.flags & 0x02;
  uint16_t panelBg = C_PANEL;
  if (approaching && !cruise) panelBg = blinkOn() ? 0xC300 : C_PANEL;
  gfx->fillRect(0, 0, LCD_W, 118, panelBg);
  gfx->drawFastHLine(0, 118, LCD_W, 0x2104);
  char b[128];
  if (cruise) {
    GpsData g;
    portENTER_CRITICAL(&gpsMux);
    g = gpsShared;
    portEXIT_CRITICAL(&gpsMux);
    if (g.timeValid) {
      snprintf(b, sizeof(b), "%02u:%02u", g.hour, g.minute);
      textCentered(b, 58, u8g2_font_helvB14_tf, C_DIM);
    }
    StatusData st;
    const char *street = statusFresh(st) && st.street[0] ? st.street : n.text;
    strncpy(b, street, sizeof(b) - 1);
    b[sizeof(b) - 1] = 0;
    gfx->setFont(u8g2_font_helvB24_tf);
    fitText(b, 300);
    textCentered(b, 98, u8g2_font_helvB24_tf, C_ACCENT);
  } else {
    NavData mini = n;
    drawManeuver(mini, CX - 92, 64, 0.32f, panelBg);
    if (n.maneuver == M_ARRIVE && n.distM < 30) {
      textAt("Chegou", CX - 52, 80, u8g2_font_logisoso32_tr, C_GREEN);
    } else {
      formatDistance(n.distM, b, sizeof(b));
      textAt(b, CX - 52, 80, u8g2_font_logisoso32_tr, C_FG);
    }
    if (n.flags & 0x01) {
      textCentered("Recalculando...", 108, u8g2_font_helvB14_tf, C_ORANGE);
    } else {
      strncpy(b, n.text, sizeof(b) - 1);
      b[sizeof(b) - 1] = 0;
      gfx->setFont(u8g2_font_helvB14_tf);
      fitText(b, 280);
      textCentered(b, 108, u8g2_font_helvB14_tf, C_ACCENT);
    }
  }
  bool over = overSpeed(speedKmh);
  gfx->fillRoundRect(CX - 62, 410, 124, 40, 20, over ? C_RED : C_PANEL);
  snprintf(b, sizeof(b), "%d km/h", (int)roundf(speedKmh));
  textCentered(b, 438, u8g2_font_helvB18_tf, C_FG);
  drawStatusBadges(CX + 125, 380, CX + 115, 255, speedKmh);
}

float routePointAt(const RouteData &m, float along, float &x, float &y) {
  if (m.n < 2) return 0;
  float total = m.rcum[m.n - 1];
  float d = constrain(along, 0.0f, total);
  uint32_t lo = 0, hi = m.n - 1;
  while (hi - lo > 1) {
    uint32_t mid = (lo + hi) / 2;
    if (m.rcum[mid] <= d) lo = mid;
    else hi = mid;
  }
  float seg = m.rcum[hi] - m.rcum[lo];
  float t = seg > 0 ? (d - m.rcum[lo]) / seg : 0;
  x = m.rx[lo] + (m.rx[hi] - m.rx[lo]) * t;
  y = m.ry[lo] + (m.ry[hi] - m.ry[lo]) * t;
  return total;
}

float routeBearingAt(const RouteData &m, float along) {
  float x0, y0, x1, y1;
  routePointAt(m, along - 8, x0, y0);
  routePointAt(m, along + 15, x1, y1);
  return atan2f(x1 - x0, y1 - y0) * RAD_TO_DEG;
}

float wrap180(float a) {
  while (a > 180) a -= 360;
  while (a < -180) a += 360;
  return a;
}

void updateCamera(const RouteData &m, const PosTarget &t, uint32_t now) {
  float dt = cam.init ? (now - cam.lastMs) / 1000.0f : 0;
  dt = constrain(dt, 0.0f, 0.2f);
  cam.lastMs = now;
  float age = constrain((now - t.ms) / 1000.0f, 0.0f, 3.0f);
  float targetHeading = cam.heading;
  bool onRoute = t.along >= 0 && m.n >= 2;
  if (onRoute) {
    float predicted = t.along + t.speed * age;
    if (!cam.init || fabsf(predicted - cam.along) > 80) {
      cam.along = predicted;
    } else {
      cam.along += t.speed * dt;
      cam.along += (predicted - cam.along) * min(1.0f, dt * 2.0f);
    }
    routePointAt(m, cam.along, cam.x, cam.y);
    targetHeading = routeBearingAt(m, cam.along);
  } else {
    float h = t.headingDeg >= 0 ? t.headingDeg * DEG_TO_RAD : cam.heading * DEG_TO_RAD;
    float ex = t.x + sinf(h) * t.speed * age;
    float ey = t.y + cosf(h) * t.speed * age;
    if (!cam.init || fabsf(ex - cam.x) + fabsf(ey - cam.y) > 120) {
      cam.x = ex;
      cam.y = ey;
    } else {
      float k = min(1.0f, dt * 3.0f);
      cam.x += (ex - cam.x) * k;
      cam.y += (ey - cam.y) * k;
    }
    if (t.headingDeg >= 0 && t.speed > 1.0f) targetHeading = t.headingDeg;
  }
  float targetMpp = constrain(0.7f + t.speed * 3.6f * 0.02f, 0.7f, 2.6f);
  if (!cam.init) {
    cam.heading = targetHeading;
    cam.mpp = targetMpp;
    cam.init = true;
  } else {
    cam.heading += wrap180(targetHeading - cam.heading) * min(1.0f, dt * 3.0f);
    cam.heading = wrap180(cam.heading);
    cam.mpp += (targetMpp - cam.mpp) * min(1.0f, dt * 0.8f);
  }
}

void drawChunks(const View &v, const Lines &l, uint8_t kind, float R) {
  if (l.nChunks == 0) return;
  if (l.maxX < v.px - R || l.minX > v.px + R || l.maxY < v.py - R || l.minY > v.py + R) return;
  float w;
  uint16_t col;
  if (kind == 0) {
    w = constrain(14.0f / v.mpp, 8.0f, 16.0f);
    col = C_ROUTE;
  } else {
    roadStyle(kind, v.mpp, w, col);
  }
  for (uint32_t ci = l.kindStart[kind]; ci < l.kindStart[kind + 1]; ci++) {
    const Chunk &c = l.chunks[ci];
    if (c.maxX < v.px - R || c.minX > v.px + R || c.maxY < v.py - R || c.minY > v.py + R) continue;
    float ax, ay;
    toScreen(v, l.px[c.start], l.py[c.start], ax, ay);
    for (uint16_t k = 1; k < c.count; k++) {
      float bx, by;
      toScreen(v, l.px[c.start + k], l.py[c.start + k], bx, by);
      wideSeg(ax, ay, bx, by, w, col, w >= 5);
      ax = bx;
      ay = by;
    }
  }
}

void drawMapScreen(const NavData &n, const PosTarget &t, uint32_t now) {
  gfx->fillScreen(C_MAP_BG);
  xSemaphoreTake(mapMutex, portMAX_DELAY);
  updateCamera(world.route, t, now);
  View v;
  v.px = cam.x;
  v.py = cam.y;
  float h = cam.heading * DEG_TO_RAD;
  v.cosH = cosf(h);
  v.sinH = sinf(h);
  v.mpp = cam.mpp;
  float R = 400 * v.mpp;
  const uint8_t order[4] = { 4, 3, 2, 1 };
  for (uint8_t oi = 0; oi < 4; oi++) {
    for (int i = 0; i < MAX_TILES; i++) {
      if (world.tiles[i].used) drawChunks(v, world.tiles[i].lines, order[oi], R);
    }
  }
  drawChunks(v, world.route.lines, 0, R);
  xSemaphoreGive(mapMutex);
  drawPositionMarker();
  drawMapOverlay(n, t.speed * 3.6f);
  AlertData a;
  if (curveActive(a)) drawCurveBadge(75, 290, a.angle, a.dist);
}

void drawWaitingMap(const NavData &n, const char *msg) {
  gfx->fillScreen(C_MAP_BG);
  drawMapOverlay(n, n.speedKmh);
  textCentered(msg, 300, u8g2_font_helvB18_tf, C_DIM);
}

void IRAM_ATTR onTouch() {
  touchPending = true;
}

void setup() {
  Serial.begin(115200);
  i2cMutex = xSemaphoreCreateMutex();
  mapMutex = xSemaphoreCreateMutex();
  gattSem = xSemaphoreCreateBinary();
  encSem = xSemaphoreCreateBinary();
  rxQueue = xQueueCreate(96, sizeof(RxItem));
  Wire.setBufferSize(GPS_CHUNK);
  Wire.begin(IIC_SDA, IIC_SCL, 100000);
  if (!gfx->begin()) {
    Serial.println("display init failed");
  }
  gfx->setUTF8Print(true);
  gfx->setTextWrap(false);
  panel->setBrightness(255);
  gfx->fillScreen(C_BG);
  gfx->flush();
  touch.setPins(TP_RESET, TP_INT);
  touchOk = touch.begin(Wire, CST92XX_SLAVE_ADDRESS);
  if (touchOk) {
    touch.setMaxCoordinates(LCD_W, LCD_H);
    touch.setMirrorXY(true, true);
    pinMode(TP_INT, INPUT_PULLUP);
    attachInterrupt(digitalPinToInterrupt(TP_INT), onTouch, FALLING);
  }
  pinMode(BOOT_BTN, INPUT_PULLUP);
  pmuOk = pmu.begin(Wire, AXP2101_SLAVE_ADDRESS, IIC_SDA, IIC_SCL);
  if (pmuOk) {
    pmu.enableBattDetection();
    pmu.enableBattVoltageMeasure();
  }
  lastActivity = millis();
  xTaskCreatePinnedToCore(rxTask, "rx", 8192, nullptr, 3, nullptr, 0);
  xTaskCreatePinnedToCore(gpsTask, "gps", 8192, nullptr, 1, nullptr, 0);
  xTaskCreatePinnedToCore(voiceTask, "voice", 8192, nullptr, 2, nullptr, 0);
  bleInit();
  xTaskCreatePinnedToCore(iosTask, "ios", 6144, nullptr, 2, nullptr, 0);
}

Tap readInput(uint32_t now) {
  static uint32_t lastTap = 0;
  static bool lastBtn = true;
  Tap t = {};
  static uint32_t pendingBtn = 0;
  bool btn = digitalRead(BOOT_BTN);
  if (lastBtn && !btn) {
    if (pendingBtn && now - pendingBtn < 450) {
      pendingBtn = 0;
      voiceRequested = true;
      lastActivity = now;
    } else {
      pendingBtn = now ? now : 1;
    }
  }
  lastBtn = btn;
  if (pendingBtn && now - pendingBtn >= 450) {
    pendingBtn = 0;
    t.tapped = true;
    t.button = true;
  }
  if (touchPending) {
    touchPending = false;
    if (touchOk) {
      int16_t x[2], y[2];
      xSemaphoreTake(i2cMutex, portMAX_DELAY);
      uint8_t pts = touch.getPoint(x, y, 1);
      xSemaphoreGive(i2cMutex);
      if (pts > 0) {
        t.tapped = true;
        t.x = x[0];
        t.y = y[0];
      }
    }
  }
  if (t.tapped && now - lastTap < 350) t.tapped = false;
  if (t.tapped) lastTap = now;
  return t;
}

uint8_t screensFor(uint8_t mode, uint8_t *list) {
  if (mode == 0) {
    list[0] = SCR_IDLE;
    list[1] = SCR_MUSIC;
    return 2;
  }
  if (mode == 2) {
    list[0] = SCR_MAP;
    list[1] = SCR_MUSIC;
    return 2;
  }
  list[0] = SCR_MAP;
  list[1] = SCR_ARROW;
  list[2] = SCR_MUSIC;
  return 3;
}

void applyDisplaySettings(uint32_t now) {
  bool fromPhone = settingsMs != 0 && now - settingsMs < 300000;
  uint8_t bright;
  if (fromPhone) {
    nightMode = setNight;
    bright = setBrightnessVal;
  } else {
    GpsData g;
    portENTER_CRITICAL(&gpsMux);
    g = gpsShared;
    portEXIT_CRITICAL(&gpsMux);
    nightMode = g.timeValid && (g.hour >= 19 || g.hour < 6);
    bright = nightMode ? 110 : 255;
  }
  static bool panelOff = false;
  static uint32_t lastApply = 0;
  if (screenOff) {
    if (!panelOff) {
      panel->setBrightness(0);
      panel->displayOff();
      panelOff = true;
      appliedBrightness = 0;
    }
    return;
  }
  if (panelOff) {
    panel->displayOn();
    panelOff = false;
    appliedBrightness = 0;
    navDirty = true;
  }
  if (bright != appliedBrightness || now - lastApply > 5000) {
    panel->setBrightness(bright);
    appliedBrightness = bright;
    lastApply = now;
  }
}

void loop() {
  static uint32_t lastDraw = 0;
  uint32_t now = millis();

  NavData n;
  portENTER_CRITICAL(&navMux);
  n = navShared;
  portEXIT_CRITICAL(&navMux);
  bool navOn = n.active && bleConnected && (now - lastNavMs < NAV_TIMEOUT_MS);
  bool cruise = navOn && (n.flags & 0x02);
  uint8_t mode = !navOn ? 0 : (cruise ? 2 : 1);
  if (mode == 1 && isTurn(n.maneuver) && n.distM <= 150 && !(n.flags & 0x01)) {
    if (!approaching) {
      approaching = true;
      approachStart = now;
      navDirty = true;
    }
  } else if (mode != 1 || n.distM > 180 || !isTurn(n.maneuver)) {
    approaching = false;
  }
  if (mode != lastMode) {
    lastMode = mode;
    screenIdx = 0;
    navDirty = true;
    lastActivity = now;
  }
  uint8_t list[3];
  uint8_t count = screensFor(mode, list);
  if (screenIdx >= count) screenIdx = 0;

  Tap tp = readInput(now);
  if (tp.tapped) {
    bool wasOff = screenOff;
    lastActivity = now;
    screenOff = false;
    navDirty = true;
    bool toastOn;
    portENTER_CRITICAL(&toastMux);
    toastOn = toastShared.active;
    portEXIT_CRITICAL(&toastMux);
    if (wasOff) {
    } else if (toastOn) {
      portENTER_CRITICAL(&toastMux);
      toastShared.active = false;
      portEXIT_CRITICAL(&toastMux);
    } else if (list[screenIdx] == SCR_MUSIC && !tp.button && tp.y > 290) {
      if (tp.x < CX - 60) musicCommand(4);
      else if (tp.x > CX + 60) musicCommand(3);
      else musicCommand(2);
    } else {
      screenIdx = (screenIdx + 1) % count;
    }
  }

  GpsData g;
  portENTER_CRITICAL(&gpsMux);
  g = gpsShared;
  portEXIT_CRITICAL(&gpsMux);
  updateRadarFlash(now);
  if (mode != 0 || otaActive) lastActivity = now;
  if (screenOff && now - lastActivity < SLEEP_AFTER_MS) {
    screenOff = false;
    navDirty = true;
  }
  if (!screenOff && now - lastActivity > SLEEP_AFTER_MS) {
    screenOff = true;
    navDirty = true;
  }
  applyDisplaySettings(now);
  if (screenOff) {
    delay(20);
    return;
  }

  PosTarget t;
  portENTER_CRITICAL(&posMux);
  t = posShared;
  portEXIT_CRITICAL(&posMux);
  bool mapReady = world.hasSession && t.valid && t.mapId == world.session && (now - t.ms) < 10000;
  uint8_t scr = list[screenIdx];
  bool animating = mode != 0 && scr == SCR_MAP && mapReady && !otaActive;

  uint32_t interval = animating ? 40 : ((approaching || radarFlashing(now)) ? 100 : (mode != 0 ? 250 : 1000));
  if (otaActive) interval = 200;
  if (!navDirty && now - lastDraw < interval) {
    delay(5);
    return;
  }
  navDirty = false;
  lastDraw = now;

  if (otaActive) {
    drawOtaScreen();
    gfx->flush();
    return;
  }
  if (scr == SCR_MUSIC) {
    drawMusicScreen();
  } else if (mode == 0) {
    cam.init = false;
    drawIdleScreen(g, bleConnected);
  } else if (scr == SCR_ARROW) {
    drawNavScreen(n);
  } else if (mapReady) {
    drawMapScreen(n, t, now);
  } else {
    int mp = mapProgress;
    char msg[40];
    if (mp >= 0 && mp < 100) snprintf(msg, sizeof(msg), "Recebendo mapa %d%%", mp);
    else snprintf(msg, sizeof(msg), "Aguardando mapa...");
    drawWaitingMap(n, msg);
  }
  if (mode == 1) drawAlertRing();
  if (mode != 0) drawRadarFlash(now);
  drawBatteryIcon();
  drawToast();
  gfx->flush();
}
