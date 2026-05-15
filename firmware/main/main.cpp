/*
 * ClaudeMeter — ESP-IDF 5.x + LVGL v9
 * ESP32-C3 Super Mini + ST7735 1.8" SPI (128×160 portrait)
 *
 * Exibe uso da API Claude: sessão 5h, semanal, opus, saldo extra.
 * Dados via proxy HTTP local (ver /proxy/server.py).
 *
 * Build:
 *   idf.py set-target esp32c3
 *   idf.py build flash monitor
 */

#include <cstdio>
#include <cstring>
#include <cmath>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "freertos/event_groups.h"

#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "nvs_flash.h"
#include "esp_http_client.h"
#include "esp_timer.h"
#include "driver/spi_master.h"
#include "driver/gpio.h"
#include "cJSON.h"

#include "lvgl.h"
#include "esp_lvgl_port.h"

// ─── CONFIGURAÇÃO ──────────────────────────────────────────────────
#define WIFI_SSID      "BRARUS_IoT"
#define WIFI_PASS      "L4c3rd4S4r4c*"
#define PROXY_URL      "http://192.168.68.108:8000/usage"
#define REFRESH_SEC    60

// ─── PINOS ─────────────────────────────────────────────────────────
// ESP32-C3 Super Mini — confirme com seu hardware
#define GPIO_MOSI  6    // SPI MOSI
#define GPIO_SCLK  4    // SPI CLK
#define GPIO_CS   21    // TFT CS
#define GPIO_DC    1    // TFT DC (data/command)
#define GPIO_RST   3    // TFT RST

// ─── DISPLAY ────────────────────────────────────────────────────────
#define LCD_HOST        SPI2_HOST
#define LCD_FREQ_HZ     (26 * 1000 * 1000)   // 26 MHz — seguro para ST7735
#define LCD_W           128
#define LCD_H           160
// Offsets do GREENTAB para conversão de coordenadas
#define ST7735_X_OFFSET  2
#define ST7735_Y_OFFSET  1

static const char *TAG = "claudemeter";

// ─── DADOS ──────────────────────────────────────────────────────────
typedef struct {
    float session_pct;
    float weekly_pct;
    float opus_pct;
    float extra_balance;
    int   reset_secs;
    bool  valid;
} claude_usage_t;

static claude_usage_t g_usage = {-1, -1, -1, -1, -1, false};
static SemaphoreHandle_t g_usage_lock;

// ─── WiFi ───────────────────────────────────────────────────────────
#define WIFI_CONNECTED_BIT BIT0
#define WIFI_FAIL_BIT      BIT1
static EventGroupHandle_t g_wifi_events;
static bool g_wifi_ok = false;

// ─── LVGL globals ───────────────────────────────────────────────────
static lv_display_t  *g_disp  = nullptr;
static spi_device_handle_t g_spi;

// ─── Widgets ────────────────────────────────────────────────────────
static lv_obj_t *g_bar_s, *g_bar_w, *g_bar_o;
static lv_obj_t *g_lbl_s_pct, *g_lbl_w_pct, *g_lbl_o_pct;
static lv_obj_t *g_lbl_s_reset;
static lv_obj_t *g_lbl_balance;
static lv_obj_t *g_lbl_status;
static lv_obj_t *g_dot_wifi;

// ════════════════════ ST7735 DRIVER ════════════════════════════════

// Callback: seta pino DC antes de cada transação SPI
// t->user = 0 → comando, 1 → dados
static void IRAM_ATTR spi_pre_transfer(spi_transaction_t *t) {
    gpio_set_level((gpio_num_t)GPIO_DC, (int)(intptr_t)t->user);
}

static void st7735_cmd(uint8_t cmd) {
    spi_transaction_t t = {};
    t.length    = 8;
    t.tx_data[0] = cmd;
    t.user       = (void *)0;   // DC = 0 (comando)
    t.flags      = SPI_TRANS_USE_TXDATA;
    spi_device_polling_transmit(g_spi, &t);
}

static void st7735_data(const uint8_t *data, size_t len) {
    if (len == 0) return;
    spi_transaction_t t = {};
    t.length    = len * 8;
    t.tx_buffer = data;
    t.user       = (void *)1;   // DC = 1 (dados)
    spi_device_polling_transmit(g_spi, &t);
}

static void st7735_data1(uint8_t d) { st7735_data(&d, 1); }

