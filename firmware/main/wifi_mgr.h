/*
 * Wi-Fi do ClaudeMeter.
 *
 * - Tenta as redes em ordem: a gravada pela recuperação (NVS) e depois
 *   WIFI_NETWORKS do secrets.h — permite trocar senha por OTA
 *   (rede antiga + nova num firmware, troca no roteador, remove a antiga).
 * - Após WIFI_RECOVERY_AFTER_MIN sem conectar a NENHUMA rede, abre a
 *   SoftAP de recuperação "ClaudeMeter-XXXX" (WPA2, RECOVERY_AP_PASS) com
 *   um formulário em http://192.168.4.1 para gravar uma nova rede.
 *   Sem configuração em WIFI_RECOVERY_AP_MIN, reinicia e volta a tentar.
 *   A SoftAP nunca é aberta em funcionamento normal.
 */
#pragma once

#include "esp_err.h"
#include "freertos/FreeRTOS.h"

#define WIFI_RECOVERY_AFTER_MIN  15
#define WIFI_RECOVERY_AP_MIN     10

typedef void (*wifi_status_cb_t)(const char *text);   // nullptr = esconder

// Inicializa netif/event loop/Wi-Fi e cria a task de conexão.
esp_err_t wifi_mgr_start(wifi_status_cb_t on_status);

bool wifi_mgr_connected();
bool wifi_mgr_wait_connected(TickType_t wait);
