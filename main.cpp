/*******************************************************
 * DataLogger (portrait) - ESP32 C3 Super mini + ST7735 1.8" + SD + INA226
 * Rotação: tft.setRotation(0) (retrato)
 * - LEFT_EXTEND para fechar a borda esquerda
 * - Ícones do SD e Bateria sem extend (não deformam)
 * - Bateria com swap R<->B opcional (BAT_SWAP_RB)
 * - UI sem linhas/bordas; blocos sólidos + faixas BG
 * - Alinhamento: V=3 esp, I/P=2 esp, Q/E=1 esp
 * - CSV a cada 5 s; Estados: Standby(azul), Gravando(verde), Pausa(amarelo), SD fail(vermelho)
 *******************************************************/

#include <SPI.h>
#include <SD.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_ST7735.h>
#include <INA226.h>   // robtillaart/INA226

// ======== CONFIG DO PAINEL ========
#define PANEL_TAB INITR_GREENTAB
// #define PANEL_TAB INITR_BLACKTAB

// ======== TUNING DE BORDA ESQUERDA ========
#define LEFT_EXTEND 4  // ajuste fino

// ======== PINOS ========
#define TFT_CS     21
#define TFT_DC     1
#define TFT_RST    3
#define SD_CS      7
#define BOTAO_PIN  2
#define BATERIA_PIN 0

Adafruit_ST7735 tft(TFT_CS, TFT_DC, TFT_RST);

// ======== CORES ========
#define RGB565(r,g,b)  (uint16_t)((((r)&0xF8)<<8)|(((g)&0xFC)<<3)|((b)>>3))
#define COL_BG     RGB565(0,   0,   0)     // Black (fundo, assumido correto)
#define COL_TEXT   RGB565(255, 255, 255)   // White (texto, assumido correto)
#define C_BOX1     RGB565(0,   0,   80)    // Azul escuro (assumido correto)
#define C_BOX2     RGB565(64,  64,  64)    // Cinza (assumido correto)
#define C_BOX3     RGB565(128, 0,   0)     // Vermelho escuro (assumido correto)
#define C_BOX4     RGB565(0,   80,  0)     // Verde escuro (assumido correto)
#define C_BOX5     RGB565(0,   80,  80)    // Ciano escuro (assumido correto)
#define C_BOX6     RGB565(180, 160, 0)     // Amarelo escuro (assumido correto)
#define C_OK       RGB565(0,   255, 0)     // Green (GRAVANDO, assumido correto)
#define C_WARN     RGB565(0,   255, 255)   // Cyan (para amarelo visual em PAUSA)
#define C_INFO     RGB565(255, 0,   0)     // Red (para azul visual em STANDBY)
#define C_ERR      RGB565(0,   0,   255)   // Blue (para vermelho visual em SD falha)

// ======== INA226 ========
INA226 ina(0x44);
const float R_SHUNT_OHMS = 0.1f;
const float I_MAX_A      = 3.0f;

// ======== ESTADOS ========
enum Estado { STANDBY = 0, GRAVANDO = 1, PAUSA = 2 };
volatile bool btnPressed = false;
Estado estado = STANDBY;

// =================== VALORES NA TELA ===================

char vVal[8], iVal[8], pVal[8], qVal[8], eVal[8], sT[10];

// ======= Variáveis para atualização incremental / anti-flicker =======
static int16_t prevV10 = -1;
static int16_t prevI10 = -1;
static int16_t prevP10 = -1;
static uint32_t prevQ10 = 0xFFFFFFFF;
static uint32_t prevE10 = 0xFFFFFFFF;
static uint32_t prevSecs = 0xFFFFFFFF;

// ======== Ícones ===========
static bool prevSdOK = false;
static uint16_t prevBatColIcon = 0xFFFF;
static int8_t prevBatFillWIcon = -1;
static Estado prevEstado = STANDBY;

// ======== SD / LOG ========
bool sdOK = false;
File logFile;
uint16_t sampleIndex = 0;

// --- SPI guard: sempre liberar o TFT antes de falar com o SD ---
#define TFT_IDLE()        digitalWrite(TFT_CS, HIGH)
#define SD_SELECT_SAFE()  do { TFT_IDLE(); /* SD lib gerencia SD_CS */ } while (0)

