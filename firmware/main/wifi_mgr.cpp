#include "wifi_mgr.h"

#include <cctype>
#include <cstdio>
#include <cstring>

#include "freertos/event_groups.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "esp_app_desc.h"
#include "esp_event.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "nvs.h"

#include "app_config.h"

static const char *TAG = "wifi";

// ─── Redes configuradas (secrets.h) ─────────────────────────────────
// Compatível com o formato antigo (WIFI_SSID / WIFI_PASS).
#if !defined(WIFI_NETWORKS) && defined(WIFI_SSID)
#define WIFI_NETWORKS { { WIFI_SSID, WIFI_PASS } }
#endif
#ifndef WIFI_NETWORKS
#error "Defina WIFI_NETWORKS em secrets.h (ver secrets.h.example)"
#endif
#ifndef RECOVERY_AP_PASS
#error "Defina RECOVERY_AP_PASS (8–63 caracteres) em secrets.h"
#endif

static constexpr bool str_eq(const char *a, const char *b) {
    return *a == *b && (*a == '\0' || str_eq(a + 1, b + 1));
}
static_assert(sizeof(RECOVERY_AP_PASS) - 1 >= 8 && sizeof(RECOVERY_AP_PASS) - 1 <= 63,
              "RECOVERY_AP_PASS precisa ter 8–63 caracteres (WPA2)");
static_assert(!str_eq(RECOVERY_AP_PASS, "troque-esta-senha"),
              "Defina uma RECOVERY_AP_PASS própria em secrets.h");

struct builtin_net_t { const char *ssid; const char *pass; };
static const builtin_net_t k_builtin[] = WIFI_NETWORKS;

typedef struct { char ssid[33]; char pass[65]; } wifi_net_t;
static wifi_net_t s_nets[1 + sizeof(k_builtin) / sizeof(k_builtin[0])];
static int        s_net_count = 0;

#define NVS_NS "wifi"

// ─── Estado ─────────────────────────────────────────────────────────
#define BIT_CONNECTED    BIT0
#define BIT_DISCONNECTED BIT1

static EventGroupHandle_t s_events;
static volatile bool      s_connected = false;
static wifi_status_cb_t   s_on_status;
static esp_netif_t       *s_ap_netif;
static SemaphoreHandle_t  s_saved;

static void status(const char *text) {
    if (s_on_status) s_on_status(text);
}

// ════════════════════ STA ══════════════════════════════════════════

static void on_event(void *, esp_event_base_t base, int32_t id, void *) {
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        s_connected = false;
        xEventGroupClearBits(s_events, BIT_CONNECTED);
        xEventGroupSetBits(s_events, BIT_DISCONNECTED);
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        s_connected = true;
        xEventGroupClearBits(s_events, BIT_DISCONNECTED);
        xEventGroupSetBits(s_events, BIT_CONNECTED);
    }
}

static void load_networks() {
    s_net_count = 0;

    // 1) Rede gravada pela recuperação (se houver)
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) == ESP_OK) {
        wifi_net_t n = {};
        size_t ls = sizeof(n.ssid), lp = sizeof(n.pass);
        if (nvs_get_str(h, "ssid", n.ssid, &ls) == ESP_OK &&
            nvs_get_str(h, "pass", n.pass, &lp) == ESP_OK && n.ssid[0])
            s_nets[s_net_count++] = n;
        nvs_close(h);
    }

    // 2) Redes do secrets.h, na ordem
    for (const auto &b : k_builtin) {
        wifi_net_t n = {};
        strncpy(n.ssid, b.ssid, sizeof(n.ssid) - 1);
        strncpy(n.pass, b.pass, sizeof(n.pass) - 1);
        s_nets[s_net_count++] = n;
    }
    ESP_LOGI(TAG, "%d rede(s) configurada(s)", s_net_count);
}

static bool try_connect(const wifi_net_t *n) {
    wifi_config_t w = {};
    strncpy((char *)w.sta.ssid,     n->ssid, sizeof(w.sta.ssid) - 1);
    strncpy((char *)w.sta.password, n->pass, sizeof(w.sta.password) - 1);

    esp_wifi_disconnect();
    xEventGroupClearBits(s_events, BIT_CONNECTED);
    if (esp_wifi_set_config(WIFI_IF_STA, &w) != ESP_OK) return false;
    esp_wifi_connect();

    ESP_LOGI(TAG, "conectando em \"%s\"...", n->ssid);
    EventBits_t bits = xEventGroupWaitBits(s_events, BIT_CONNECTED, pdFALSE, pdFALSE,
                                           pdMS_TO_TICKS(20000));
    return bits & BIT_CONNECTED;
}

