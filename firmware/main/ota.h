/*
 * FOTA do ClaudeMeter — ESP-IDF OTA nativo (esp_ota_ops) + rollback.
 *
 *   GET WATCH_BASE_URL/firmware/manifest.json
 *     {"project":"claudemeter","version":"1.0.1",
 *      "url":"/firmware/claudemeter-1.0.1.bin","size":1409024,"sha256":"<hex>"}
 *   GET WATCH_BASE_URL/firmware/claudemeter-1.0.1.bin
 *
 * Só baixa versão MAIOR que a instalada. Antes de trocar a partição de boot:
 * tamanho e SHA-256 do arquivo iguais ao manifest, esp_ota_end() aceita a
 * imagem (inclusive assinatura RSA) e o esp_app_desc bate com o manifest.
 */
#pragma once

typedef struct {
    bool (*can_update)();                  // Wi-Fi conectado, bateria OK...
    void (*on_status)(const char *text);   // banner no display; nullptr = esconder
} ota_hooks_t;

// Logo após nvs_flash_init(): registra estado e versão rejeitada em rollback.
void ota_boot_init();

// true se esta imagem chegou por OTA e ainda não foi validada.
bool ota_pending_verify();

// Resultado do autoteste LOCAL. Só tem efeito com ota_pending_verify():
// healthy → marca válida; senão → marca inválida e reinicia na anterior.
void ota_selftest_result(bool healthy, const char *reason);

// Inicia a verificação periódica do manifest (após o autoteste).
void ota_start(const ota_hooks_t *hooks);
