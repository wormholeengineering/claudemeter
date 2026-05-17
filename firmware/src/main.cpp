/*
 * ClaudeMeter — ESP32 C3 Super Mini + ST7735 1.8" (128×160 portrait)
 * Exibe uso da API Claude: sessão 5h, semanal, opus e saldo extra.
 * Dados via proxy HTTP local — ver /proxy/server.py
 *
 * Pinos: mesmos do DataLogger de referência
 *   TFT_CS=21  TFT_DC=1  TFT_RST=3  (SPI padrão)
 */

#include <Arduino.h>
#include <SPI.h>
#include <Adafruit_GFX.h>
#include <Adafruit_ST7735.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>

// ───────────────────────── CONFIGURAÇÃO ──────────────────────────
#define WIFI_SSID    "SuaRedeWiFi"
#define WIFI_PASS    "SuaSenha"
#define PROXY_URL    "http://192.168.1.100:8000/usage"
#define REFRESH_MS   60000UL     // atualiza a cada 60 s

// ───────────────────────── PINOS TFT ─────────────────────────────
#define TFT_CS   21
#define TFT_DC    1
#define TFT_RST   3

Adafruit_ST7735 tft(TFT_CS, TFT_DC, TFT_RST);
static int16_t W, H;

// ───────────────────────── PALETA ────────────────────────────────
// Macro RGB→RGB565 (sem swap B/R — ajustar se as cores saírem trocadas)
#define RGB565(r,g,b) ((uint16_t)((((r)&0xF8)<<8)|(((g)&0xFC)<<3)|((b)>>3)))

#define C_BG       RGB565(  8,  10,  22)   // fundo geral (azul noite)
#define C_TEXT     RGB565(220, 225, 255)   // branco frio
#define C_DIM      RGB565( 75,  80, 115)   // cinza azulado (rótulos)

#define C_S_BOX    RGB565( 12,  28,  80)   // sessão → azul
#define C_W_BOX    RGB565( 45,  12,  72)   // semanal → roxo
#define C_O_BOX    RGB565(  8,  55,  65)   // opus    → teal
#define C_B_BOX    RGB565(  8,  46,  18)   // saldo   → verde escuro

#define C_BAR_BG   RGB565( 18,  18,  36)   // trilho da barra
#define C_GREEN    RGB565( 38, 210,  78)   // ≤65%
#define C_YELLOW   RGB565(220, 195,   0)   // 65-85%
#define C_RED      RGB565(210,  38,  38)   // >85%

#define C_WIFI_ON  RGB565( 38, 210,  78)
#define C_WIFI_OFF RGB565(210,  38,  38)
#define C_DOT_LO   RGB565( 35,  38,  70)   // dot apagado

// ───────────────────────── LAYOUT (pixels) ───────────────────────
// 18 + 2 + 38 + 2 + 38 + 2 + 30 + 2 + 26 + 2 = 160 ✓
const int16_t TOP_Y  =   0, TOP_H  = 18;
const int16_t GAP              = 2;
const int16_t BOX_S_Y = 20,   BOX_S_H = 38;
const int16_t BOX_W_Y = 60,   BOX_W_H = 38;
const int16_t BOX_O_Y = 100,  BOX_O_H = 30;
const int16_t BOX_B_Y = 132,  BOX_B_H = 26;

// ───────────────────────── DADOS ─────────────────────────────────
struct ClaudeUsage {
    float session_pct   = -1;
    float weekly_pct    = -1;
    float opus_pct      = -1;
    float extra_balance = -1;
    int   reset_secs    = -1;
    bool  valid         = false;
};

static ClaudeUsage g_data;
static bool     g_wifiOk   = false;
static uint32_t g_lastFetch = 0;
static uint32_t g_lastAnim  = 0;
static uint8_t  g_animTick  = 0;

// ═══════════════════════ HELPERS ═════════════════════════════════

static void fillBox(int16_t y, int16_t h, uint16_t col) {
    tft.fillRect(0, y, W, h, col);
}

static uint16_t pctColor(float pct) {
    if (pct < 0)    return C_DIM;
    if (pct < 65.f) return C_GREEN;
    if (pct < 85.f) return C_YELLOW;
    return C_RED;
}

// Barra de progresso com trilho arredondado visualmente
static void drawBar(int16_t bx, int16_t by, int16_t bw, int16_t bh,
                    float pct, uint16_t fillCol) {
    tft.fillRect(bx, by, bw, bh, C_BAR_BG);
    tft.drawRect(bx, by, bw, bh, C_DIM);
    if (pct >= 0 && pct <= 100) {
        int16_t fill = (int16_t)((pct / 100.f) * (bw - 2) + .5f);
        if (fill > 0)
            tft.fillRect(bx + 1, by + 1, fill, bh - 2, fillCol);
        // marcador de 65% (referência visual)
        int16_t m65 = (int16_t)(0.65f * (bw - 2));
        tft.drawFastVLine(bx + 1 + m65, by, bh, C_DIM);
    }
}