// ======== TEMPO / AMOSTRAGEM ========
const uint32_t UI_TICK_MS     = 200;   // só para ritmo de UI
const uint32_t LOG_PERIOD_MS  = 5000;  // grava a cada 5 s
uint32_t lastUITick     = 0;
uint32_t lastLogMillis  = 0;
uint32_t chronoStartMillis = 0;
uint32_t chronoPausedAccum = 0;

// ======== FILTRO ========
float alpha = 0.2f;
bool emaInit = false;
float vFilt = 0.0f;      // volts
float iFilt_mA = 0.0f;   // mA

// ======== INTEGRAÇÕES (x10) ========
uint32_t carga_mAh_x10   = 0;
uint32_t energia_mWh_x10 = 0;
uint32_t lastCalcMillis  = 0;

// ======== LAYOUT (dinâmico) ========
int16_t W = 0, H = 0;          // do driver após rotação
const int16_t TOP_BAR_H = 18;  // barra ícones
const int16_t BOX_H     = 22;  // altura das caixas
const int16_t BOX_GAP   = 2;   // faixa BG entre caixas

struct Box { int16_t y,h; uint16_t c; };
Box boxes[6];

// ======== AJUSTE DE COR SÓ PARA O ÍCONE DA BATERIA ========
#define BAT_SWAP_RB 1
static inline uint16_t batColor(uint8_t r, uint8_t g, uint8_t b) {
#if BAT_SWAP_RB
  return tft.color565(b, g, r);  // swap R<->B
#else
  return tft.color565(r, g, b);
#endif
}
static inline uint16_t BAT_RED()    { return batColor(255, 0,   0);   }
static inline uint16_t BAT_YELLOW() { return batColor(255, 210, 0);   }
static inline uint16_t BAT_GREEN()  { return batColor(0,   200, 0);   }
//static inline uint16_t BAT_RED()    { return batColor(0,   0,   255); } // Blue hex (vermelho visual)
//static inline uint16_t BAT_YELLOW() { return batColor(0,   255, 255); } // Cyan hex (amarelo visual)
//static inline uint16_t BAT_GREEN()  { return batColor(0,   255, 0);   } // Green (assumido correto)

// =================== HELPERS ===================

static inline int16_t clampi(int16_t v, int16_t lo, int16_t hi) {
  if (v < lo) return lo;
  if (v > hi) return hi;
  return v;
}

// valor *10 inteiro (clamp >=0)
static inline int16_t toFixed1i(float x) {
  if (x <= 0) return 0;
  long v = (long)(x * 10.0f + 0.5f);
  if (v > 32767) v = 32767;
  return (int16_t)v;
}

// HH:MM:SS
void toHHMMSS(uint32_t secs, char out[9]) {
  uint16_t h = secs / 3600UL;
  uint8_t  m = (secs % 3600UL) / 60UL;
  uint8_t  s = secs % 60UL;
  out[0] = '0' + ((h/10)%10); out[1] = '0' + (h%10);
  out[2] = ':'; out[3] = '0' + (m/10); out[4] = '0' + (m%10);
  out[5] = ':'; out[6] = '0' + (s/10); out[7] = '0' + (s%10);
  out[8] = 0;
}

// Strings fixas (sem unidade)
void buildVVal(int16_t V10, char* out) {
  uint16_t intp = (uint16_t)(V10 / 10);
  uint8_t  dec  = (uint8_t)(V10 % 10);
  if (intp > 99) intp = 99;
  out[0] = '0' + (intp/10);
  out[1] = '0' + (intp%10);
  out[2] = '.';
  out[3] = '0' + dec;
  out[4] = 0;
}

void buildMMMMVal(int16_t x10, char* out) {
  uint16_t intp = (uint16_t)(x10 / 10);
  uint8_t  dec  = (uint8_t)(x10 % 10);
  if (intp > 9999) intp = 9999;
  uint8_t th = intp/1000;
  uint8_t hu = (intp/100)%10;
  uint8_t te = (intp/10)%10;
  uint8_t on = intp%10;
  out[0]='0'+th; out[1]='0'+hu; out[2]='0'+te; out[3]='0'+on; out[4]='.'; out[5]='0'+dec; out[6]=0;
}

// ISR botão (debounce reforçado)
void isrButton() {
  static uint32_t last = 0;
  uint32_t now = millis();
  if ((now - last) > 400) { // Aumentado para 400ms
    btnPressed = true;
    Serial.println(F("Botão pressionado detectado")); // Debug
  }
  last = now;
}

