/*
 * Cliente HTTPS para watch.jimmylab.com.br — configuração única para
 * /usage e OTA: CA bundle do ESP-IDF, sem seguir redirects (um 302 do
 * Cloudflare Access é falha, nunca dado) e headers do Service Token.
 */
#pragma once

#include "esp_http_client.h"
#include "freertos/FreeRTOS.h"

// Chamar uma vez no app_main, antes de criar as tasks de rede.
void watch_http_init();

// Cria o cliente já com TLS, User-Agent e headers CF-Access-*.
esp_http_client_handle_t watch_http_client(const char *url,
                                           http_event_handle_cb cb = nullptr,
                                           void *user_data = nullptr);

// Serializa o uso da rede entre /usage e OTA.
bool watch_http_lock(TickType_t wait);
void watch_http_unlock();