// ════════════════════ RECUPERAÇÃO (SoftAP) ═════════════════════════

static void url_decode(char *s) {
    char *o = s;
    for (; *s; s++) {
        if (*s == '+') {
            *o++ = ' ';
        } else if (*s == '%' && isxdigit((unsigned char)s[1]) && isxdigit((unsigned char)s[2])) {
            char hex[3] = { s[1], s[2], '\0' };
            *o++ = (char)strtol(hex, nullptr, 16);
            s += 2;
        } else {
            *o++ = *s;
        }
    }
    *o = '\0';
}

// Extrai "key=valor" de um corpo x-www-form-urlencoded.
static bool form_get(const char *body, const char *key, char *out, size_t sz) {
    size_t kl = strlen(key);
    for (const char *p = body; p && *p; p = strchr(p, '&') ? strchr(p, '&') + 1 : nullptr) {
        if (strncmp(p, key, kl) == 0 && p[kl] == '=') {
            const char *v = p + kl + 1;
            size_t n = strcspn(v, "&");
            if (n >= sz) return false;
            memcpy(out, v, n);
            out[n] = '\0';
            url_decode(out);
            return true;
        }
    }
    return false;
}

static esp_err_t page_get(httpd_req_t *req) {
    char html[900];
    snprintf(html, sizeof(html),
        "<!doctype html><html><head><meta charset=utf-8>"
        "<meta name=viewport content='width=device-width,initial-scale=1'>"
        "<title>ClaudeMeter</title><style>body{font-family:sans-serif;max-width:22em;"
        "margin:2em auto;padding:0 1em}input,button{width:100%%;padding:.6em;margin:.3em 0;"
        "box-sizing:border-box}</style></head><body>"
        "<h2>ClaudeMeter %s</h2><p>Recuperação de Wi-Fi. A nova rede será tentada "
        "primeiro; as redes do firmware continuam como alternativa.</p>"
        "<form method=post action=/save>"
        "<input name=ssid placeholder='Nome da rede (SSID)' maxlength=32 required>"
        "<input name=pass type=password placeholder='Senha' maxlength=63>"
        "<button>Salvar e reiniciar</button></form></body></html>",
        esp_app_get_description()->version);
    httpd_resp_set_type(req, "text/html");
    return httpd_resp_send(req, html, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t save_post(httpd_req_t *req) {
    char body[256];
    if (req->content_len <= 0 || req->content_len >= sizeof(body))
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "corpo invalido");

    int got = 0;
    while (got < (int)req->content_len) {
        int r = httpd_req_recv(req, body + got, req->content_len - got);
        if (r <= 0) return ESP_FAIL;
        got += r;
    }
    body[got] = '\0';

    char ssid[33] = "", pass[65] = "";
    form_get(body, "ssid", ssid, sizeof(ssid));
    form_get(body, "pass", pass, sizeof(pass));
    size_t pl = strlen(pass);
    if (!ssid[0] || (pl > 0 && pl < 8))
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST,
                                   "SSID obrigatorio; senha vazia ou com 8-63 caracteres");

    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NS, NVS_READWRITE, &h);
    if (err == ESP_OK) {
        err = nvs_set_str(h, "ssid", ssid);
        if (err == ESP_OK) err = nvs_set_str(h, "pass", pass);
        if (err == ESP_OK) err = nvs_commit(h);
        nvs_close(h);
    }
    if (err != ESP_OK)
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "falha ao gravar");

    ESP_LOGI(TAG, "recuperação: rede \"%s\" gravada", ssid);
    httpd_resp_set_type(req, "text/html");
    httpd_resp_sendstr(req, "<!doctype html><meta charset=utf-8>"
                            "<p>Rede salva. O ClaudeMeter vai reiniciar.</p>");
    xSemaphoreGive(s_saved);
    return ESP_OK;
}