// =================== LAYOUT / UI ===================

void layoutInit() {
  int16_t y = TOP_BAR_H;
  uint16_t colors[6] = { C_BOX1, C_BOX2, C_BOX3, C_BOX4, C_BOX5, C_BOX6 };
  for (uint8_t i=0;i<6;i++) {
    boxes[i] = { y, BOX_H, colors[i] };
    y += BOX_H + BOX_GAP;
  }
  tft.fillScreen(COL_BG);
}

// fillRect seguro + “estende” à esquerda (LEFT_EXTEND)
static inline void safeFillRectExtL(int16_t x, int16_t y, int16_t w, int16_t h, uint16_t col) {
  x -= LEFT_EXTEND; w += LEFT_EXTEND;
  if (w <= 0 || h <= 0) return;
  if (x < 0) { w += x; x = 0; }
  if (y < 0) { h += y; y = 0; }
  if (x >= W || y >= H) return;
  if (x + w > W) w = W - x;
  if (y + h > H) h = H - y;
  if (w > 0 && h > 0) tft.fillRect(x, y, w, h, col);
}

// fillRect com clamp, SEM extender (para ícones/outline)
static inline void safeFillRectNoExt(int16_t x, int16_t y, int16_t w, int16_t h, uint16_t col) {
  if (w <= 0 || h <= 0) return;
  if (x < 0) { w += x; x = 0; }
  if (y < 0) { h += y; y = 0; }
  if (x >= W || y >= H) return;
  if (x + w > W) w = W - x;
  if (y + h > H) h = H - y;
  if (w > 0 && h > 0) tft.fillRect(x, y, w, h, col);
}

void desenhaTopBar() {
  safeFillRectExtL(0, 0, W, TOP_BAR_H, COL_BG);
}

void desenhaUI() {
  tft.fillScreen(COL_BG);
  tft.setTextWrap(false);
  desenhaTopBar();

  for (uint8_t i=0;i<6;i++) {
    Box &b = boxes[i];
    safeFillRectExtL(0, b.y, W, b.h, b.c);
    if (i < 5) safeFillRectExtL(0, b.y + b.h, W, BOX_GAP, COL_BG);
  }
  Box &last = boxes[5];
  if (last.y + last.h < H) safeFillRectExtL(0, last.y + last.h, W, H - (last.y + last.h), COL_BG);
}

// imprime valor (direita) + unidade (direita)
void desenhaBoxValAligned(const Box& b, const char* val, const char* unit, uint8_t spacesBeforeVal) {
  safeFillRectExtL(0, b.y, W, b.h, b.c);
  if (b.y + b.h < H) safeFillRectExtL(0, b.y + b.h, W, BOX_GAP, COL_BG);

  tft.setTextWrap(false);
  tft.setTextColor(COL_TEXT);
  tft.setTextSize(2);

  const int16_t chW = 11;
  uint8_t lenVal=0;  while (val[lenVal]  && lenVal  < 16) lenVal++;
  uint8_t lenUnit=0; while (unit[lenUnit] && lenUnit < 16) lenUnit++;

  int16_t right = W - 6; if (right < 0) right = 0;

  int16_t unitEndX   = right;
  int16_t unitStartX = unitEndX - (int16_t)lenUnit * chW + 1;
  if (unitStartX < 0) unitStartX = 0;

  int16_t gapPx = (int16_t)spacesBeforeVal * chW;

  int16_t valEndX   = unitStartX - 1 - gapPx;
  int16_t valStartX = valEndX - (int16_t)lenVal * chW + 1;
  if (valStartX < 0) valStartX = 0;

  int16_t py = b.y + 4;
  py = clampi(py, 0, (int16_t)(H - 8));

  int16_t dx = LEFT_EXTEND;
  int16_t vsx = clampi(valStartX - dx, 0, (int16_t)(W - 6));
  int16_t usx = clampi(unitStartX - dx, 0, (int16_t)(W - 6));

  tft.setCursor(vsx, py); tft.print(val);
  tft.setCursor(usx, py); tft.print(unit);
}