static void printRight(const char* s, int16_t y, uint16_t col, uint8_t sz = 1) {
    tft.setTextSize(sz);
    tft.setTextColor(col, 0); // transparent bg
    int16_t tw = (int16_t)(strlen(s) * 6 * sz);
    tft.setCursor(W - tw - 3, y);
    tft.print(s);
}

static void printCenter(const char* s, int16_t y, uint16_t col, uint8_t sz = 1) {
    tft.setTextSize(sz);
    tft.setTextColor(col, 0);
    int16_t tw = (int16_t)(strlen(s) * 6 * sz);
    tft.setCursor((W - tw) / 2, y);
    tft.print(s);
}

static void fmtReset(int secs, char* buf, size_t len) {
    if (secs < 0) { snprintf(buf, len, "--"); return; }
    int h = secs / 3600, m = (secs % 3600) / 60;
    if (h > 0) snprintf(buf, len, "%dh%02dm", h, m);
    else        snprintf(buf, len, "%dm", m);
}

// ═══════════════════════ DESENHO ═════════════════════════════════

static void drawGaps() {
    tft.fillRect(0, TOP_Y  + TOP_H,    W, GAP, C_BG);
    tft.fillRect(0, BOX_S_Y + BOX_S_H, W, GAP, C_BG);
    tft.fillRect(0, BOX_W_Y + BOX_W_H, W, GAP, C_BG);
    tft.fillRect(0, BOX_O_Y + BOX_O_H, W, GAP, C_BG);
    // rodapé (2px restantes)
    tft.fillRect(0, BOX_B_Y + BOX_B_H, W, H - (BOX_B_Y + BOX_B_H), C_BG);
}

static void drawTopBar() {
    fillBox(TOP_Y, TOP_H, C_BG);

    // Ícone WiFi (3 barrinhas verticais)
    uint16_t wc = g_wifiOk ? C_WIFI_ON : C_WIFI_OFF;
    tft.fillRect(2,  13, 2, 3, wc);
    tft.fillRect(5,  10, 2, 6, wc);
    tft.fillRect(8,   7, 2, 9, wc);

    // Título
    printCenter("CLAUDE METER", 5, C_TEXT, 1);

    // Ponto de status (pisca no loop)
    tft.fillCircle(W - 5, 9, 3, C_DOT_LO);
}

// Seção de métrica com barra de progresso
static void drawMetricBox(int16_t by, int16_t bh, uint16_t boxCol,
                          const char* label, const char* tag,
                          float pct, int resetSecs) {
    fillBox(by, bh, boxCol);

    // Rótulo (esquerda, top)
    tft.setTextSize(1);
    tft.setTextColor(C_TEXT, 0);
    tft.setCursor(4, by + 3);
    tft.print(label);

    // Tag (direita, top) — ex: "5h" ou "7d"
    if (tag && tag[0])
        printRight(tag, by + 3, C_DIM, 1);

    // Porcentagem
    char pBuf[8];
    uint16_t pc = pctColor(pct);
    if (pct < 0) strcpy(pBuf, "--");
    else         snprintf(pBuf, sizeof(pBuf), "%d%%", (int)(pct + .5f));

    // Barra de progresso
    int16_t barY = by + 14;
    drawBar(3, barY, W - 6, 8, pct, pc);

    // % sobreposto à direita da barra (fundo igual à caixa para não vazar)
    tft.setTextSize(1);
    tft.setTextColor(pc, boxCol);
    int16_t tw = (int16_t)(strlen(pBuf) * 6);
    tft.setCursor(W - tw - 5, barY + 1);
    tft.print(pBuf);

    // Tempo de reset no rodapé (só se cabe)
    if (bh >= 36) {
        char tbuf[10];
        fmtReset(resetSecs, tbuf, sizeof(tbuf));
        char line[20];
        snprintf(line, sizeof(line), "reset: %s", tbuf);
        tft.setTextSize(1);
        tft.setTextColor(C_DIM, 0);
        tft.setCursor(4, by + bh - 10);
        tft.print(line);
    }
}