static void st7735_set_window(int x0, int y0, int x1, int y1) {
    st7735_cmd(0x2A);  // CASET
    uint8_t cx[4] = { 0, (uint8_t)x0, 0, (uint8_t)x1 };
    st7735_data(cx, 4);

    st7735_cmd(0x2B);  // RASET
    uint8_t cy[4] = { 0, (uint8_t)y0, 0, (uint8_t)y1 };
    st7735_data(cy, 4);

    st7735_cmd(0x2C);  // RAMWR
}

static void st7735_init() {
    // Reset por hardware
    gpio_set_level((gpio_num_t)GPIO_RST, 0);
    vTaskDelay(pdMS_TO_TICKS(20));
    gpio_set_level((gpio_num_t)GPIO_RST, 1);
    vTaskDelay(pdMS_TO_TICKS(150));

    st7735_cmd(0x01);  vTaskDelay(pdMS_TO_TICKS(150)); // SWRESET
    st7735_cmd(0x11);  vTaskDelay(pdMS_TO_TICKS(255)); // SLPOUT

    // Frame rate
    st7735_cmd(0xB1); st7735_data1(0x01); st7735_data1(0x2C); st7735_data1(0x2D);
    st7735_cmd(0xB2); st7735_data1(0x01); st7735_data1(0x2C); st7735_data1(0x2D);
    st7735_cmd(0xB3);
    for (int i = 0; i < 6; i++) st7735_data1(i < 3 ? (i == 0 ? 0x01 : (i==1?0x2C:0x2D))
                                                     : (i == 3 ? 0x01 : (i==4?0x2C:0x2D)));

    st7735_cmd(0xB4); st7735_data1(0x07);  // INVCTR

    // Power control
    st7735_cmd(0xC0); st7735_data1(0xA2); st7735_data1(0x02); st7735_data1(0x84);
    st7735_cmd(0xC1); st7735_data1(0xC5);
    st7735_cmd(0xC2); st7735_data1(0x0A); st7735_data1(0x00);
    st7735_cmd(0xC3); st7735_data1(0x8A); st7735_data1(0x2A);
    st7735_cmd(0xC4); st7735_data1(0x8A); st7735_data1(0xEE);
    st7735_cmd(0xC5); st7735_data1(0x0E);  // VMCTR1

    st7735_cmd(0x20);  // INVOFF

    // Orientação retrato + ordem RGB
    st7735_cmd(0x36); st7735_data1(0x08);  // MADCTL (portrait, RGB)
    st7735_cmd(0x3A); st7735_data1(0x05);  // COLMOD 16-bit

    // Área GREENTAB: x offset=2, y offset=1
    st7735_cmd(0x2A);
    { uint8_t d[] = {0x00, ST7735_X_OFFSET, 0x00, LCD_W - 1 + ST7735_X_OFFSET}; st7735_data(d, 4); }
    st7735_cmd(0x2B);
    { uint8_t d[] = {0x00, ST7735_Y_OFFSET, 0x00, LCD_H - 1 + ST7735_Y_OFFSET}; st7735_data(d, 4); }

    // Gamma positivo
    st7735_cmd(0xE0);
    { uint8_t g[] = {0x0F,0x1A,0x0F,0x18,0x2F,0x28,0x20,0x22,
                     0x1F,0x1B,0x23,0x37,0x00,0x07,0x02,0x10};
      st7735_data(g, 16); }
    // Gamma negativo
    st7735_cmd(0xE1);
    { uint8_t g[] = {0x0F,0x1B,0x0F,0x17,0x33,0x2C,0x29,0x2E,
                     0x30,0x30,0x39,0x3F,0x00,0x07,0x03,0x10};
      st7735_data(g, 16); }

    st7735_cmd(0x13);  vTaskDelay(pdMS_TO_TICKS(10));  // NORON
    st7735_cmd(0x29);  vTaskDelay(pdMS_TO_TICKS(100)); // DISPON
}

// ════════════════════ LVGL FLUSH CALLBACK ══════════════════════════