// cronômetro centralizado
void desenhaBoxValCenter(const Box& b, const char* s) {
  safeFillRectExtL(0, b.y, W, b.h, b.c);
  if (b.y + b.h < H) safeFillRectExtL(0, b.y + b.h, W, BOX_GAP, COL_BG);

  tft.setTextWrap(false);
  tft.setTextColor(COL_TEXT);
  tft.setTextSize(2);

  const int16_t chW = 11;
  uint8_t len=0; while (s[len] && len<16) len++;
  int16_t textW = (int16_t)len * chW;
  int16_t px = (W - textW)/2; if (px < 0) px = 0;
  int16_t py = b.y + 4;        py = clampi(py, 0, (int16_t)(H - 8));
  int16_t dx = LEFT_EXTEND / 2;
  int16_t csx = clampi(px - dx, 0, (int16_t)(W - textW));

  tft.setCursor(csx, py); tft.print(s);
}

// =================== ÍCONES ===================

void desenhaIconeSD(bool &prevSdOK, bool forceRedraw=false) {
    uint16_t c = C_INFO;

    if (!sdOK) c = C_ERR;
    else if (estado == GRAVANDO) c = C_OK;
    else if (estado == PAUSA)    c = C_WARN;

    if (!forceRedraw && sdOK == prevSdOK && estado == prevEstado) return;  // Nada mudou

    safeFillRectNoExt(0, 0, 20, TOP_BAR_H, COL_BG);
    int x=2, y=2, w=14, h=14;
    safeFillRectNoExt(x, y, w, h, c);
    safeFillRectNoExt(x+2, y+2, 6, 3, COL_BG); // entalhe

    prevSdOK = sdOK;    // Atualiza estado anterior
    prevEstado = estado; // Atualiza estado anterior
}

// estado da bateria (anti flicker)
int8_t   lastBatFillW = -1;
uint16_t lastBatCol   = 0;

void desenhaIconeBateriaThrottled(float vbat, bool forceRedraw, int8_t &prevFillW, uint16_t &prevCol) {
    float pct = (vbat - 3.0f) / 2.0f;
    if (pct < 0) pct = 0;
    if (pct > 1) pct = 1;

    uint16_t col = BAT_RED();
    if (pct >= 0.80f)      col = BAT_GREEN();
    else if (pct > 0.40f)  col = BAT_YELLOW();

    const int x = W - 24;
    const int y = 2;
    const int innerW = 18, innerH = 10;
    const int innerX = x + 1, innerY = y + 1;

    int fillW = (pct >= 0.999f) ? innerW : (int)(pct * innerW + 0.5f);
    if (fillW < 0)       fillW = 0;
    if (fillW > innerW)  fillW = innerW;

    if (!forceRedraw && fillW == prevFillW && col == prevCol) return;

    safeFillRectNoExt(W - 28, 0, 28, TOP_BAR_H, COL_BG);
    safeFillRectNoExt(innerX, innerY, innerW, innerH, COL_BG);
    if (fillW > 0) {
        safeFillRectNoExt(innerX, innerY, fillW, innerH, col);
        if (fillW >= innerW - 1) safeFillRectNoExt(innerX + innerW - 1, innerY, 1, innerH, col);
    }
    tft.drawRect(x, y, 20, 12, COL_TEXT);
    safeFillRectNoExt(x+21, y+3, 2, 6, COL_TEXT);

    prevFillW = fillW;
    prevCol   = col;
}

// =================== MEDIÇÕES ===================

float leituraBateria_V() {
  int raw = analogRead(BATERIA_PIN);
  float v = (raw * 5.0f) / 1023.0f;
  if (v < 0)    v = 0;
  if (v > 5.2f) v = 5.2f;
  return v;
}

void atualizaMedidas(float& V, float& mA, float& mW, float& vbat) {
  vbat = leituraBateria_V();

  float vb=0, im=0;
  if (ina.isConnected()) {
    vb = ina.getBusVoltage();
    im = ina.getShuntVoltage_mV() / 0.1;
    if (vb < 0) vb=0;
    if (im < 0) im=0;
  }

  if (!emaInit) { vFilt = vb; iFilt_mA = im; emaInit = true; }
  else {
    vFilt    = alpha*vb + (1.0f-alpha)*vFilt;
    iFilt_mA = alpha*im + (1.0f-alpha)*iFilt_mA;
  }

  V  = vFilt;
  mA = iFilt_mA;
  mW = V * mA;
}

// =================== VALORES NA TELA ===================

