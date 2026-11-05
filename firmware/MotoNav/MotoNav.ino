#include <Arduino.h>
#include <Wire.h>
#include <math.h>
#include <U8g2lib.h>
#include "Arduino_GFX_Library.h"
#include <TinyGPSPlus.h>
#include <BLEDevice.h>
#include <BLEServer.h>
#include <BLEUtils.h>
#include <BLE2902.h>

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

#define GPS_ADDR_W 0x50
#define GPS_ADDR_R 0x54
#define GPS_CHUNK 2048
#define TZ_OFFSET_H -3

#define DEVICE_NAME "MotoNav"
#define SERVICE_UUID "6e0a0001-7a3b-4c2d-9e1f-0b5a4d3c2b1a"
#define NAV_CHAR_UUID "6e0a0002-7a3b-4c2d-9e1f-0b5a4d3c2b1a"

#define CX (LCD_W / 2)
#define NAV_TIMEOUT_MS 20000

#define C_BG RGB565_BLACK
#define C_FG RGB565_WHITE
#define C_DIM 0x8410
#define C_ACCENT 0x07FF
#define C_GREEN 0x47E8
#define C_ORANGE 0xFD20

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

Arduino_DataBus *bus = new Arduino_ESP32QSPI(LCD_CS, LCD_SCLK, LCD_SDIO0, LCD_SDIO1, LCD_SDIO2, LCD_SDIO3);
Arduino_CO5300 *panel = new Arduino_CO5300(bus, LCD_RESET, 0, LCD_W, LCD_H, 6, 0, 0, 0);
Arduino_Canvas *gfx = new Arduino_Canvas(LCD_W, LCD_H, panel);
TinyGPSPlus gps;

portMUX_TYPE navMux = portMUX_INITIALIZER_UNLOCKED;
portMUX_TYPE gpsMux = portMUX_INITIALIZER_UNLOCKED;
NavData navShared = {};
GpsData gpsShared = {};
volatile uint32_t lastNavMs = 0;
volatile bool navDirty = true;
volatile bool bleConnected = false;
BLEServer *bleServer = nullptr;

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

void readGps() {
  const uint8_t askLen[] = { 0x08, 0x00, 0x51, 0xAA, 0x04, 0x00, 0x00, 0x00 };
  uint8_t lenBuf[4] = { 0 };
  if (!i2cWrite(GPS_ADDR_W, askLen, sizeof(askLen))) return;
  vTaskDelay(pdMS_TO_TICKS(100));
  if (!i2cRead(GPS_ADDR_R, lenBuf, sizeof(lenBuf))) return;
  uint32_t len = lenBuf[0] | (lenBuf[1] << 8) | (lenBuf[2] << 16) | ((uint32_t)lenBuf[3] << 24);
  if (len == 0) return;
  if (len > GPS_CHUNK) len = GPS_CHUNK;
  const uint8_t askData[] = { 0x00, 0x20, 0x51, 0xAA, (uint8_t)(len & 0xFF), (uint8_t)((len >> 8) & 0xFF), 0x00, 0x00 };
  vTaskDelay(pdMS_TO_TICKS(100));
  if (!i2cWrite(GPS_ADDR_W, askData, sizeof(askData))) return;
  static uint8_t data[GPS_CHUNK];
  vTaskDelay(pdMS_TO_TICKS(100));
  if (!i2cRead(GPS_ADDR_R, data, len)) return;
  for (uint32_t i = 0; i < len; i++) gps.encode(data[i]);
}