static void lvgl_flush_cb(lv_display_t *disp, const lv_area_t *area, uint8_t *px_map) {
    // Aplica offsets do GREENTAB
    int x0 = area->x1 + ST7735_X_OFFSET;
    int y0 = area->y1 + ST7735_Y_OFFSET;
    int x1 = area->x2 + ST7735_X_OFFSET;
    int y1 = area->y2 + ST7735_Y_OFFSET;

    st7735_set_window(x0, y0, x1, y1);

    // Envia pixels (LVGL com LV_COLOR_16_SWAP já entrega bytes na ordem correta)
    size_t len = (size_t)(x1 - x0 + 1) * (size_t)(y1 - y0 + 1) * 2;
    spi_transaction_t t = {};
    t.length    = len * 8;
    t.tx_buffer = px_map;
    t.user       = (void *)1;   // DC = dados
    spi_device_polling_transmit(g_spi, &t);

    lv_display_flush_ready(disp);
}

// ════════════════════ WiFi ═════════════════════════════════════════

static void wifi_event_handler(void *arg, esp_event_base_t base,
                               int32_t id, void *data) {
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        g_wifi_ok = false;
        xEventGroupSetBits(g_wifi_events, WIFI_FAIL_BIT);
        ESP_LOGW(TAG, "WiFi desconectado — reconectando...");
        vTaskDelay(pdMS_TO_TICKS(3000));
        esp_wifi_connect();
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        g_wifi_ok = true;
        xEventGroupSetBits(g_wifi_events, WIFI_CONNECTED_BIT);
        ESP_LOGI(TAG, "WiFi conectado");
    }
}

static void wifi_init() {
    g_wifi_events = xEventGroupCreate();
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    esp_event_handler_instance_t h1, h2;
    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                                        &wifi_event_handler, nullptr, &h1));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                                        &wifi_event_handler, nullptr, &h2));

    wifi_config_t wcfg = {};
    strncpy((char*)wcfg.sta.ssid,     WIFI_SSID, sizeof(wcfg.sta.ssid) - 1);
    strncpy((char*)wcfg.sta.password, WIFI_PASS,  sizeof(wcfg.sta.password) - 1);

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wcfg));
    ESP_ERROR_CHECK(esp_wifi_start());
}

// ════════════════════ HTTP FETCH ═══════════════════════════════════

#define HTTP_BUF_SIZE 512
static char g_http_buf[HTTP_BUF_SIZE];
static int  g_http_len = 0;

static esp_err_t http_event_handler(esp_http_client_event_t *evt) {
    if (evt->event_id == HTTP_EVENT_ON_DATA) {
        int room = HTTP_BUF_SIZE - g_http_len - 1;
        if (room > 0) {
            int copy = evt->data_len < room ? evt->data_len : room;
            memcpy(g_http_buf + g_http_len, evt->data, copy);
            g_http_len += copy;
        }
    }
    return ESP_OK;
}

static bool fetch_usage() {
    g_http_len = 0;
    memset(g_http_buf, 0, sizeof(g_http_buf));

    esp_http_client_config_t cfg = {};
    cfg.url             = PROXY_URL;
    cfg.timeout_ms      = 8000;
    cfg.event_handler   = http_event_handler;

    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client) return false;

    esp_err_t err = esp_http_client_perform(client);
    int status    = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);

    if (err != ESP_OK || status != 200) {
        ESP_LOGW(TAG, "HTTP erro: %s (status %d)", esp_err_to_name(err), status);
        return false;
    }

    cJSON *root = cJSON_ParseWithLength(g_http_buf, g_http_len);
    if (!root) {
        ESP_LOGW(TAG, "JSON inválido");
        return false;
    }

    if (xSemaphoreTake(g_usage_lock, pdMS_TO_TICKS(500)) == pdTRUE) {
        auto f = [&](const char *k, float fallback) -> float {
            cJSON *item = cJSON_GetObjectItemCaseSensitive(root, k);
            return (item && cJSON_IsNumber(item)) ? (float)item->valuedouble : fallback;
        };
        g_usage.session_pct   = f("session_pct",   -1);
        g_usage.weekly_pct    = f("weekly_pct",    -1);
        g_usage.opus_pct      = f("opus_pct",      -1);
        g_usage.extra_balance = f("extra_balance", -1);
        g_usage.reset_secs    = (int)f("tokens_reset_secs", -1);
        g_usage.valid = true;
        xSemaphoreGive(g_usage_lock);
    }

    cJSON_Delete(root);
    ESP_LOGI(TAG, "Dados atualizados — session=%.1f%% weekly=%.1f%%",
             g_usage.session_pct, g_usage.weekly_pct);
    return true;
}