void desenhaValoresTela(float V, float mA, float mW,
                        uint32_t mAh_x10, uint32_t mWh_x10,
                        uint32_t secs) {
  int16_t V10 = toFixed1i(V);
  int16_t I10 = toFixed1i(mA);
  int16_t P10 = toFixed1i(mW);
  uint32_t Q10 = (mAh_x10>99999)?99999:mAh_x10;
  uint32_t E10 = (mWh_x10>99999)?99999:mWh_x10;

  if (estado == STANDBY) { Q10=0; E10=0; }

  // Atualizar V
  if (V10 != prevV10) { buildVVal(V10, vVal); desenhaBoxValAligned(boxes[0], vVal, "V", 3); prevV10 = V10; }

  // Atualizar I
  if (I10 != prevI10) { buildMMMMVal(I10, iVal); desenhaBoxValAligned(boxes[1], iVal, "mA", 2); prevI10 = I10; }

  // Atualizar P
  if (P10 != prevP10) { buildMMMMVal(P10, pVal); desenhaBoxValAligned(boxes[2], pVal, "mW", 2); prevP10 = P10; }

  // Atualizar Q
  if (Q10 != prevQ10) { buildMMMMVal((int16_t)Q10, qVal); desenhaBoxValAligned(boxes[3], qVal, "mAh", 1); prevQ10 = Q10; }

  // Atualizar E
  if (E10 != prevE10) { 
      char buf[6]; sprintf(buf, "%05lu", E10);
      desenhaBoxValAligned(boxes[4], buf, "mWh", 1); 
      prevE10 = E10; 
  }

  // Atualizar cronômetro
  if (secs != prevSecs) { toHHMMSS(secs, sT); desenhaBoxValCenter(boxes[5], sT); prevSecs = secs; }
}

// =================== CSV / LOG ===================

void printFixed1(Print &p, int16_t v10) {
  uint16_t u = (v10<0?0:v10);
  uint16_t intp = u/10; uint8_t dec = u%10;
  char tmp[6]; uint8_t t=0;
  do { tmp[t++] = '0' + (intp % 10); intp/=10; } while (intp>0);
  while (t--) p.write(tmp[t]);
  p.write(','); p.write('0'+dec);
}

// "LogMeter%03d.csv" sem sprintf
void nextLogFilename(char* name /* >=16 bytes */) {
  for (uint16_t i = 1; i <= 999; i++) {
    name[0] = '/';
    name[1] = 'l';
    name[2] = 'o';
    name[3] = 'g';
    name[4] = '0' + (i / 1000) % 10;
    name[5] = '0' + (i / 100) % 10;
    name[6] = '0' + (i / 10) % 10;
    name[7] = '0' + (i % 10);
    name[8] = '.';
    name[9] = 'c';
    name[10] = 's';
    name[11] = 'v';
    name[12] = '\0';
    if (!SD.exists(name)) return;
  }
  name[0] = '/';
  name[1] = 'l';
  name[2] = 'o';
  name[3] = 'g';
  name[4] = '0';
  name[5] = '9';
  name[6] = '9';
  name[7] = '9';
  name[8] = '.';
  name[9] = 'c';
  name[10] = 's';
  name[11] = 'v';
  name[12] = '\0';
}


void writeCSV(uint16_t idx, const char* hhmmss,
              int16_t V10, int16_t I10, int16_t P10,
              uint32_t Q10, uint32_t E10) {
  if (!logFile) return;
  SD_SELECT_SAFE();
  logFile.print(idx); logFile.print(';');
  logFile.print(hhmmss); logFile.print(';');
  printFixed1(logFile, V10); logFile.print(';');
  printFixed1(logFile, I10); logFile.print(';');
  printFixed1(logFile, P10); logFile.print(';');
  printFixed1(logFile, (Q10>32767)?32767:(int16_t)Q10); logFile.print(';');
  uint32_t Eint = (E10>99999)?99999:E10;
  char buf[6]; sprintf(buf, "%05lu", Eint);
  logFile.print(buf);
  logFile.println();
  SD_SELECT_SAFE();
  logFile.flush();
}

