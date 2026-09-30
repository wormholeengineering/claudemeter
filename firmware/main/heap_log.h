/*
 * Telemetria de memória: heap livre, mínimo histórico e maior bloco livre
 * (fragmentação). Só números — nada de secrets ou dados pessoais.
 */
#pragma once

#include "esp_heap_caps.h"
#include "esp_log.h"

static inline void heap_log(const char *tag, const char *where) {
    ESP_LOGI(tag, "heap [%s]: livre=%u min=%u maior_bloco=%u", where,
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_8BIT),
             (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_8BIT),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
}
