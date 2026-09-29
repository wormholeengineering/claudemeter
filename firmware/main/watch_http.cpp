#include "watch_http.h"

#include <cstdio>

#include "freertos/semphr.h"
#include "esp_app_desc.h"
#include "esp_crt_bundle.h"

#include "app_config.h"

static SemaphoreHandle_t s_lock;
static char s_user_agent[64];

void watch_http_init() {
    s_lock = xSemaphoreCreateMutex();
    snprintf(s_user_agent, sizeof(s_user_agent), "ClaudeMeter-ESP32/%s",
             esp_app_get_description()->version);
}

esp_http_client_handle_t watch_http_client(const char *url,
                                           http_event_handle_cb cb,
                                           void *user_data) {
    esp_http_client_config_t cfg = {};
    cfg.url                   = url;
    cfg.timeout_ms            = HTTP_TIMEOUT_MS;
    cfg.event_handler         = cb;
    cfg.user_data             = user_data;
    cfg.crt_bundle_attach     = esp_crt_bundle_attach;   // CA bundle do ESP-IDF
    cfg.user_agent            = s_user_agent;
    cfg.disable_auto_redirect = true;
    cfg.buffer_size           = 2048;   // headers do Cloudflare podem passar de 512 B
    cfg.buffer_size_tx        = 1024;

    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client) return nullptr;

    if (CF_ACCESS_CLIENT_ID[0] != '\0') {
        esp_http_client_set_header(client, "CF-Access-Client-Id",     CF_ACCESS_CLIENT_ID);
        esp_http_client_set_header(client, "CF-Access-Client-Secret", CF_ACCESS_CLIENT_SECRET);
    }
    return client;
}

bool watch_http_lock(TickType_t wait) {
    return xSemaphoreTake(s_lock, wait) == pdTRUE;
}

void watch_http_unlock() {
    xSemaphoreGive(s_lock);
}