void startLogging() {
  SD_SELECT_SAFE();
  if (!sdOK) {
    sdOK = SD.begin(SD_CS);
    if (!sdOK) {
      Serial.println(F("Falha ao inicializar SD!"));
      desenhaIconeSD(prevSdOK, true); // Força redraw do ícone vermelho
      return;
    }
  }

  if (logFile) {
    SD_SELECT_SAFE();
    logFile.flush();
    SD_SELECT_SAFE();
    logFile.close();
  }

  char fname[16];
  nextLogFilename(fname);
  Serial.print(F("Tentando abrir arquivo: "));
  Serial.println(fname); // Debug: Confirmar nome do arquivo

  SD_SELECT_SAFE();
  logFile = SD.open(fname, FILE_WRITE);
  if (!logFile) {
    sdOK = false;
    Serial.println(F("Falha ao abrir arquivo CSV!"));
    desenhaIconeSD(prevSdOK, true); // Força redraw do ícone vermelho
    return;
  }

  SD_SELECT_SAFE();
  logFile.println(F("index;hh:mm:ss;Voltage(V);Current(mA);Power(mW);Charge(mAh);Energy(mWh)"));
  logFile.flush();

  sampleIndex = 0;
  lastLogMillis = millis();
  desenhaIconeSD(prevSdOK, true); // Força redraw para refletir estado
}

void stopLogging() {
  if (logFile) {
    SD_SELECT_SAFE(); logFile.flush();
    SD_SELECT_SAFE(); logFile.close();
  }
}

// =================== ESTADOS ===================

uint32_t getElapsedSecs() {
  return (estado==GRAVANDO)
      ? (chronoPausedAccum + (millis() - chronoStartMillis))/1000UL
      : (chronoPausedAccum/1000UL);
}

void mudaEstado() {
  Serial.print(F("Mudando estado de "));
  Serial.println(estado); // Debug

  if (!sdOK && (estado == STANDBY || estado == PAUSA)) {
    estado = (estado == STANDBY) ? PAUSA : STANDBY;
    Serial.println(F("SD não inicializado, alternando entre STANDBY/PAUSA"));
  } else {
    if (estado == STANDBY) {
      estado = GRAVANDO;
      chronoStartMillis = lastCalcMillis = millis();
      chronoPausedAccum = 0;
      sampleIndex = 0;
      carga_mAh_x10 = 0;
      energia_mWh_x10 = 0;
      startLogging();
      Serial.println(F("Transição para GRAVANDO"));
    } else if (estado == GRAVANDO) {
      estado = PAUSA;
      chronoPausedAccum += (millis() - chronoStartMillis);
      stopLogging();
      Serial.println(F("Transição para PAUSA"));
    } else { // PAUSA -> STANDBY
      estado = STANDBY;
      sampleIndex = 0;
      carga_mAh_x10 = 0;
      energia_mWh_x10 = 0;
      chronoStartMillis = 0;
      chronoPausedAccum = 0;
      stopLogging();
      Serial.println(F("Transição para STANDBY"));
    }
  }

  // Força redraw dos ícones no cambio de estado
  safeFillRectNoExt(0, 0, 20, TOP_BAR_H, COL_BG); // Limpa área do SD
  safeFillRectNoExt(W - 28, 0, 28, TOP_BAR_H, COL_BG); // Limpa área da bateria
  desenhaIconeSD(prevSdOK, true);
  float vbat = leituraBateria_V();
  desenhaIconeBateriaThrottled(vbat, true, prevBatFillWIcon, prevBatColIcon);
  desenhaValoresTela(vFilt, iFilt_mA, vFilt * iFilt_mA, carga_mAh_x10, energia_mWh_x10, getElapsedSecs());
}

// =================== FUNÇÃO TOP BAR FIXA ===================

void desenhaTopBarFix() {
    // Verifica se os ícones precisam ser redesenhados
    float vbat = leituraBateria_V();
    float pct = (vbat - 3.0f) / 2.0f;
    if (pct < 0) pct = 0;
    if (pct > 1) pct = 1;
    uint16_t col = BAT_RED();
    if (pct >= 0.80f) col = BAT_GREEN();
    else if (pct > 0.40f) col = BAT_YELLOW();
    int fillW = (pct >= 0.999f) ? 18 : (int)(pct * 18 + 0.5f);
    if (fillW < 0) fillW = 0;
    if (fillW > 18) fillW = 18;

    bool sdNeedsRedraw = (sdOK != prevSdOK || estado != prevEstado);
    bool batNeedsRedraw = (fillW != prevBatFillWIcon || col != prevBatColIcon);

    // Limpa apenas as áreas dos ícones que precisam ser redesenhados
    if (sdNeedsRedraw) {
        safeFillRectNoExt(0, 0, 20, TOP_BAR_H, COL_BG); // Área do SD
    }
    if (batNeedsRedraw) {
        safeFillRectNoExt(W - 28, 0, 28, TOP_BAR_H, COL_BG); // Área da bateria
    }

    // Desenha ícones
    desenhaIconeSD(prevSdOK, sdNeedsRedraw); // Força redraw apenas se necessário
    desenhaIconeBateriaThrottled(vbat, batNeedsRedraw, prevBatFillWIcon, prevBatColIcon); // Força redraw apenas se necessário
}

