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
#define C_MAP_BG 0x0841
#define C_PANEL 0x0000
#define C_ROUTE 0x2D7F
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

struct Chunk {
  int32_t minX, minY, maxX, maxY;
  uint32_t start;
  uint16_t count;
  uint8_t kind;
};

struct MapData {
  bool ready;
  uint16_t id;
  int32_t *px;
  int32_t *py;
  uint32_t nPts;
  Chunk *chunks;
  uint32_t nChunks;
  float *rx;
  float *ry;
  float *rcum;
  uint32_t nRoute;
};

struct View {
  float px, py;
  float cosH, sinH;
  float mpp;
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
SemaphoreHandle_t i2cMutex;
SemaphoreHandle_t mapMutex;
QueueHandle_t rxQueue;

NavData navShared = {};
GpsData gpsShared = {};
PosTarget posShared = {};
MapData mapData = {};
volatile uint32_t lastNavMs = 0;
volatile bool navDirty = true;
volatile bool bleConnected = false;
volatile int mapProgress = -1;
volatile bool touchPending = false;
bool showMap = true;

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

void gpsTask(void *) {
  for (;;) {
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

void freeMap(MapData &m) {
  if (m.px) heap_caps_free(m.px);
  if (m.py) heap_caps_free(m.py);
  if (m.chunks) heap_caps_free(m.chunks);
  if (m.rx) heap_caps_free(m.rx);
  if (m.ry) heap_caps_free(m.ry);
  if (m.rcum) heap_caps_free(m.rcum);
  m = {};
}

void *psAlloc(size_t bytes) {
  return heap_caps_malloc(bytes > 0 ? bytes : 4, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
}

int drawOrder(uint8_t kind) {
  switch (kind) {
    case 4: return 0;
    case 3: return 1;
    case 2: return 2;
    case 1: return 3;
    case 0: return 5;
    default: return 0;
  }
}

bool parseMap(const uint8_t *b, uint32_t len, uint16_t id, MapData &m) {
  if (len < 6 || memcmp(b, "MNM1", 4) != 0) return false;
  uint16_t count = rd16(b + 4);
  uint32_t off = 6;
  uint32_t totalPts = 0, totalChunks = 0, routePts = 0;
  for (uint16_t i = 0; i < count; i++) {
    if (off + 11 > len) return false;
    uint8_t kind = b[off];
    uint16_t n = rd16(b + off + 1);
    uint32_t need = 11 + (uint32_t)(n > 0 ? n - 1 : 0) * 4;
    if (n == 0 || off + need > len) return false;
    totalPts += n;
    if (n >= 2) totalChunks += (n - 2) / (CHUNK_PTS - 1) + 1;
    if (kind == 0 && routePts == 0) routePts = n;
    off += need;
  }
  MapData r = {};
  r.id = id;
  r.px = (int32_t *)psAlloc(totalPts * sizeof(int32_t));
  r.py = (int32_t *)psAlloc(totalPts * sizeof(int32_t));
  r.chunks = (Chunk *)psAlloc(totalChunks * sizeof(Chunk));
  r.rx = (float *)psAlloc(routePts * sizeof(float));
  r.ry = (float *)psAlloc(routePts * sizeof(float));
  r.rcum = (float *)psAlloc(routePts * sizeof(float));
  if (!r.px || !r.py || !r.chunks || !r.rx || !r.ry || !r.rcum) {
    freeMap(r);
    return false;
  }
  off = 6;
  uint32_t pi = 0, ci = 0;
  bool routeDone = false;
  for (uint16_t i = 0; i < count; i++) {
    uint8_t kind = b[off];
    uint16_t n = rd16(b + off + 1);
    int32_t x = (int32_t)rd32(b + off + 3);
    int32_t y = (int32_t)rd32(b + off + 7);
    off += 11;
    uint32_t base = pi;
    r.px[pi] = x;
    r.py[pi] = y;
    pi++;
    for (uint16_t k = 1; k < n; k++) {
      x += (int16_t)rd16(b + off);
      y += (int16_t)rd16(b + off + 2);
      off += 4;
      r.px[pi] = x;
      r.py[pi] = y;
      pi++;
    }
    if (kind == 0 && !routeDone) {
      routeDone = true;
      r.nRoute = n;
      float cum = 0;
      for (uint16_t k = 0; k < n; k++) {
        r.rx[k] = r.px[base + k];
        r.ry[k] = r.py[base + k];
        if (k > 0) {
          float dx = r.rx[k] - r.rx[k - 1], dy = r.ry[k] - r.ry[k - 1];
          cum += sqrtf(dx * dx + dy * dy);
        }
        r.rcum[k] = cum;
      }
    }
    if (n < 2) continue;
    for (uint32_t s = 0; s + 1 < n; s += CHUNK_PTS - 1) {
      uint32_t cnt = min<uint32_t>(CHUNK_PTS, n - s);
      Chunk &c = r.chunks[ci++];
      c.start = base + s;
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
    return drawOrder(a.kind) < drawOrder(b.kind);
  });
  r.ready = true;
  m = r;
  return true;
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
    MapData fresh = {};
    bool ok = parseMap(rxBuf, rxTotal, id, fresh);
    heap_caps_free(rxBuf);
    rxBuf = nullptr;
    if (!ok) {
      mapProgress = -1;
      return;
    }
    xSemaphoreTake(mapMutex, portMAX_DELAY);
    MapData old = mapData;
    mapData = fresh;
    xSemaphoreGive(mapMutex);
    freeMap(old);
    mapProgress = 100;
  }
}

void rxTask(void *) {
  RxItem it;
  for (;;) {
    if (xQueueReceive(rxQueue, &it, portMAX_DELAY) != pdTRUE) continue;
    if (it.len == 0) continue;
    switch (it.data[0]) {
      case 0x01: handleNavPacket(it.data, it.len); break;
      case 0x02: handlePosPacket(it.data, it.len); break;
      case 0x10:
      case 0x11:
      case 0x12: handleMapPacket(it.data, it.len); break;
      default: break;
    }
  }
}

class ServerCallbacks : public BLEServerCallbacks {
  void onConnect(BLEServer *s) override {
    bleConnected = true;
    navDirty = true;
  }
  void onDisconnect(BLEServer *s) override {
    bleConnected = false;
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
    xQueueSend(rxQueue, &it, pdMS_TO_TICKS(200));
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
  svc->start();
  BLEAdvertising *adv = BLEDevice::getAdvertising();
  adv->addServiceUUID(SERVICE_UUID);
  adv->setScanResponse(true);
  BLEDevice::startAdvertising();
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

void drawNavScreen(const NavData &n) {
  gfx->fillScreen(C_BG);
  char b[48];
  snprintf(b, sizeof(b), "%u km/h", n.speedKmh);
  textCentered(b, 62, u8g2_font_helvB18_tf, C_DIM);
  if (n.maneuver == M_ARRIVE && n.distM < 30) {
    textCentered("Chegou", 120, u8g2_font_logisoso42_tr, C_GREEN);
  } else {
    formatDistance(n.distM, b, sizeof(b));
    textCentered(b, 120, u8g2_font_logisoso42_tr, C_FG);
  }
  drawManeuver(n, CX, 220, 1.0f, C_BG);
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
    case 1: w = 9 * z; c = 0x9CD3; break;
    case 2: w = 7 * z; c = 0x7BEF; break;
    case 3: w = 5 * z; c = 0x5AEB; break;
    default: w = 4 * z; c = 0x4208; break;
  }
}

void drawPositionMarker() {
  int x = CX, y = MAP_CY;
  gfx->fillCircle(x, y, 22, C_ROUTE_EDGE);
  gfx->fillTriangle(x, y - 26, x - 17, y + 16, x + 17, y + 16, C_FG);
  gfx->fillTriangle(x - 17, y + 16, x, y + 6, x + 17, y + 16, C_ROUTE_EDGE);
}

void drawMapOverlay(const NavData &n, float speedKmh) {
  gfx->fillRect(0, 0, LCD_W, 150, C_PANEL);
  gfx->drawFastHLine(0, 150, LCD_W, 0x2104);
  char b[48];
  NavData mini = n;
  drawManeuver(mini, CX - 115, 92, 0.42f, C_PANEL);
  if (n.maneuver == M_ARRIVE && n.distM < 30) {
    textAt("Chegou", CX - 65, 108, u8g2_font_logisoso42_tr, C_GREEN);
  } else {
    formatDistance(n.distM, b, sizeof(b));
    textAt(b, CX - 65, 108, u8g2_font_logisoso42_tr, C_FG);
  }
  if (n.flags & 0x01) {
    textCentered("Recalculando...", 140, u8g2_font_helvB18_tf, C_ORANGE);
  } else {
    char s[128];
    strncpy(s, n.text, sizeof(s) - 1);
    s[sizeof(s) - 1] = 0;
    gfx->setFont(u8g2_font_helvB18_tf);
    fitText(s, 300);
    textCentered(s, 140, u8g2_font_helvB18_tf, C_ACCENT);
  }
  gfx->fillRoundRect(CX - 62, 410, 124, 40, 20, C_PANEL);
  snprintf(b, sizeof(b), "%d km/h", (int)roundf(speedKmh));
  textCentered(b, 438, u8g2_font_helvB18_tf, C_FG);
}

float routePointAt(const MapData &m, float along, float &x, float &y) {
  if (m.nRoute < 2) return 0;
  float total = m.rcum[m.nRoute - 1];
  float d = constrain(along, 0.0f, total);
  uint32_t lo = 0, hi = m.nRoute - 1;
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

float routeBearingAt(const MapData &m, float along) {
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

struct Camera {
  bool init;
  float x, y, along, heading, mpp;
  uint32_t lastMs;
} cam = {};

void updateCamera(const MapData &m, const PosTarget &t, uint32_t now) {
  float dt = cam.init ? (now - cam.lastMs) / 1000.0f : 0;
  dt = constrain(dt, 0.0f, 0.2f);
  cam.lastMs = now;
  float age = constrain((now - t.ms) / 1000.0f, 0.0f, 3.0f);
  float targetHeading = cam.heading;
  bool onRoute = t.along >= 0 && m.nRoute >= 2;
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

void drawMapScreen(const NavData &n, const PosTarget &t, uint32_t now) {
  gfx->fillScreen(C_MAP_BG);
  xSemaphoreTake(mapMutex, portMAX_DELAY);
  const MapData &m = mapData;
  updateCamera(m, t, now);
  View v;
  v.px = cam.x;
  v.py = cam.y;
  float h = cam.heading * DEG_TO_RAD;
  v.cosH = cosf(h);
  v.sinH = sinf(h);
  v.mpp = cam.mpp;
  float R = 400 * v.mpp;
  for (uint32_t ci = 0; ci < m.nChunks; ci++) {
    const Chunk &c = m.chunks[ci];
    if (c.maxX < v.px - R || c.minX > v.px + R || c.maxY < v.py - R || c.minY > v.py + R) continue;
    float w;
    uint16_t col;
    bool isRoute = c.kind == 0;
    if (isRoute) {
      w = constrain(14.0f / v.mpp, 8.0f, 16.0f);
      col = C_ROUTE;
    } else {
      roadStyle(c.kind, v.mpp, w, col);
    }
    float ax, ay;
    toScreen(v, m.px[c.start], m.py[c.start], ax, ay);
    for (uint16_t k = 1; k < c.count; k++) {
      float bx, by;
      toScreen(v, m.px[c.start + k], m.py[c.start + k], bx, by);
      wideSeg(ax, ay, bx, by, w, col, w >= 5);
      ax = bx;
      ay = by;
    }
  }
  xSemaphoreGive(mapMutex);
  drawPositionMarker();
  drawMapOverlay(n, t.speed * 3.6f);
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
    pinMode(TP_INT, INPUT_PULLUP);
    attachInterrupt(digitalPinToInterrupt(TP_INT), onTouch, FALLING);
  }
  pinMode(BOOT_BTN, INPUT_PULLUP);
  xTaskCreatePinnedToCore(rxTask, "rx", 8192, nullptr, 3, nullptr, 0);
  xTaskCreatePinnedToCore(gpsTask, "gps", 8192, nullptr, 1, nullptr, 0);
  bleInit();
}

bool checkToggle(uint32_t now) {
  static uint32_t lastToggle = 0;
  static bool lastBtn = true;
  bool toggle = false;
  bool btn = digitalRead(BOOT_BTN);
  if (lastBtn && !btn) toggle = true;
  lastBtn = btn;
  if (touchPending) {
    touchPending = false;
    if (touchOk) {
      int16_t x[2], y[2];
      xSemaphoreTake(i2cMutex, portMAX_DELAY);
      uint8_t pts = touch.getPoint(x, y, 1);
      xSemaphoreGive(i2cMutex);
      if (pts > 0) toggle = true;
    }
  }
  if (toggle && now - lastToggle > 500) {
    lastToggle = now;
    return true;
  }
  return false;
}

void loop() {
  static uint32_t lastDraw = 0;
  uint32_t now = millis();

  NavData n;
  portENTER_CRITICAL(&navMux);
  n = navShared;
  portEXIT_CRITICAL(&navMux);
  bool navOn = n.active && bleConnected && (now - lastNavMs < NAV_TIMEOUT_MS);

  if (checkToggle(now) && navOn) {
    showMap = !showMap;
    navDirty = true;
  }

  PosTarget t;
  portENTER_CRITICAL(&posMux);
  t = posShared;
  portEXIT_CRITICAL(&posMux);
  bool mapReady = mapData.ready && t.valid && t.mapId == mapData.id && (now - t.ms) < 10000;
  bool animating = navOn && showMap && mapReady;

  uint32_t interval = animating ? 40 : (navOn ? 250 : 1000);
  if (!navDirty && now - lastDraw < interval) {
    delay(5);
    return;
  }
  navDirty = false;
  lastDraw = now;

  if (!navOn) {
    cam.init = false;
    GpsData g;
    portENTER_CRITICAL(&gpsMux);
    g = gpsShared;
    portEXIT_CRITICAL(&gpsMux);
    drawIdleScreen(g, bleConnected);
  } else if (!showMap) {
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
  gfx->flush();
}
