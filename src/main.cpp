#include <Arduino.h>
#include <math.h>
#include <ESP32-HUB75-MatrixPanel-I2S-DMA.h>
#include <JPEGDEC.h>
#include "camera.h"
#include "wifi-connection.h"

#define SCREEN_WIDTH 64
#define SCREEN_HEIGHT 64
#define PANEL_CHAIN 1

static const uint32_t RETRY_DELAY_MS = 1000;
static const float GAMMA = 2.2f;  // les LED sont lineaires, l'image camera est en sRGB

MatrixPanel_I2S_DMA* display = nullptr;
static JPEGDEC jpeg;

// Accumulateurs pour la reduction par moyenne de blocs vers 64x64
static uint16_t sumR[SCREEN_HEIGHT][SCREEN_WIDTH];
static uint16_t sumG[SCREEN_HEIGHT][SCREEN_WIDTH];
static uint16_t sumB[SCREEN_HEIGHT][SCREEN_WIDTH];
static uint8_t count[SCREEN_HEIGHT][SCREEN_WIDTH];
static uint8_t gammaLut[256];

// Region carree centree de l'image (deja reduite par JPEGDEC) projetee sur la dalle
static int cropX, cropY, cropSize;

static int onJpegBlock(JPEGDRAW* block) {
  for (int y = 0; y < block->iHeight; y++) {
    int sy = block->y + y - cropY;
    if (sy < 0 || sy >= cropSize) continue;
    int dy = sy * SCREEN_HEIGHT / cropSize;
    const uint16_t* row = block->pPixels + y * block->iWidth;
    for (int x = 0; x < block->iWidth; x++) {
      int sx = block->x + x - cropX;
      if (sx < 0 || sx >= cropSize) continue;
      int dx = sx * SCREEN_WIDTH / cropSize;
      uint16_t c = row[x];
      sumR[dy][dx] += (c >> 8) & 0xF8;
      sumG[dy][dx] += (c >> 3) & 0xFC;
      sumB[dy][dx] += (c << 3) & 0xF8;
      if (count[dy][dx] < 255) count[dy][dx]++;
    }
  }
  return 1;
}

static bool showJpeg(uint8_t* data, size_t len) {
  if (!jpeg.openRAM(data, len, onJpegBlock)) {
    Serial.printf("JPEG invalide (%u octets)\n", len);
    return false;
  }
  int w = jpeg.getWidth();
  int h = jpeg.getHeight();

  // Plus forte reduction materielle qui garde au moins 64 px sur le petit cote
  int scale = 1, option = 0;
  const int scales[] = {8, 4, 2};
  const int options[] = {JPEG_SCALE_EIGHTH, JPEG_SCALE_QUARTER, JPEG_SCALE_HALF};
  for (int i = 0; i < 3; i++) {
    if (min(w, h) / scales[i] >= SCREEN_WIDTH) {
      scale = scales[i];
      option = options[i];
      break;
    }
  }
  int sw = w / scale, sh = h / scale;
  cropSize = min(sw, sh);
  cropX = (sw - cropSize) / 2;
  cropY = (sh - cropSize) / 2;

  memset(sumR, 0, sizeof(sumR));
  memset(sumG, 0, sizeof(sumG));
  memset(sumB, 0, sizeof(sumB));
  memset(count, 0, sizeof(count));

  jpeg.setPixelType(RGB565_LITTLE_ENDIAN);
  bool ok = jpeg.decode(0, 0, option);
  jpeg.close();
  if (!ok) {
    Serial.printf("Decodage JPEG en echec (erreur %d)\n", jpeg.getLastError());
    return false;
  }

  for (int y = 0; y < SCREEN_HEIGHT; y++) {
    for (int x = 0; x < SCREEN_WIDTH; x++) {
      uint8_t n = count[y][x];
      if (!n) continue;
      display->drawPixelRGB888(x, y, gammaLut[sumR[y][x] / n], gammaLut[sumG[y][x] / n], gammaLut[sumB[y][x] / n]);
    }
  }
  return true;
}

void setup() {
  Serial.begin(115200);

  // Meme configuration de dalle que led-matrix-esp32
  HUB75_I2S_CFG mxconfig(SCREEN_WIDTH, SCREEN_HEIGHT, PANEL_CHAIN);
  mxconfig.gpio.e = 32;
  mxconfig.clkphase = false;
  mxconfig.driver = HUB75_I2S_CFG::FM6126A;
  mxconfig.i2sspeed = HUB75_I2S_CFG::HZ_20M;

  display = new MatrixPanel_I2S_DMA(mxconfig);
  display->begin();
  display->setBrightness8(40);  // 0-255, fixe (pas de capteur de luminosite)
  display->clearScreen();

  for (int i = 0; i < 256; i++) gammaLut[i] = (uint8_t)(powf(i / 255.0f, GAMMA) * 255.0f + 0.5f);

  WifiConnection::setup();
  if (!Camera::setup()) Serial.println("Pas assez de memoire pour le tampon JPEG");
}

void loop() {
  if (!WifiConnection::ensureConnected()) {
    delay(RETRY_DELAY_MS);
    return;
  }

  uint32_t start = millis();
  uint8_t* data;
  size_t len = Camera::fetchSnapshot(&data);
  uint32_t fetched = millis();
  if (!len || !showJpeg(data, len)) {
    delay(RETRY_DELAY_MS);
    return;
  }
  Serial.printf("Image %u octets : telechargement %u ms, decodage %u ms\n", len, fetched - start, millis() - fetched);
}