void gpsTask(void *) {
  Wire.setBufferSize(GPS_CHUNK);
  Wire.begin(IIC_SDA, IIC_SCL, 100000);
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

class NavCallbacks : public BLECharacteristicCallbacks {
  void onWrite(BLECharacteristic *c) override {
    const uint8_t *p = c->getData();
    size_t n = c->getLength();
    if (n < 18 || p[0] != 1) return;
    NavData d = {};
    d.active = p[1] == 1;
    d.maneuver = p[2];
    d.exitNumber = p[3];
    d.angle = (int16_t)(p[4] | (p[5] << 8));
    d.distM = p[6] | (p[7] << 8) | (p[8] << 16) | ((uint32_t)p[9] << 24);
    d.remainM = p[10] | (p[11] << 8) | (p[12] << 16) | ((uint32_t)p[13] << 24);
    d.remainMin = p[14] | (p[15] << 8);
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
};

void bleInit() {
  BLEDevice::init(DEVICE_NAME);
  BLEDevice::setMTU(247);
  bleServer = BLEDevice::createServer();
  bleServer->setCallbacks(new ServerCallbacks());
  BLEService *svc = bleServer->createService(SERVICE_UUID);
  BLECharacteristic *ch = svc->createCharacteristic(NAV_CHAR_UUID, BLECharacteristic::PROPERTY_WRITE | BLECharacteristic::PROPERTY_WRITE_NR);
  ch->setCallbacks(new NavCallbacks());
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

void textCentered(const char *s, int baseline, const uint8_t *font, uint16_t color) {
  gfx->setFont(font);
  gfx->setTextColor(color);
  int w = textWidth(s);
  gfx->setCursor(CX - w / 2, baseline);
  gfx->print(s);
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
  if (li > 0) {
    char *last = lines[li - 1];
    bool truncated = wi < nw;
    if (truncated || textWidth(last) > maxW) {
      size_t L = strlen(last);
      for (;;) {
        char test[170];
        snprintf(test, sizeof(test), "%s...", last);
        if (textWidth(test) <= maxW || L == 0) {
          strcpy(last, test);
          break;
        }
        L--;
        while (L > 0 && ((uint8_t)last[L] & 0xC0) == 0x80) L--;
        last[L] = 0;
      }
    }
  }
  int startY = baseline - ((li - 1) * lineGap) / 2;
  for (int i = 0; i < li; i++) {
    int w = textWidth(lines[i]);
    gfx->setCursor(CX - w / 2, startY + i * lineGap);
    gfx->print(lines[i]);
  }
}

void thickLine(float x0, float y0, float x1, float y1, int r, uint16_t c) {
  float dx = x1 - x0, dy = y1 - y0;
  float len = sqrtf(dx * dx + dy * dy);
  int n = max(1, (int)(len / 2));
  for (int i = 0; i <= n; i++) {
    float t = (float)i / n;
    gfx->fillCircle(x0 + dx * t, y0 + dy * t, r, c);
  }
}

void arrowHead(float x, float y, float ang, float size, uint16_t c) {
  float dx = sinf(ang), dy = -cosf(ang);
  float px = cosf(ang), py = sinf(ang);
  float tx = x + dx * size, ty = y + dy * size;
  float bx = x - dx * size * 0.15f, by = y - dy * size * 0.15f;
  gfx->fillTriangle(tx, ty, bx + px * size * 0.8f, by + py * size * 0.8f, bx - px * size * 0.8f, by - py * size * 0.8f, c);
}

void drawTurn(int cx, int cy, float deg, uint16_t c) {
  const int r = 15;
  float a = deg * DEG_TO_RAD;
  if (fabsf(deg) < 10) {
    thickLine(cx, cy + 80, cx, cy - 30, r, c);
    arrowHead(cx, cy - 30, 0, 60, c);
    return;
  }
  float shift = -sinf(a) * 40;
  float sx = cx + shift;
  float kneeY = cy + 15;
  thickLine(sx, cy + 80, sx, kneeY, r, c);
  float len = 75;
  float ex = sx + sinf(a) * len, ey = kneeY - cosf(a) * len;
  thickLine(sx, kneeY, ex, ey, r, c);
  arrowHead(ex, ey, a, 55, c);
}

void drawUTurn(int cx, int cy, uint16_t c) {
  const int r = 15;
  const float rad = 40;
  float topY = cy - 20;
  thickLine(cx + rad, cy + 80, cx + rad, topY, r, c);
  float px = cx + rad, py = topY;
  for (int i = 1; i <= 36; i++) {
    float t = PI * i / 36.0f;
    float nx = cx + rad * cosf(t), ny = topY - rad * sinf(t);
    thickLine(px, py, nx, ny, r, c);
    px = nx;
    py = ny;
  }
  thickLine(cx - rad, topY, cx - rad, cy + 30, r, c);
  arrowHead(cx - rad, cy + 30, PI, 55, c);
}

void drawRoundabout(int cx, int cy, float deg, uint8_t exitNo, uint16_t c) {
  const int ringR = 42;
  const int ringT = 13;
  int rcY = cy + 15;
  thickLine(cx, cy + 95, cx, rcY + ringR, 15, c);
  for (int t = -ringT / 2; t <= ringT / 2; t++) gfx->drawCircle(cx, rcY, ringR + t, c);
  for (int t = -ringT / 2; t <= ringT / 2; t++) gfx->drawCircle(cx, rcY, ringR + t + 1, c);
  float a = constrain(deg, -170.0f, 170.0f) * DEG_TO_RAD;
  float sx = cx + sinf(a) * ringR, sy = rcY - cosf(a) * ringR;
  float ex = cx + sinf(a) * (ringR + 30), ey = rcY - cosf(a) * (ringR + 30);
  thickLine(sx, sy, ex, ey, 15, c);
  arrowHead(ex, ey, a, 38, c);
  if (exitNo > 0) {
    char b[4];
    snprintf(b, sizeof(b), "%u", exitNo);
    textCentered(b, rcY + 12, u8g2_font_helvB24_tf, C_ACCENT);
  }
}

void drawFlag(int cx, int cy, uint16_t c) {
  int px = cx - 55;
  thickLine(px, cy + 80, px, cy - 85, 8, c);
  int fx = px + 6, fy = cy - 85, fw = 120, fh = 80;
  int cw = fw / 4, chh = fh / 3;
  for (int r = 0; r < 3; r++) {
    for (int k = 0; k < 4; k++) {
      uint16_t col = ((r + k) % 2 == 0) ? c : C_BG;
      gfx->fillRect(fx + k * cw, fy + r * chh, cw, chh, col);
    }
  }
  gfx->drawRect(fx, fy, cw * 4, chh * 3, c);
}

void drawManeuver(const NavData &n, int cx, int cy) {
  switch (n.maneuver) {
    case M_UTURN: drawUTurn(cx, cy, C_FG); break;
    case M_ROUNDABOUT: drawRoundabout(cx, cy, n.angle, n.exitNumber, C_FG); break;
    case M_ARRIVE: drawFlag(cx, cy, C_FG); break;
    case M_STRAIGHT:
    case M_DEPART:
    case M_NONE: drawTurn(cx, cy, 0, C_FG); break;
    case M_KEEP_RIGHT: drawTurn(cx, cy, 30, C_FG); break;
    case M_KEEP_LEFT: drawTurn(cx, cy, -30, C_FG); break;
    case M_SLIGHT_RIGHT: drawTurn(cx, cy, 45, C_FG); break;
    case M_SLIGHT_LEFT: drawTurn(cx, cy, -45, C_FG); break;
    case M_RIGHT: drawTurn(cx, cy, 90, C_FG); break;
    case M_LEFT: drawTurn(cx, cy, -90, C_FG); break;
    case M_SHARP_RIGHT: drawTurn(cx, cy, 135, C_FG); break;
    case M_SHARP_LEFT: drawTurn(cx, cy, -135, C_FG); break;
    default: drawTurn(cx, cy, constrain((float)n.angle, -150.0f, 150.0f), C_FG); break;
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
  drawManeuver(n, CX, 220);
  if (n.flags & 0x01) {
    textCentered("Recalculando...", 372, u8g2_font_helvB24_tf, C_ORANGE);
  } else {
    wrapCentered(n.text, 372, 32, 330, 2, u8g2_font_helvB24_tf, C_ACCENT);
  }
  char d[16], t[16];
  formatDistance(n.remainM, d, sizeof(d));
  formatMinutes(n.remainMin, t, sizeof(t));
  snprintf(b, sizeof(b), "%s · %s", d, t);
  textCentered(b, 432, u8g2_font_helvB18_tf, C_DIM);
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

void setup() {
  Serial.begin(115200);
  if (!gfx->begin()) {
    Serial.println("display init failed");
  }
  gfx->setUTF8Print(true);
  gfx->setTextWrap(false);
  panel->setBrightness(255);
  gfx->fillScreen(C_BG);
  gfx->flush();
  bleInit();
  xTaskCreatePinnedToCore(gpsTask, "gps", 8192, nullptr, 1, nullptr, 0);
}

void loop() {
  static uint32_t lastDraw = 0;
  uint32_t now = millis();
  if (!navDirty && now - lastDraw < 1000) {
    delay(20);
    return;
  }
  navDirty = false;
  lastDraw = now;

  NavData n;
  portENTER_CRITICAL(&navMux);
  n = navShared;
  portEXIT_CRITICAL(&navMux);
  GpsData g;
  portENTER_CRITICAL(&gpsMux);
  g = gpsShared;
  portEXIT_CRITICAL(&gpsMux);

  bool navOn = n.active && bleConnected && (now - lastNavMs < NAV_TIMEOUT_MS);
  if (navOn) drawNavScreen(n);
  else drawIdleScreen(g, bleConnected);
  gfx->flush();
}