// =================== SETUP / LOOP ===================

void setup() {
  Serial.begin(115200);

  // Configura pinos SPI e botão
  pinMode(TFT_CS, OUTPUT);
  pinMode(SD_CS, OUTPUT);
  digitalWrite(TFT_CS, HIGH);
  digitalWrite(SD_CS, HIGH);

  pinMode(BOTAO_PIN, INPUT_PULLUP);
  attachInterrupt(digitalPinToInterrupt(BOTAO_PIN), isrButton, FALLING);

  // Inicializa TFT
  tft.initR(PANEL_TAB);
  tft.setRotation(0);
  W = tft.width();
  H = tft.height();
  tft.fillScreen(COL_BG);
  tft.setTextWrap(false);

  // Inicializa I2C INA226
  Wire.begin();
  ina.begin();
  if (ina.isConnected()) {
    ina.setMaxCurrentShunt(I_MAX_A, R_SHUNT_OHMS);
  }

  // Inicializa SD
  sdOK = SD.begin(SD_CS);
  if (!sdOK) Serial.println(F("SD falhou!"));
  prevSdOK = false;  // Força redesenho

  // Layout e UI
  layoutInit();
  desenhaUI();

  // Desenha ícones iniciais
  safeFillRectNoExt(0, 0, 20, TOP_BAR_H, COL_BG); // Limpa área do SD
  safeFillRectNoExt(W - 28, 0, 28, TOP_BAR_H, COL_BG); // Limpa área da bateria
  desenhaIconeSD(prevSdOK, true);
  float vbat = leituraBateria_V();
  desenhaIconeBateriaThrottled(vbat, true, prevBatFillWIcon, prevBatColIcon);

  // Inicializa variáveis de controle
  lastBatFillW = -1;
  lastBatCol = 0;
  lastCalcMillis = lastUITick = lastLogMillis = millis();
}

void loop() {
  if (btnPressed) {
    btnPressed = false;
    mudaEstado();
  }

  uint32_t now = millis();

  // Atualiza leituras
  float V = 0, mA = 0, mW = 0, vbat = 0;
  atualizaMedidas(V, mA, mW, vbat);

  // Gravação a cada LOG_PERIOD_MS (5s)
  if (estado == GRAVANDO && (now - lastLogMillis >= LOG_PERIOD_MS)) {
    float dt_h = (now - lastLogMillis) / 3600000.0f; // Usar intervalo real
    carga_mAh_x10 += (uint32_t)(iFilt_mA * dt_h * 10.0f + 0.5f);
    energia_mWh_x10 += (uint32_t)(vFilt * iFilt_mA * dt_h * 10.0f + 0.5f);

    // Grava CSV
    if (sdOK && logFile) {
      char hhmmss[9];
      toHHMMSS(getElapsedSecs(), hhmmss);
      sampleIndex++;
      writeCSV(sampleIndex, hhmmss,
               toFixed1i(vFilt),
               toFixed1i(iFilt_mA),
               toFixed1i(vFilt * iFilt_mA),
               carga_mAh_x10,
               energia_mWh_x10);
      SD_SELECT_SAFE();
      logFile.flush();
      Serial.println(F("Dados gravados no CSV")); // Debug
    }

    lastLogMillis = now;
  }

  // Atualização UI (anti-flicker)
  if (now - lastUITick >= UI_TICK_MS) {
    lastUITick = now;

    // Atualiza valores dos boxes
    desenhaValoresTela(V, mA, mW, carga_mAh_x10, energia_mWh_x10, getElapsedSecs());

    // Top bar com ícones
    desenhaTopBarFix();
  } else {
    // Atualiza apenas ícone de bateria parcial, se necessário
    float vbat = leituraBateria_V();
    desenhaIconeBateriaThrottled(vbat, false, prevBatFillWIcon, prevBatColIcon);
  }
}