static void recovery_run() {
    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_WIFI_SOFTAP);
    char ssid[32];
    snprintf(ssid, sizeof(ssid), "ClaudeMeter-%02X%02X", mac[4], mac[5]);

    ESP_LOGW(TAG, "%d min sem Wi-Fi — SoftAP de recuperação \"%s\"",
             WIFI_RECOVERY_AFTER_MIN, ssid);
    char txt[96];
    snprintf(txt, sizeof(txt), "RECUPERACAO Wi-Fi\n%s\nhttp://192.168.4.1", ssid);
    status(txt);

    esp_wifi_disconnect();
    esp_wifi_stop();
    if (!s_ap_netif) s_ap_netif = esp_netif_create_default_wifi_ap();

    wifi_config_t ap = {};
    strncpy((char *)ap.ap.ssid, ssid, sizeof(ap.ap.ssid) - 1);
    ap.ap.ssid_len       = strlen(ssid);
    strncpy((char *)ap.ap.password, RECOVERY_AP_PASS, sizeof(ap.ap.password) - 1);
    ap.ap.authmode       = WIFI_AUTH_WPA2_PSK;
    ap.ap.channel        = 1;
    ap.ap.max_connection = 2;

    httpd_handle_t srv = nullptr;
    if (esp_wifi_set_mode(WIFI_MODE_AP) == ESP_OK &&
        esp_wifi_set_config(WIFI_IF_AP, &ap) == ESP_OK &&
        esp_wifi_start() == ESP_OK) {
        httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
        cfg.max_uri_handlers = 2;
        if (httpd_start(&srv, &cfg) == ESP_OK) {
            const httpd_uri_t get  = { .uri = "/",     .method = HTTP_GET,  .handler = page_get,  .user_ctx = nullptr };
            const httpd_uri_t save = { .uri = "/save", .method = HTTP_POST, .handler = save_post, .user_ctx = nullptr };
            httpd_register_uri_handler(srv, &get);
            httpd_register_uri_handler(srv, &save);
        }
    }

    bool saved = xSemaphoreTake(s_saved, pdMS_TO_TICKS(WIFI_RECOVERY_AP_MIN * 60 * 1000)) == pdTRUE;
    vTaskDelay(pdMS_TO_TICKS(2000));   // deixa a resposta chegar ao navegador
    if (srv) httpd_stop(srv);
    esp_wifi_stop();
    ESP_LOGW(TAG, "recuperação encerrada (%s) — reiniciando", saved ? "rede gravada" : "timeout");
}

// ════════════════════ task ═════════════════════════════════════════

static void wifi_task(void *) {
    const int64_t recovery_us = (int64_t)WIFI_RECOVERY_AFTER_MIN * 60 * 1000000;
    int64_t last_ok = esp_timer_get_time();
    int idx = 0;

    while (true) {
        if (s_connected) {
            xEventGroupWaitBits(s_events, BIT_DISCONNECTED, pdTRUE, pdFALSE, portMAX_DELAY);
            if (!s_connected) {
                ESP_LOGW(TAG, "conexão perdida");
                last_ok = esp_timer_get_time();
            }
            continue;
        }

        bool ok = false;
        for (int i = 0; i < s_net_count && !ok; i++) {
            int j = (idx + i) % s_net_count;
            if (try_connect(&s_nets[j])) { idx = j; ok = true; }
        }
        if (ok) {
            ESP_LOGI(TAG, "conectado em \"%s\"", s_nets[idx].ssid);
            last_ok = esp_timer_get_time();
            continue;
        }

        if (esp_timer_get_time() - last_ok >= recovery_us) {
            recovery_run();
            esp_restart();
        }
        vTaskDelay(pdMS_TO_TICKS(10000));
    }
}

esp_err_t wifi_mgr_start(wifi_status_cb_t on_status) {
    s_on_status = on_status;
    s_events    = xEventGroupCreate();
    s_saved     = xSemaphoreCreateBinary();
    if (!s_events || !s_saved) return ESP_ERR_NO_MEM;

    esp_err_t err;
    if ((err = esp_netif_init()) != ESP_OK) return err;
    if ((err = esp_event_loop_create_default()) != ESP_OK) return err;
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    if ((err = esp_wifi_init(&cfg)) != ESP_OK) return err;
    if ((err = esp_event_handler_instance_register(WIFI_EVENT, WIFI_EVENT_STA_DISCONNECTED,
                                                   on_event, nullptr, nullptr)) != ESP_OK) return err;
    if ((err = esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                                   on_event, nullptr, nullptr)) != ESP_OK) return err;
    if ((err = esp_wifi_set_storage(WIFI_STORAGE_RAM)) != ESP_OK) return err;
    if ((err = esp_wifi_set_mode(WIFI_MODE_STA)) != ESP_OK) return err;
    if ((err = esp_wifi_start()) != ESP_OK) return err;

    load_networks();
    if (xTaskCreate(wifi_task, "wifi", 4096, nullptr, 4, nullptr) != pdPASS) return ESP_ERR_NO_MEM;
    return ESP_OK;
}

bool wifi_mgr_connected() {
    return s_connected;
}

bool wifi_mgr_wait_connected(TickType_t wait) {
    return xEventGroupWaitBits(s_events, BIT_CONNECTED, pdFALSE, pdFALSE, wait) & BIT_CONNECTED;
}