// ════════════════════ LVGL UI ══════════════════════════════════════

// Cor da barra conforme percentual
static lv_color_t bar_color(float pct) {
    if (pct < 0)    return lv_color_hex(0x4B5073);  // desconhecido
    if (pct < 65.f) return lv_color_hex(0x26D24E);  // verde
    if (pct < 85.f) return lv_color_hex(0xDCC800);  // amarelo
    return lv_color_hex(0xD22626);                   // vermelho
}

// Cria um painel (container) de métrica com barra LVGL
// Retorna: container obj; bar, lbl_pct, lbl_sub passados por ponteiro
static lv_obj_t *create_metric_panel(lv_obj_t *parent,
                                     int y, int h, lv_color_t bg,
                                     const char *label, const char *tag,
                                     lv_obj_t **out_bar,
                                     lv_obj_t **out_lbl_pct,
                                     lv_obj_t **out_lbl_sub) {
    // Container
    lv_obj_t *panel = lv_obj_create(parent);
    lv_obj_set_pos(panel, 0, y);
    lv_obj_set_size(panel, LCD_W, h);
    lv_obj_set_style_bg_color(panel, bg, 0);
    lv_obj_set_style_bg_opa(panel, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(panel, 0, 0);
    lv_obj_set_style_pad_all(panel, 0, 0);
    lv_obj_set_style_radius(panel, 0, 0);

    // Rótulo principal (esquerda)
    lv_obj_t *lbl = lv_label_create(panel);
    lv_label_set_text(lbl, label);
    lv_obj_set_style_text_color(lbl, lv_color_hex(0xDCE1FF), 0);
    lv_obj_set_style_text_font(lbl, &lv_font_montserrat_8, 0);
    lv_obj_set_pos(lbl, 4, 2);

    // Tag (direita, ex: "5h" / "7d")
    if (tag && tag[0]) {
        lv_obj_t *ltag = lv_label_create(panel);
        lv_label_set_text(ltag, tag);
        lv_obj_set_style_text_color(ltag, lv_color_hex(0x4B5073), 0);
        lv_obj_set_style_text_font(ltag, &lv_font_montserrat_8, 0);
        lv_obj_align(ltag, LV_ALIGN_TOP_RIGHT, -4, 2);
    }

    // Barra de progresso
    lv_obj_t *bar = lv_bar_create(panel);
    lv_obj_set_size(bar, LCD_W - 6, 9);
    lv_obj_set_pos(bar, 3, 13);
    lv_bar_set_range(bar, 0, 100);
    lv_bar_set_value(bar, 0, LV_ANIM_OFF);

    // Estilo do trilho (background)
    static lv_style_t s_track;
    lv_style_init(&s_track);
    lv_style_set_bg_color(&s_track, lv_color_hex(0x14142A));
    lv_style_set_radius(&s_track, 3);
    lv_obj_add_style(bar, &s_track, LV_PART_MAIN);

    // Estilo do indicador (fill) — cor atualizada via set_style_bg_color
    static lv_style_t s_ind;
    lv_style_init(&s_ind);
    lv_style_set_bg_color(&s_ind, lv_color_hex(0x26D24E));
    lv_style_set_radius(&s_ind, 3);
    lv_obj_add_style(bar, &s_ind, LV_PART_INDICATOR);

    // Percentagem (sobreposição direita, dentro da barra)
    lv_obj_t *lpct = lv_label_create(panel);
    lv_label_set_text(lpct, "--");
    lv_obj_set_style_text_color(lpct, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_text_font(lpct, &lv_font_montserrat_8, 0);
    lv_obj_set_pos(lpct, LCD_W - 28, 14);

    // Sub-label (rodapé — reset time ou vazio)
    lv_obj_t *lsub = lv_label_create(panel);
    lv_label_set_text(lsub, "");
    lv_obj_set_style_text_color(lsub, lv_color_hex(0x4B5073), 0);
    lv_obj_set_style_text_font(lsub, &lv_font_montserrat_8, 0);
    lv_obj_set_pos(lsub, 4, h - 11);

    if (out_bar)     *out_bar     = bar;
    if (out_lbl_pct) *out_lbl_pct = lpct;
    if (out_lbl_sub) *out_lbl_sub = lsub;
    return panel;
}

static void ui_create() {
    // Fundo geral
    lv_obj_t *scr = lv_screen_active();
    lv_obj_set_style_bg_color(scr, lv_color_hex(0x080A16), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);

    // ── Top bar ─────────────────────────────────────────────────
    lv_obj_t *topbar = lv_obj_create(scr);
    lv_obj_set_pos(topbar, 0, 0);
    lv_obj_set_size(topbar, LCD_W, 18);
    lv_obj_set_style_bg_color(topbar, lv_color_hex(0x080A16), 0);
    lv_obj_set_style_bg_opa(topbar, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(topbar, 0, 0);
    lv_obj_set_style_pad_all(topbar, 0, 0);
    lv_obj_set_style_radius(topbar, 0, 0);

    // Ícone WiFi (3 retângulos)
    for (int i = 0; i < 3; i++) {
        lv_obj_t *bar = lv_obj_create(topbar);
        lv_obj_set_style_border_width(bar, 0, 0);
        lv_obj_set_style_bg_color(bar, lv_color_hex(0x26D24E), 0);
        lv_obj_set_style_radius(bar, 1, 0);
        int bw = 2, bh = 4 + i * 3;
        lv_obj_set_size(bar, bw, bh);
        lv_obj_set_pos(bar, 2 + i * 4, 18 - 2 - bh);
    }
    // Salva referência ao primeiro retângulo para mudar cor depois
    g_dot_wifi = lv_obj_get_child(topbar, 0);

    // Título
    lv_obj_t *title = lv_label_create(topbar);
    lv_label_set_text(title, "CLAUDE METER");
    lv_obj_set_style_text_color(title, lv_color_hex(0xDCE1FF), 0);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_8, 0);
    lv_obj_align(title, LV_ALIGN_CENTER, 0, 0);

    // Ponto de status pulsante
    g_lbl_status = lv_label_create(topbar);
    lv_label_set_text(g_lbl_status, LV_SYMBOL_BULLET);
    lv_obj_set_style_text_color(g_lbl_status, lv_color_hex(0x4B5073), 0);
    lv_obj_set_style_text_font(g_lbl_status, &lv_font_montserrat_8, 0);
    lv_obj_align(g_lbl_status, LV_ALIGN_TOP_RIGHT, -3, 4);

    // ── Painéis de métrica ────────────────────────────────────────
    // session_pct = utilização da janela de 5 horas
    create_metric_panel(scr, 20, 38, lv_color_hex(0x0C1C50),
                        "SESSAO", "5h",
                        &g_bar_s, &g_lbl_s_pct, &g_lbl_s_reset);

    // weekly_pct = utilização semanal (todos os modelos)
    create_metric_panel(scr, 60, 38, lv_color_hex(0x2D0C44),
                        "SEMANAL", "7d",
                        &g_bar_w, &g_lbl_w_pct, nullptr);

    // opus_pct = utilização semanal do Opus/Design
    create_metric_panel(scr, 100, 30, lv_color_hex(0x08373E),
                        "OPUS", "7d",
                        &g_bar_o, &g_lbl_o_pct, nullptr);

    // ── Painel saldo ─────────────────────────────────────────────
    lv_obj_t *bal_panel = lv_obj_create(scr);
    lv_obj_set_pos(bal_panel, 0, 132);
    lv_obj_set_size(bal_panel, LCD_W, 26);
    lv_obj_set_style_bg_color(bal_panel, lv_color_hex(0x082C12), 0);
    lv_obj_set_style_bg_opa(bal_panel, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(bal_panel, 0, 0);
    lv_obj_set_style_pad_all(bal_panel, 0, 0);
    lv_obj_set_style_radius(bal_panel, 0, 0);

    lv_obj_t *lbal_title = lv_label_create(bal_panel);
    lv_label_set_text(lbal_title, "CREDITO EXTRA");
    lv_obj_set_style_text_color(lbal_title, lv_color_hex(0x4B5073), 0);
    lv_obj_set_style_text_font(lbal_title, &lv_font_montserrat_8, 0);
    lv_obj_set_pos(lbal_title, 4, 2);

    g_lbl_balance = lv_label_create(bal_panel);
    lv_label_set_text(g_lbl_balance, "N/D");
    lv_obj_set_style_text_color(g_lbl_balance, lv_color_hex(0x26D24E), 0);
    lv_obj_set_style_text_font(g_lbl_balance, &lv_font_montserrat_12, 0);
    lv_obj_align(g_lbl_balance, LV_ALIGN_BOTTOM_MID, 0, -2);
}

// Atualiza widgets com dados mais recentes
static void ui_update() {
    claude_usage_t u;
    if (xSemaphoreTake(g_usage_lock, pdMS_TO_TICKS(100)) != pdTRUE) return;
    u = g_usage;
    xSemaphoreGive(g_usage_lock);

    // Helper: atualiza barra + label %
    auto update_bar = [](lv_obj_t *bar, lv_obj_t *lbl, float pct) {
        char buf[8];
        if (pct < 0) {
            lv_bar_set_value(bar, 0, LV_ANIM_ON);
            lv_label_set_text(lbl, "--");
            lv_obj_set_style_bg_color(bar, lv_color_hex(0x4B5073), LV_PART_INDICATOR);
        } else {
            int v = (int)(pct + .5f);
            lv_bar_set_value(bar, v, LV_ANIM_ON);
            snprintf(buf, sizeof(buf), "%d%%", v);
            lv_label_set_text(lbl, buf);
            lv_obj_set_style_bg_color(bar, bar_color(pct), LV_PART_INDICATOR);
            lv_obj_set_style_text_color(lbl, bar_color(pct), 0);
        }
    };

    update_bar(g_bar_s, g_lbl_s_pct, u.session_pct);
    update_bar(g_bar_w, g_lbl_w_pct, u.weekly_pct);
    update_bar(g_bar_o, g_lbl_o_pct, u.opus_pct);

    // Reset time
    if (g_lbl_s_reset) {
        if (u.reset_secs > 0) {
            int h = u.reset_secs / 3600, m = (u.reset_secs % 3600) / 60;
            char rb[20];
            if (h > 0) snprintf(rb, sizeof(rb), "reset: %dh%02dm", h, m);
            else        snprintf(rb, sizeof(rb), "reset: %dm", m);
            lv_label_set_text(g_lbl_s_reset, rb);
        } else {
            lv_label_set_text(g_lbl_s_reset, "");
        }
    }

    // Saldo extra
    char bbuf[16];
    if (u.extra_balance < 0) {
        lv_label_set_text(g_lbl_balance, "N/D");
        lv_obj_set_style_text_color(g_lbl_balance, lv_color_hex(0x4B5073), 0);
    } else {
        snprintf(bbuf, sizeof(bbuf), "R$%.2f", u.extra_balance);
        lv_label_set_text(g_lbl_balance, bbuf);
        lv_color_t bc = (u.extra_balance < 5.f) ? lv_color_hex(0xDCC800) : lv_color_hex(0x26D24E);
        lv_obj_set_style_text_color(g_lbl_balance, bc, 0);
    }

    // Status WiFi (ícone)
    lv_color_t wc = g_wifi_ok ? lv_color_hex(0x26D24E) : lv_color_hex(0xD22626);
    for (int i = 0; i < 3; i++) {
        lv_obj_t *bar_w = lv_obj_get_parent(g_dot_wifi);
        lv_obj_t *child = lv_obj_get_child(bar_w, i);
        if (child) lv_obj_set_style_bg_color(child, wc, 0);
    }
}

// ════════════════════ TASKS ════════════════════════════════════════

// Task de fetch HTTP — roda a cada REFRESH_SEC
static void fetch_task(void *) {
    // Aguarda WiFi
    xEventGroupWaitBits(g_wifi_events, WIFI_CONNECTED_BIT,
                        pdFALSE, pdFALSE, portMAX_DELAY);
    vTaskDelay(pdMS_TO_TICKS(1000));  // dá tempo ao TCP stack

    while (true) {
        // Pisca status verde durante fetch
        if (lvgl_port_lock(100)) {
            lv_obj_set_style_text_color(g_lbl_status, lv_color_hex(0x26D24E), 0);
            lvgl_port_unlock();
        }

        fetch_usage();

        if (lvgl_port_lock(100)) {
            ui_update();
            lv_obj_set_style_text_color(g_lbl_status,
                g_wifi_ok ? lv_color_hex(0x1A4A28) : lv_color_hex(0x4A1A1A), 0);
            lvgl_port_unlock();
        }

        vTaskDelay(pdMS_TO_TICKS(REFRESH_SEC * 1000));
    }
}

// Pulsa o ponto de status (blink) — leve task auxiliar
static void blink_task(void *) {
    uint8_t tick = 0;
    while (true) {
        vTaskDelay(pdMS_TO_TICKS(1000));
        tick ^= 1;
        lv_color_t c = tick
            ? (g_wifi_ok ? lv_color_hex(0x1A6A38) : lv_color_hex(0x6A1A1A))
            : lv_color_hex(0x0D0D20);
        if (lvgl_port_lock(50)) {
            lv_obj_set_style_text_color(g_lbl_status, c, 0);
            lvgl_port_unlock();
        }
    }
}

// ════════════════════ APP MAIN ═════════════════════════════════════

extern "C" void app_main() {
    // NVS (necessário para WiFi)
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase();
        nvs_flash_init();
    }

    g_usage_lock = xSemaphoreCreateMutex();

    // ── SPI bus ────────────────────────────────────────────────────
    spi_bus_config_t bus = {};
    bus.mosi_io_num  = GPIO_MOSI;
    bus.miso_io_num  = -1;
    bus.sclk_io_num  = GPIO_SCLK;
    bus.quadwp_io_num = -1;
    bus.quadhd_io_num = -1;
    bus.max_transfer_sz = LCD_W * LCD_H * 2;
    ESP_ERROR_CHECK(spi_bus_initialize(LCD_HOST, &bus, SPI_DMA_CH_AUTO));

    spi_device_interface_config_t dev = {};
    dev.clock_speed_hz = LCD_FREQ_HZ;
    dev.mode           = 0;
    dev.spics_io_num   = GPIO_CS;
    dev.queue_size     = 7;
    dev.pre_cb         = spi_pre_transfer;
    ESP_ERROR_CHECK(spi_bus_add_device(LCD_HOST, &dev, &g_spi));

    // ── GPIO RST e DC ─────────────────────────────────────────────
    gpio_config_t io = {};
    io.pin_bit_mask  = (1ULL << GPIO_DC) | (1ULL << GPIO_RST);
    io.mode          = GPIO_MODE_OUTPUT;
    gpio_config(&io);
    gpio_set_level((gpio_num_t)GPIO_DC,  1);
    gpio_set_level((gpio_num_t)GPIO_RST, 1);

    // ── ST7735 init ────────────────────────────────────────────────
    st7735_init();
    ESP_LOGI(TAG, "ST7735 inicializado");

    // ── LVGL init — lvgl_port gerencia tick e mutex ────────────────
    const lvgl_port_cfg_t lvgl_cfg = ESP_LVGL_PORT_INIT_CONFIG();
    lvgl_port_init(&lvgl_cfg);

    // Cria display com driver SPI próprio — sem esp_lcd panel.
    // LVGL v9: byte swap via LV_COLOR_FORMAT_RGB565_SWAPPED (LV_COLOR_16_SWAP foi removido)
    static lv_color_t disp_buf1[LCD_W * 20];
    static lv_color_t disp_buf2[LCD_W * 20];
    g_disp = lv_display_create(LCD_W, LCD_H);
    lv_display_set_color_format(g_disp, LV_COLOR_FORMAT_RGB565_SWAPPED);
    lv_display_set_buffers(g_disp, disp_buf1, disp_buf2,
                           sizeof(disp_buf1), LV_DISPLAY_RENDER_MODE_PARTIAL);
    lv_display_set_flush_cb(g_disp, lvgl_flush_cb);

    // ── Cria UI ────────────────────────────────────────────────────
    if (lvgl_port_lock(0)) {
        ui_create();
        lvgl_port_unlock();
    }

    // ── WiFi ───────────────────────────────────────────────────────
    wifi_init();
    ESP_LOGI(TAG, "WiFi iniciando...");

    // ── Tasks ──────────────────────────────────────────────────────
    // lvgl_port_add_disp não foi chamado → não há task interna de LVGL;
    // criamos a nossa que chama lv_timer_handler().
    xTaskCreate(fetch_task,  "fetch",  8192, nullptr, 5, nullptr);
    xTaskCreate(blink_task,  "blink",  2048, nullptr, 3, nullptr);
    xTaskCreate([](void*) {
        while (true) {
            if (lvgl_port_lock(10)) {
                lv_timer_handler();
                lvgl_port_unlock();
            }
            vTaskDelay(pdMS_TO_TICKS(10));
        }
    }, "lvgl", 8192, nullptr, 4, nullptr);

    ESP_LOGI(TAG, "ClaudeMeter iniciado");
}
