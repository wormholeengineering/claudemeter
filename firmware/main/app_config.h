/*
 * Configuração compartilhada do ClaudeMeter (sem secrets).
 * Secrets ficam em secrets.h (fora do Git) — copie secrets.h.example.
 */
#pragma once

#if !__has_include("secrets.h")
#error "Crie firmware/main/secrets.h a partir de secrets.h.example"
#endif
#include "secrets.h"

// ─── Servidor ──────────────────────────────────────────────────────
#ifndef WATCH_BASE_URL
#define WATCH_BASE_URL     "https://watch.jimmylab.com.br"
#endif
#define USAGE_URL          WATCH_BASE_URL "/usage"
#define MANIFEST_URL       WATCH_BASE_URL "/firmware/manifest.json"
#define FIRMWARE_PATH_PREFIX "/firmware/"

#define HTTP_TIMEOUT_MS    15000   // handshake TLS no C3 leva alguns segundos
#define REFRESH_SEC        60

// ─── Cloudflare Access Service Token (vazio = não envia) ──────────
#ifndef CF_ACCESS_CLIENT_ID
#define CF_ACCESS_CLIENT_ID     ""
#endif
#ifndef CF_ACCESS_CLIENT_SECRET
#define CF_ACCESS_CLIENT_SECRET ""
#endif

// ─── OTA ───────────────────────────────────────────────────────────
#define OTA_FIRST_CHECK_SEC     120            // após o firmware se marcar válido
#define OTA_INTERVAL_SEC        (6 * 3600)
#define OTA_JITTER_SEC          (30 * 60)      // ± aleatório
#define OTA_RETRY_SEC           (30 * 60)      // quando pulado (bateria, rede)
#define OTA_MIN_BATTERY_PCT     30             // ou carregando
#define OTA_MANIFEST_MAX_BYTES  1024

// ─── Autoteste da imagem nova (rollback) ──────────────────────────
// Só falhas LOCAIS rejeitam a imagem; rede/Internet/servidor apenas
// são registrados no log.
#define SELFTEST_STABLE_SEC     60