static void drawBalanceBox(float bal) {
    fillBox(BOX_B_Y, BOX_B_H, C_B_BOX);

    tft.setTextSize(1);
    tft.setTextColor(C_DIM, 0);
    tft.setCursor(4, BOX_B_Y + 2);
    tft.print("SALDO EXTRA");

    char buf[14];
    uint16_t bc;
    if      (bal < 0)   { strcpy(buf, "N/D"); bc = C_DIM;    }
    else if (bal < 2.f) { snprintf(buf, sizeof(buf), "$ %.2f", bal); bc = C_YELLOW; }
    else                { snprintf(buf, sizeof(buf), "$ %.2f", bal); bc = C_GREEN;  }

    printCenter(buf, BOX_B_Y + 11, bc, 2);
}

// Tela de splash inicial
static void drawSplash() {
    tft.fillScreen(C_BG);
    // Logo: 2 palavras centralizadas com cor diferente
    printCenter("CLAUDE", 54, C_TEXT, 2);
    printCenter("METER",  70, RGB565(90, 110, 255), 2);
    tft.drawFastHLine(22, 90, 84, RGB565(60, 70, 180));
    printCenter("conectando...", 106, C_DIM, 1);
}

// Redesenha toda a UI com dados atuais
static void drawAll(bool withGaps = false) {
    if (withGaps) drawGaps();
    drawTopBar();
    drawMetricBox(BOX_S_Y, BOX_S_H, C_S_BOX,
                  "SESSION", "5h",
                  g_data.session_pct, g_data.reset_secs);
    drawMetricBox(BOX_W_Y, BOX_W_H, C_W_BOX,
                  "SEMANAL", "7d",
                  g_data.weekly_pct, -1);
    drawMetricBox(BOX_O_Y, BOX_O_H, C_O_BOX,
                  "OPUS", nullptr,
                  g_data.opus_pct, -1);
    drawBalanceBox(g_data.extra_balance);
}

// ═══════════════════════ WIFI ════════════════════════════════════

static void connectWifi() {
    WiFi.mode(WIFI_STA);
    WiFi.begin(WIFI_SSID, WIFI_PASS);
    for (int i = 0; i < 50 && WiFi.status() != WL_CONNECTED; i++)
        delay(200);
    g_wifiOk = (WiFi.status() == WL_CONNECTED);
}

// ═══════════════════════ HTTP FETCH ══════════════════════════════

static bool fetchUsage() {
    if (WiFi.status() != WL_CONNECTED) { g_wifiOk = false; return false; }
    g_wifiOk = true;

    HTTPClient http;
    http.begin(PROXY_URL);
    http.setTimeout(8000);
    int code = http.GET();
    if (code != 200) { http.end(); return false; }

    String body = http.getString();
    http.end();

    JsonDocument doc;
    if (deserializeJson(doc, body) != DeserializationError::Ok) return false;

    g_data.session_pct   = doc["session_pct"]       | -1.f;
    g_data.weekly_pct    = doc["weekly_pct"]        | -1.f;
    g_data.opus_pct      = doc["opus_pct"]          | -1.f;
    g_data.extra_balance = doc["extra_balance"]     | -1.f;
    g_data.reset_secs    = doc["tokens_reset_secs"] | -1;
    g_data.valid = true;
    return true;
}

// ═══════════════════════ SETUP / LOOP ════════════════════════════

void setup() {
    Serial.begin(115200);

    pinMode(TFT_CS, OUTPUT);
    digitalWrite(TFT_CS, HIGH);

    tft.initR(INITR_GREENTAB);
    tft.setRotation(0);
    W = tft.width();
    H = tft.height();
    tft.fillScreen(C_BG);
    tft.setTextWrap(false);

    drawSplash();
    connectWifi();

    drawAll(true);
    fetchUsage();
    drawAll(false);   // redesenha sem apagar os gaps

    g_lastFetch = millis();
    g_lastAnim  = millis();
}

void loop() {
    uint32_t now = millis();

    // Reconecta WiFi se cair
    if (WiFi.status() != WL_CONNECTED) {
        bool wasOk = g_wifiOk;
        g_wifiOk = false;
        connectWifi();
        if (wasOk != g_wifiOk) drawTopBar();
    }

    // Refresh periódico
    if (now - g_lastFetch >= REFRESH_MS) {
        g_lastFetch = now;
        tft.fillCircle(W - 5, 9, 3, C_GREEN);   // pisca verde ao buscar
        fetchUsage();
        drawAll(false);
    }

    // Pulso do ponto de status (alterna a cada 1 s)
    if (now - g_lastAnim >= 1000) {
        g_lastAnim = now;
        g_animTick ^= 1;
        uint16_t dc = g_animTick
            ? (g_wifiOk ? RGB565(25, 120, 45) : RGB565(120, 25, 25))
            : C_DOT_LO;
        tft.fillCircle(W - 5, 9, 3, dc);
    }

    delay(100);
}
