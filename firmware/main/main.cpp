/*
 * ClaudeMeter — ESP-IDF 5.x + LVGL v9
 * ESP32-C3 Super Mini + ST7735 1.8" SPI (128×160 portrait)
 *
 * Layout: topbar | arc sessão grande | seção semanal+extra
 * Dados via proxy HTTP local (ver /proxy/server.py).
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
#include "esp_adc/adc_oneshot.h"
#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"
#include "cJSON.h"

#include "lvgl.h"
#include "esp_lvgl_port.h"

// ─── CONFIGURAÇÃO ──────────────────────────────────────────────────
#define WIFI_SSID      "BRARUS_IoT"
#define WIFI_PASS      "L4c3rd4S4r4c*"
#define PROXY_URL      "http://192.168.68.108:8000/usage"
#define REFRESH_SEC    60

// ─── PINOS ─────────────────────────────────────────────────────────
#define GPIO_MOSI     1
#define GPIO_SCLK    21
#define GPIO_CS       4
#define GPIO_DC       3
#define GPIO_RST      2
#define GPIO_BAT_ADC  0

// ─── DISPLAY (portrait) ─────────────────────────────────────────────
#define LCD_HOST        SPI2_HOST
#define LCD_FREQ_HZ     (26 * 1000 * 1000)
#define LCD_W           128
#define LCD_H           160
// Offsets GREENTAB em portrait
#define ST7735_X_OFFSET  2
#define ST7735_Y_OFFSET  1

// ─── CORES ──────────────────────────────────────────────────────────
#define C_BG        0x141414   // fundo
#define C_TOPBAR    0x1A1A1A   // barra topo
#define C_DIV       0x2A2A2A   // divisor
#define C_SESSION   0xE07040   // arco sessão (coral)
#define C_WEEKLY    0xA0A0A0   // arco semanal (cinza)
#define C_ARC_BG    0x2A2A2A   // trilho do arco
#define C_GREEN     0x26D24E   // dot heartbeat + saldo extra
#define C_LABEL     0x606060   // labels "SESSAO", "SEMANAL", "EXTRA"
#define C_TEXT      0xC8C8C8   // texto secundário (tempo reset)
#define C_WHITE     0xE8E8E8   // texto principal

// ─── BATERIA ────────────────────────────────────────────────────────
#define BAT_ADC_CHAN      ADC_CHANNEL_0
#define BAT_V_FULL_MV     4200
#define BAT_V_EMPTY_MV    3000
#define BAT_DIV_MULT     3251   // R1+R2 = 102.6k+222.5k (×10 para precisão inteira)
#define BAT_DIV_DIV      2225   // R2 = 222.5k (×10)
#define BAT_CHG_THR_MV    80

static const char *TAG = "claudemeter";

// ─── DADOS ──────────────────────────────────────────────────────────
typedef struct {
    float session_pct;
    float weekly_pct;
    float extra_balance;
    int   session_reset_secs;
    int   weekly_reset_secs;
    char  weekly_reset_day[4];   // "MON".."SUN"
    char  weekly_reset_time[6];  // "HH:MM" Brasília local
    bool  valid;
} claude_usage_t;

static claude_usage_t g_usage = {0, 0, -1, -1, -1, "", "", false};
static SemaphoreHandle_t g_usage_lock;

// ─── WiFi ───────────────────────────────────────────────────────────
#define WIFI_CONNECTED_BIT BIT0
#define WIFI_FAIL_BIT      BIT1
static EventGroupHandle_t g_wifi_events;
static bool g_wifi_ok = false;

// ─── LVGL globals ───────────────────────────────────────────────────
static lv_display_t      *g_disp = nullptr;
static spi_device_handle_t g_spi;

// ─── Widgets ────────────────────────────────────────────────────────
static lv_obj_t *g_arc_session;
static lv_obj_t *g_lbl_session_pct;
static lv_obj_t *g_lbl_session_label;
static lv_obj_t *g_lbl_session_reset;

static lv_obj_t *g_arc_weekly;
static lv_obj_t *g_lbl_weekly_pct;
static lv_obj_t *g_lbl_weekly_reset;

static lv_obj_t *g_lbl_balance;

static lv_obj_t *g_dot_heartbeat;
static lv_obj_t *g_wifi_bars[3];
static lv_obj_t *g_lbl_bat;

// ─── ADC bateria ─────────────────────────────────────────────────────
static adc_oneshot_unit_handle_t g_adc_handle;
static adc_cali_handle_t         g_adc_cali;
static struct { int mv; int pct; bool charging; } g_bat = {3700, 50, false};
static SemaphoreHandle_t g_bat_lock;

// ════════════════════ ST7735 DRIVER ════════════════════════════════

static void IRAM_ATTR spi_pre_transfer(spi_transaction_t *t) {
    gpio_set_level((gpio_num_t)GPIO_DC, (int)(intptr_t)t->user);
}

static void st7735_cmd(uint8_t cmd) {
    spi_transaction_t t = {};
    t.length     = 8;
    t.tx_data[0] = cmd;
    t.user       = (void *)0;
    t.flags      = SPI_TRANS_USE_TXDATA;
    spi_device_polling_transmit(g_spi, &t);
}

static void st7735_data(const uint8_t *data, size_t len) {
    if (!len) return;
    spi_transaction_t t = {};
    t.length    = len * 8;
    t.tx_buffer = data;
    t.user      = (void *)1;
    spi_device_polling_transmit(g_spi, &t);
}

static void st7735_data1(uint8_t d) { st7735_data(&d, 1); }

static void st7735_set_window(int x0, int y0, int x1, int y1) {
    st7735_cmd(0x2A);
    uint8_t cx[4] = {0, (uint8_t)x0, 0, (uint8_t)x1};
    st7735_data(cx, 4);
    st7735_cmd(0x2B);
    uint8_t cy[4] = {0, (uint8_t)y0, 0, (uint8_t)y1};
    st7735_data(cy, 4);
    st7735_cmd(0x2C);
}

static void st7735_init() {
    gpio_set_level((gpio_num_t)GPIO_RST, 0);
    vTaskDelay(pdMS_TO_TICKS(20));
    gpio_set_level((gpio_num_t)GPIO_RST, 1);
    vTaskDelay(pdMS_TO_TICKS(150));

    st7735_cmd(0x01); vTaskDelay(pdMS_TO_TICKS(150));  // SWRESET
    st7735_cmd(0x11); vTaskDelay(pdMS_TO_TICKS(255));  // SLPOUT

    // Frame rate
    st7735_cmd(0xB1); st7735_data1(0x01); st7735_data1(0x2C); st7735_data1(0x2D);
    st7735_cmd(0xB2); st7735_data1(0x01); st7735_data1(0x2C); st7735_data1(0x2D);
    st7735_cmd(0xB3);
    for (int i = 0; i < 6; i++)
        st7735_data1(i < 3 ? (i==0?0x01:i==1?0x2C:0x2D) : (i==3?0x01:i==4?0x2C:0x2D));

    st7735_cmd(0xB4); st7735_data1(0x07);

    st7735_cmd(0xC0); st7735_data1(0xA2); st7735_data1(0x02); st7735_data1(0x84);
    st7735_cmd(0xC1); st7735_data1(0xC5);
    st7735_cmd(0xC2); st7735_data1(0x0A); st7735_data1(0x00);
    st7735_cmd(0xC3); st7735_data1(0x8A); st7735_data1(0x2A);
    st7735_cmd(0xC4); st7735_data1(0x8A); st7735_data1(0xEE);
    st7735_cmd(0xC5); st7735_data1(0x0E);

    st7735_cmd(0x20);  // INVOFF

    // Portrait: sem mirror/swap, BGR=0
    st7735_cmd(0x36); st7735_data1(0x00);
    st7735_cmd(0x3A); st7735_data1(0x05);  // COLMOD 16-bit

    // Área GREENTAB em landscape (x offset=1, y offset=2)
    st7735_cmd(0x2A);
    { uint8_t d[] = {0x00, ST7735_X_OFFSET, 0x00, LCD_W - 1 + ST7735_X_OFFSET}; st7735_data(d, 4); }
    st7735_cmd(0x2B);
    { uint8_t d[] = {0x00, ST7735_Y_OFFSET, 0x00, LCD_H - 1 + ST7735_Y_OFFSET}; st7735_data(d, 4); }

    // Gamma
    st7735_cmd(0xE0);
    { uint8_t g[] = {0x0F,0x1A,0x0F,0x18,0x2F,0x28,0x20,0x22,
                     0x1F,0x1B,0x23,0x37,0x00,0x07,0x02,0x10};
      st7735_data(g, 16); }
    st7735_cmd(0xE1);
    { uint8_t g[] = {0x0F,0x1B,0x0F,0x17,0x33,0x2C,0x29,0x2E,
                     0x30,0x30,0x39,0x3F,0x00,0x07,0x03,0x10};
      st7735_data(g, 16); }

    st7735_cmd(0x13); vTaskDelay(pdMS_TO_TICKS(10));
    st7735_cmd(0x29); vTaskDelay(pdMS_TO_TICKS(100));
}

// ════════════════════ LVGL FLUSH ════════════════════════════════════

static void lvgl_flush_cb(lv_display_t *disp, const lv_area_t *area, uint8_t *px_map) {
    int x0 = area->x1 + ST7735_X_OFFSET;
    int y0 = area->y1 + ST7735_Y_OFFSET;
    int x1 = area->x2 + ST7735_X_OFFSET;
    int y1 = area->y2 + ST7735_Y_OFFSET;

    st7735_set_window(x0, y0, x1, y1);

    size_t len = (size_t)(x1 - x0 + 1) * (size_t)(y1 - y0 + 1) * 2;
    spi_transaction_t t = {};
    t.length    = len * 8;
    t.tx_buffer = px_map;
    t.user      = (void *)1;
    spi_device_polling_transmit(g_spi, &t);

    lv_display_flush_ready(disp);
}

// ════════════════════ WiFi ══════════════════════════════════════════

static void wifi_event_handler(void *arg, esp_event_base_t base,
                               int32_t id, void *data) {
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        g_wifi_ok = false;
        xEventGroupSetBits(g_wifi_events, WIFI_FAIL_BIT);
        vTaskDelay(pdMS_TO_TICKS(3000));
        esp_wifi_connect();
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        g_wifi_ok = true;
        xEventGroupSetBits(g_wifi_events, WIFI_CONNECTED_BIT);
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

// ════════════════════ HTTP FETCH ════════════════════════════════════

#define HTTP_BUF_SIZE 512
static char g_http_buf[HTTP_BUF_SIZE];
static int  g_http_len = 0;

static esp_err_t http_event_handler(esp_http_client_event_t *evt) {
    if (evt->event_id == HTTP_EVENT_ON_DATA) {
        int room = HTTP_BUF_SIZE - g_http_len - 1;
        int copy = (evt->data_len < room) ? evt->data_len : room;
        if (copy > 0) {
            memcpy(g_http_buf + g_http_len, evt->data, copy);
            g_http_len += copy;
        }
    }
    return ESP_OK;
}

// Formata segundos como "2h 14m" ou "45m"
static void fmt_reset(char *buf, size_t sz, int secs) {
    if (secs <= 0) { snprintf(buf, sz, "--"); return; }
    int h = secs / 3600, m = (secs % 3600) / 60;
    if (h > 0) snprintf(buf, sz, "%dh %02dm", h, m);
    else        snprintf(buf, sz, "%dm", m);
}

static bool fetch_usage() {
    g_http_len = 0;
    memset(g_http_buf, 0, sizeof(g_http_buf));

    esp_http_client_config_t cfg = {};
    cfg.url           = PROXY_URL;
    cfg.timeout_ms    = 8000;
    cfg.event_handler = http_event_handler;

    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client) return false;

    esp_err_t err = esp_http_client_perform(client);
    int status    = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);

    if (err != ESP_OK || status != 200) return false;

    cJSON *root = cJSON_ParseWithLength(g_http_buf, g_http_len);
    if (!root) return false;

    auto f = [&](const char *k, float fb) -> float {
        cJSON *item = cJSON_GetObjectItemCaseSensitive(root, k);
        return (item && cJSON_IsNumber(item)) ? (float)item->valuedouble : fb;
    };

    if (xSemaphoreTake(g_usage_lock, pdMS_TO_TICKS(500)) == pdTRUE) {
        g_usage.session_pct        = f("session_pct",      0);
        g_usage.weekly_pct         = f("weekly_pct",       0);
        g_usage.extra_balance      = f("extra_balance",   -1);
        g_usage.session_reset_secs = (int)f("tokens_reset_secs", -1);
        g_usage.weekly_reset_secs  = (int)f("weekly_reset_secs", -1);
        cJSON *day_item = cJSON_GetObjectItemCaseSensitive(root, "weekly_reset_day");
        if (day_item && cJSON_IsString(day_item) && day_item->valuestring) {
            strncpy(g_usage.weekly_reset_day, day_item->valuestring, 3);
            g_usage.weekly_reset_day[3] = '\0';
        } else {
            g_usage.weekly_reset_day[0] = '\0';
        }
        cJSON *time_item = cJSON_GetObjectItemCaseSensitive(root, "weekly_reset_time");
        if (time_item && cJSON_IsString(time_item) && time_item->valuestring) {
            strncpy(g_usage.weekly_reset_time, time_item->valuestring, 5);
            g_usage.weekly_reset_time[5] = '\0';
        } else {
            g_usage.weekly_reset_time[0] = '\0';
        }
        g_usage.valid = true;
        xSemaphoreGive(g_usage_lock);
    }

    cJSON_Delete(root);
    return true;
}

// ════════════════════ LVGL UI ═══════════════════════════════════════


static void ui_create() {
    lv_obj_t *scr = lv_screen_active();
    lv_obj_set_style_bg_color(scr, lv_color_hex(C_BG), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);

    // ── Top bar (18px) ───────────────────────────────────────────────
    lv_obj_t *topbar = lv_obj_create(scr);
    lv_obj_set_pos(topbar, 0, 0);
    lv_obj_set_size(topbar, LCD_W, 18);
    lv_obj_set_style_bg_color(topbar, lv_color_hex(C_TOPBAR), 0);
    lv_obj_set_style_bg_opa(topbar, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(topbar, 0, 0);
    lv_obj_set_style_pad_all(topbar, 0, 0);
    lv_obj_set_style_radius(topbar, 0, 0);

    // Barras WiFi
    for (int i = 0; i < 3; i++) {
        g_wifi_bars[i] = lv_obj_create(topbar);
        lv_obj_set_style_border_width(g_wifi_bars[i], 0, 0);
        lv_obj_set_style_radius(g_wifi_bars[i], 1, 0);
        lv_obj_set_style_bg_color(g_wifi_bars[i], lv_color_hex(C_DIV), 0);
        int bh = 4 + i * 3;
        lv_obj_set_size(g_wifi_bars[i], 2, bh);
        lv_obj_set_pos(g_wifi_bars[i], 3 + i * 4, 18 - 2 - bh);
    }

    // "CLAUDE • WATCH" centralizado — portrait: LCD_W=128, centro x=64
    lv_obj_t *lbl_l = lv_label_create(topbar);
    lv_label_set_text(lbl_l, "CLAUDE");
    lv_obj_set_style_text_color(lbl_l, lv_color_hex(C_WHITE), 0);
    lv_obj_set_style_text_font(lbl_l, &lv_font_montserrat_8, 0);
    lv_obj_set_pos(lbl_l, 23, 5);

    g_dot_heartbeat = lv_obj_create(topbar);
    lv_obj_set_size(g_dot_heartbeat, 5, 5);
    lv_obj_set_style_radius(g_dot_heartbeat, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(g_dot_heartbeat, lv_color_hex(C_GREEN), 0);
    lv_obj_set_style_bg_opa(g_dot_heartbeat, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(g_dot_heartbeat, 0, 0);
    lv_obj_set_pos(g_dot_heartbeat, 62, 7);

    lv_obj_t *lbl_r = lv_label_create(topbar);
    lv_label_set_text(lbl_r, "WATCH");
    lv_obj_set_style_text_color(lbl_r, lv_color_hex(C_WHITE), 0);
    lv_obj_set_style_text_font(lbl_r, &lv_font_montserrat_8, 0);
    lv_obj_set_pos(lbl_r, 70, 5);

    g_lbl_bat = lv_label_create(topbar);
    lv_label_set_text(g_lbl_bat, LV_SYMBOL_BATTERY_FULL);
    lv_obj_set_style_text_color(g_lbl_bat, lv_color_hex(C_TEXT), 0);
    lv_obj_set_style_text_font(g_lbl_bat, &lv_font_montserrat_12, 0);
    lv_obj_align(g_lbl_bat, LV_ALIGN_TOP_RIGHT, -3, 3);

    // ── Arc sessão — grande, centrado (portrait) ─────────────────────
    // Centro: x=64, y=61; raio=41  → arco y=20..102
    const int ARC_S_X = 64, ARC_S_Y = 61, ARC_S_R = 41;

    g_arc_session = lv_arc_create(scr);
    lv_obj_remove_style_all(g_arc_session);
    lv_obj_set_size(g_arc_session, ARC_S_R * 2, ARC_S_R * 2);
    lv_obj_set_pos(g_arc_session, ARC_S_X - ARC_S_R, ARC_S_Y - ARC_S_R);
    lv_arc_set_rotation(g_arc_session, 270);
    lv_arc_set_bg_angles(g_arc_session, 0, 360);
    lv_arc_set_range(g_arc_session, 0, 100);
    lv_arc_set_value(g_arc_session, 0);
    lv_arc_set_mode(g_arc_session, LV_ARC_MODE_NORMAL);

    lv_obj_set_style_arc_color(g_arc_session, lv_color_hex(C_ARC_BG), LV_PART_MAIN);
    lv_obj_set_style_arc_width(g_arc_session, 10, LV_PART_MAIN);
    lv_obj_set_style_arc_color(g_arc_session, lv_color_hex(C_SESSION), LV_PART_INDICATOR);
    lv_obj_set_style_arc_width(g_arc_session, 10, LV_PART_INDICATOR);
    lv_obj_set_style_bg_opa(g_arc_session, LV_OPA_TRANSP, LV_PART_KNOB);

    // % sessão — centralizado dentro do arco
    g_lbl_session_pct = lv_label_create(scr);
    lv_label_set_text(g_lbl_session_pct, "--");
    lv_obj_set_size(g_lbl_session_pct, 80, 28);
    lv_obj_set_pos(g_lbl_session_pct, ARC_S_X - 40, ARC_S_Y - 14);
    lv_obj_set_style_text_align(g_lbl_session_pct, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_color(g_lbl_session_pct, lv_color_hex(C_WHITE), 0);
    lv_obj_set_style_text_font(g_lbl_session_pct, &lv_font_montserrat_20, 0);

    // "SESSION" — centralizado abaixo do arco
    g_lbl_session_label = lv_label_create(scr);
    lv_label_set_text(g_lbl_session_label, "SESSION");
    lv_obj_set_size(g_lbl_session_label, LCD_W, 10);
    lv_obj_set_pos(g_lbl_session_label, 0, 105);
    lv_obj_set_style_text_align(g_lbl_session_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_color(g_lbl_session_label, lv_color_hex(C_LABEL), 0);
    lv_obj_set_style_text_font(g_lbl_session_label, &lv_font_montserrat_8, 0);

    // Tempo de reset sessão — laranja, centralizado, font_14
    // Termina em y=115+14=129; divider em y=131
    g_lbl_session_reset = lv_label_create(scr);
    lv_label_set_text(g_lbl_session_reset, "--");
    lv_obj_set_size(g_lbl_session_reset, LCD_W, 18);
    lv_obj_set_pos(g_lbl_session_reset, 0, 115);
    lv_obj_set_style_text_align(g_lbl_session_reset, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_color(g_lbl_session_reset, lv_color_hex(C_SESSION), 0);
    lv_obj_set_style_text_font(g_lbl_session_reset, &lv_font_montserrat_14, 0);

    // ── Linha divisória horizontal ───────────────────────────────────
    lv_obj_t *hdiv = lv_obj_create(scr);
    lv_obj_set_pos(hdiv, 0, 131);
    lv_obj_set_size(hdiv, LCD_W, 1);
    lv_obj_set_style_bg_color(hdiv, lv_color_hex(C_DIV), 0);
    lv_obj_set_style_bg_opa(hdiv, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(hdiv, 0, 0);
    lv_obj_set_style_radius(hdiv, 0, 0);

    // ── Linha divisória vertical (weekly | extra) ────────────────────
    lv_obj_t *vdiv = lv_obj_create(scr);
    lv_obj_set_pos(vdiv, 64, 132);
    lv_obj_set_size(vdiv, 1, LCD_H - 132);
    lv_obj_set_style_bg_color(vdiv, lv_color_hex(C_DIV), 0);
    lv_obj_set_style_bg_opa(vdiv, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(vdiv, 0, 0);
    lv_obj_set_style_radius(vdiv, 0, 0);

    // ── Bottom-left: arc weekly + labels ─────────────────────────────
    // Arc: centro (15, 146), raio=11  → pos(4, 135) size 22×22
    const int ARC_W_X = 15, ARC_W_Y = 146, ARC_W_R = 11;

    g_arc_weekly = lv_arc_create(scr);
    lv_obj_remove_style_all(g_arc_weekly);
    lv_obj_set_size(g_arc_weekly, ARC_W_R * 2, ARC_W_R * 2);
    lv_obj_set_pos(g_arc_weekly, ARC_W_X - ARC_W_R, ARC_W_Y - ARC_W_R);
    lv_arc_set_rotation(g_arc_weekly, 270);
    lv_arc_set_bg_angles(g_arc_weekly, 0, 360);
    lv_arc_set_range(g_arc_weekly, 0, 100);
    lv_arc_set_value(g_arc_weekly, 0);
    lv_arc_set_mode(g_arc_weekly, LV_ARC_MODE_NORMAL);

    lv_obj_set_style_arc_color(g_arc_weekly, lv_color_hex(C_ARC_BG), LV_PART_MAIN);
    lv_obj_set_style_arc_width(g_arc_weekly, 4, LV_PART_MAIN);
    lv_obj_set_style_arc_color(g_arc_weekly, lv_color_hex(C_WEEKLY), LV_PART_INDICATOR);
    lv_obj_set_style_arc_width(g_arc_weekly, 4, LV_PART_INDICATOR);
    lv_obj_set_style_bg_opa(g_arc_weekly, LV_OPA_TRANSP, LV_PART_KNOB);

    lv_obj_t *lbl_semanal = lv_label_create(scr);
    lv_label_set_text(lbl_semanal, "WEEKLY");
    lv_obj_set_size(lbl_semanal, 33, 10);
    lv_obj_set_pos(lbl_semanal, 29, 132);
    lv_obj_set_style_text_color(lbl_semanal, lv_color_hex(C_LABEL), 0);
    lv_obj_set_style_text_font(lbl_semanal, &lv_font_montserrat_8, 0);

    g_lbl_weekly_pct = lv_label_create(scr);
    lv_label_set_text(g_lbl_weekly_pct, "--");
    lv_obj_set_size(g_lbl_weekly_pct, 33, 12);
    lv_obj_set_pos(g_lbl_weekly_pct, 29, 140);
    lv_obj_set_style_text_color(g_lbl_weekly_pct, lv_color_hex(C_WHITE), 0);
    lv_obj_set_style_text_font(g_lbl_weekly_pct, &lv_font_montserrat_12, 0);

    // Abaixo do arco, largura total do painel esquerdo: "MON 09:00"
    g_lbl_weekly_reset = lv_label_create(scr);
    lv_label_set_text(g_lbl_weekly_reset, "--:--");
    lv_obj_set_size(g_lbl_weekly_reset, 57, 10);
    lv_obj_set_pos(g_lbl_weekly_reset, 4, 152);
    lv_obj_set_style_text_color(g_lbl_weekly_reset, lv_color_hex(C_TEXT), 0);
    lv_obj_set_style_text_font(g_lbl_weekly_reset, &lv_font_montserrat_8, 0);

    // ── Bottom-right: EXTRA + balance ────────────────────────────────
    lv_obj_t *lbl_extra = lv_label_create(scr);
    lv_label_set_text(lbl_extra, "EXTRA");
    lv_obj_set_size(lbl_extra, 55, 10);
    lv_obj_set_pos(lbl_extra, 68, 132);
    lv_obj_set_style_text_color(lbl_extra, lv_color_hex(C_LABEL), 0);
    lv_obj_set_style_text_font(lbl_extra, &lv_font_montserrat_8, 0);

    g_lbl_balance = lv_label_create(scr);
    lv_label_set_text(g_lbl_balance, "--");
    lv_obj_set_size(g_lbl_balance, 55, 18);
    lv_obj_set_pos(g_lbl_balance, 68, 144);
    lv_obj_set_style_text_color(g_lbl_balance, lv_color_hex(C_GREEN), 0);
    lv_obj_set_style_text_font(g_lbl_balance, &lv_font_montserrat_14, 0);
}

static void ui_update() {
    claude_usage_t u;
    if (xSemaphoreTake(g_usage_lock, pdMS_TO_TICKS(100)) != pdTRUE) return;
    u = g_usage;
    xSemaphoreGive(g_usage_lock);

    // Arc sessão — cor sempre laranja (C_SESSION)
    int sv = (u.session_pct > 0) ? (int)(u.session_pct + .5f) : 0;
    lv_arc_set_value(g_arc_session, sv);

    char buf[24];
    if (u.valid && u.session_pct >= 0) {
        snprintf(buf, sizeof(buf), "%d%%", sv);
        lv_label_set_text(g_lbl_session_pct, buf);
        lv_obj_set_style_text_color(g_lbl_session_pct, lv_color_hex(C_WHITE), 0);
    } else {
        lv_label_set_text(g_lbl_session_pct, "--");
        lv_obj_set_style_text_color(g_lbl_session_pct, lv_color_hex(C_LABEL), 0);
    }

    // Reset sessão — cor laranja igual ao arco
    fmt_reset(buf, sizeof(buf), u.session_reset_secs);
    lv_label_set_text(g_lbl_session_reset, buf);
    lv_obj_set_style_text_color(g_lbl_session_reset, lv_color_hex(C_SESSION), 0);

    // Arc semanal
    int wv = (u.weekly_pct > 0) ? (int)(u.weekly_pct + .5f) : 0;
    lv_arc_set_value(g_arc_weekly, wv);

    if (u.valid && u.weekly_pct >= 0) {
        snprintf(buf, sizeof(buf), "%d%%", wv);
        lv_label_set_text(g_lbl_weekly_pct, buf);
    } else {
        lv_label_set_text(g_lbl_weekly_pct, "--");
    }

    // Reset semanal — horário local Brasília vindo direto do proxy
    if (u.weekly_reset_day[0] != '\0') {
        const char *t = u.weekly_reset_time[0] ? u.weekly_reset_time : "--:--";
        snprintf(buf, sizeof(buf), "%s %s", u.weekly_reset_day, t);
    } else {
        snprintf(buf, sizeof(buf), "--:--");
    }
    lv_label_set_text(g_lbl_weekly_reset, buf);

    // Saldo
    if (u.extra_balance >= 0) {
        snprintf(buf, sizeof(buf), "R$%.2f", u.extra_balance);
        lv_label_set_text(g_lbl_balance, buf);
    } else {
        lv_label_set_text(g_lbl_balance, "--");
    }

    // WiFi bars
    lv_color_t wc = g_wifi_ok ? lv_color_hex(C_GREEN) : lv_color_hex(0x4A1A1A);
    for (int i = 0; i < 3; i++)
        lv_obj_set_style_bg_color(g_wifi_bars[i], wc, 0);
}

// ════════════════════ TASKS ════════════════════════════════════════

static void fetch_task(void *) {
    xEventGroupWaitBits(g_wifi_events, WIFI_CONNECTED_BIT,
                        pdFALSE, pdFALSE, portMAX_DELAY);
    vTaskDelay(pdMS_TO_TICKS(1000));

    while (true) {
        fetch_usage();

        if (lvgl_port_lock(100)) {
            ui_update();
            lvgl_port_unlock();
        }

        vTaskDelay(pdMS_TO_TICKS(REFRESH_SEC * 1000));
    }
}

static void heartbeat_task(void *) {
    bool on = true;
    while (true) {
        vTaskDelay(pdMS_TO_TICKS(800));
        on = !on;
        if (lvgl_port_lock(50)) {
            lv_obj_set_style_bg_color(g_dot_heartbeat,
                on ? lv_color_hex(C_GREEN) : lv_color_hex(C_BG), 0);
            lvgl_port_unlock();
        }
    }
}

// ════════════════════ BATERIA ══════════════════════════════════════

static void battery_init() {
    adc_oneshot_unit_init_cfg_t unit_cfg = { .unit_id = ADC_UNIT_1 };
    ESP_ERROR_CHECK(adc_oneshot_new_unit(&unit_cfg, &g_adc_handle));

    adc_oneshot_chan_cfg_t chan_cfg = {
        .atten    = ADC_ATTEN_DB_12,
        .bitwidth = ADC_BITWIDTH_DEFAULT,
    };
    ESP_ERROR_CHECK(adc_oneshot_config_channel(g_adc_handle, BAT_ADC_CHAN, &chan_cfg));

    adc_cali_curve_fitting_config_t cali_cfg = {
        .unit_id  = ADC_UNIT_1,
        .chan     = BAT_ADC_CHAN,
        .atten    = ADC_ATTEN_DB_12,
        .bitwidth = ADC_BITWIDTH_DEFAULT,
    };
    adc_cali_create_scheme_curve_fitting(&cali_cfg, &g_adc_cali);
}

static int battery_read_mv() {
    int sum = 0;
    for (int i = 0; i < 8; i++) {
        int raw = 0;
        adc_oneshot_read(g_adc_handle, BAT_ADC_CHAN, &raw);
        sum += raw;
    }
    int mv_adc = 0;
    adc_cali_raw_to_voltage(g_adc_cali, sum / 8, &mv_adc);
    return mv_adc * BAT_DIV_MULT / BAT_DIV_DIV;
}

static void battery_task(void *) {
    int  prev_mv  = 0;
    bool charging = false;

    while (true) {
        int mv  = battery_read_mv();
        int pct = (mv - BAT_V_EMPTY_MV) * 100 / (BAT_V_FULL_MV - BAT_V_EMPTY_MV);
        pct = pct < 0 ? 0 : pct > 100 ? 100 : pct;

        if (prev_mv > 0) {
            int delta = mv - prev_mv;
            if (delta >  BAT_CHG_THR_MV) charging = true;
            else if (delta < -50)         charging = false;
        }
        prev_mv = mv;

        if (xSemaphoreTake(g_bat_lock, pdMS_TO_TICKS(100)) == pdTRUE) {
            g_bat.mv = mv; g_bat.pct = pct; g_bat.charging = charging;
            xSemaphoreGive(g_bat_lock);
        }

        if (lvgl_port_lock(100)) {
            const char *sym = charging  ? LV_SYMBOL_CHARGE        :
                              pct >= 75 ? LV_SYMBOL_BATTERY_FULL  :
                              pct >= 50 ? LV_SYMBOL_BATTERY_3     :
                              pct >= 25 ? LV_SYMBOL_BATTERY_2     :
                              pct >= 10 ? LV_SYMBOL_BATTERY_1     :
                                          LV_SYMBOL_BATTERY_EMPTY;
            lv_label_set_text(g_lbl_bat, sym);
            lv_color_t c = charging ? lv_color_hex(C_GREEN)  :
                           pct < 20 ? lv_color_hex(0xFF4040) :
                                      lv_color_hex(C_TEXT);
            lv_obj_set_style_text_color(g_lbl_bat, c, 0);
            lvgl_port_unlock();
        }

        vTaskDelay(pdMS_TO_TICKS(10000));
    }
}

// ════════════════════ APP MAIN ════════════════════════════════════

extern "C" void app_main() {
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase();
        nvs_flash_init();
    }

    g_usage_lock = xSemaphoreCreateMutex();
    g_bat_lock   = xSemaphoreCreateMutex();
    battery_init();

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

    // ── GPIO RST / DC ──────────────────────────────────────────────
    gpio_config_t io = {};
    io.pin_bit_mask = (1ULL << GPIO_DC) | (1ULL << GPIO_RST);
    io.mode         = GPIO_MODE_OUTPUT;
    gpio_config(&io);
    gpio_set_level((gpio_num_t)GPIO_DC,  1);
    gpio_set_level((gpio_num_t)GPIO_RST, 1);

    st7735_init();
    ESP_LOGI(TAG, "ST7735 OK (portrait 128x160)");

    // ── LVGL ───────────────────────────────────────────────────────
    const lvgl_port_cfg_t lvgl_cfg = ESP_LVGL_PORT_INIT_CONFIG();
    lvgl_port_init(&lvgl_cfg);

    static lv_color_t disp_buf1[LCD_W * 20];
    static lv_color_t disp_buf2[LCD_W * 20];
    g_disp = lv_display_create(LCD_W, LCD_H);
    lv_display_set_color_format(g_disp, LV_COLOR_FORMAT_RGB565_SWAPPED);
    lv_display_set_buffers(g_disp, disp_buf1, disp_buf2,
                           sizeof(disp_buf1), LV_DISPLAY_RENDER_MODE_PARTIAL);
    lv_display_set_flush_cb(g_disp, lvgl_flush_cb);

    // Tick LVGL
    {
        const esp_timer_create_args_t ta = {
            .callback = [](void*){ lv_tick_inc(5); },
            .name     = "lvgl_tick",
        };
        esp_timer_handle_t th;
        esp_timer_create(&ta, &th);
        esp_timer_start_periodic(th, 5000);
    }

    if (lvgl_port_lock(0)) {
        ui_create();
        lvgl_port_unlock();
    }

    wifi_init();

    // LVGL handler task
    xTaskCreate([](void*) {
        while (true) {
            if (lvgl_port_lock(10)) { lv_timer_handler(); lvgl_port_unlock(); }
            vTaskDelay(pdMS_TO_TICKS(10));
        }
    }, "lvgl", 8192, nullptr, 4, nullptr);

    xTaskCreate(fetch_task,     "fetch",  8192, nullptr, 5, nullptr);
    xTaskCreate(heartbeat_task, "hbeat",  2048, nullptr, 3, nullptr);
    xTaskCreate(battery_task,   "bat",    3072, nullptr, 3, nullptr);

    ESP_LOGI(TAG, "ClaudeMeter iniciado");
}
