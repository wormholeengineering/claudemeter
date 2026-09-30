#include "ota.h"

#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_app_desc.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_random.h"
#include "esp_system.h"
#include "nvs.h"
#include "cJSON.h"
#include "mbedtls/sha256.h"

#include "app_config.h"
#include "heap_log.h"
#include "watch_http.h"

static const char *TAG = "ota";

#define NVS_NS       "ota"
#define NVS_BAD_VER  "bad_ver"   // versão que já sofreu rollback — não baixar de novo

static ota_hooks_t s_hooks;
static bool        s_pending = false;

typedef struct {
    char version[32];
    char path[96];
    char sha256[65];
    int  size;
} manifest_t;

typedef enum { CHECK_DONE, CHECK_RETRY } check_result_t;

// ════════════════════ helpers ══════════════════════════════════════

static void status(const char *text) {
    if (s_hooks.on_status) s_hooks.on_status(text);
}

// "MAJOR.MINOR.PATCH" estrito (só dígitos e pontos)
static bool parse_semver(const char *s, int v[3]) {
    if (!s[0] || strspn(s, "0123456789.") != strlen(s)) return false;
    int n = 0;
    if (sscanf(s, "%d.%d.%d%n", &v[0], &v[1], &v[2], &n) != 3) return false;
    return s[n] == '\0';
}

// <0 se a<b, 0 se igual, >0 se a>b
static int semver_cmp(const int a[3], const int b[3]) {
    for (int i = 0; i < 3; i++)
        if (a[i] != b[i]) return a[i] < b[i] ? -1 : 1;
    return 0;
}

static bool nvs_get_bad_ver(char *out, size_t sz) {
    nvs_handle_t h;
    out[0] = '\0';
    if (nvs_open(NVS_NS, NVS_READONLY, &h) != ESP_OK) return false;
    esp_err_t err = nvs_get_str(h, NVS_BAD_VER, out, &sz);
    nvs_close(h);
    return err == ESP_OK;
}

static void nvs_set_bad_ver(const char *ver) {
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_str(h, NVS_BAD_VER, ver);
    nvs_commit(h);
    nvs_close(h);
}

static void to_hex(const uint8_t *in, size_t n, char *out) {
    for (size_t i = 0; i < n; i++) sprintf(out + 2 * i, "%02x", in[i]);
    out[2 * n] = '\0';
}

// ════════════════════ boot / rollback ══════════════════════════════

void ota_boot_init() {
    const esp_partition_t *running = esp_ota_get_running_partition();
    esp_ota_img_states_t st;
    s_pending = esp_ota_get_state_partition(running, &st) == ESP_OK &&
                st == ESP_OTA_IMG_PENDING_VERIFY;

    ESP_LOGI(TAG, "firmware %s em %s%s", esp_app_get_description()->version,
             running->label, s_pending ? " (PENDING_VERIFY)" : "");

    // Imagem que falhou (rollback) — lembra a versão para não baixá-la em loop
    const esp_partition_t *invalid = esp_ota_get_last_invalid_partition();
    esp_app_desc_t desc;
    if (invalid && esp_ota_get_partition_description(invalid, &desc) == ESP_OK) {
        char bad[32];
        nvs_get_bad_ver(bad, sizeof(bad));
        if (strcmp(bad, desc.version) != 0) {
            ESP_LOGW(TAG, "rollback detectado: versão %s rejeitada", desc.version);
            nvs_set_bad_ver(desc.version);
        }
    }
}

bool ota_pending_verify() {
    return s_pending;
}

void ota_selftest_result(bool healthy, const char *reason) {
    if (!s_pending) return;

    if (healthy) {
        esp_err_t err = esp_ota_mark_app_valid_cancel_rollback();
        ESP_LOGI(TAG, "autoteste OK — imagem marcada válida (%s)", esp_err_to_name(err));
        s_pending = false;
        return;
    }

    ESP_LOGE(TAG, "autoteste FALHOU (%s) — rollback; imagem NÃO marcada válida", reason);
    ESP_LOGW(TAG, "esp_ota_mark_app_invalid_rollback_and_reboot() — voltando à imagem anterior");
    esp_ota_mark_app_invalid_rollback_and_reboot();
    // Só retorna se não existir imagem anterior válida: segue rodando esta.
    ESP_LOGE(TAG, "sem imagem anterior para rollback — mantendo a atual");
}

// ════════════════════ manifest ═════════════════════════════════════

static bool fetch_manifest(char *buf, int max) {
    esp_http_client_handle_t c = watch_http_client(MANIFEST_URL);
    if (!c) return false;

    bool ok = false;
    esp_err_t err = esp_http_client_open(c, 0);
    if (err == ESP_OK) {
        esp_http_client_fetch_headers(c);
        int status_code = esp_http_client_get_status_code(c);
        if (status_code == 200) {
            int total = 0, r;
            while (total < max - 1 &&
                   (r = esp_http_client_read(c, buf + total, max - 1 - total)) > 0)
                total += r;
            buf[total] = '\0';
            ok = total > 0 && esp_http_client_is_complete_data_received(c);
            if (!ok) ESP_LOGW(TAG, "manifest incompleto ou maior que %d B", max - 1);
        } else {
            ESP_LOGW(TAG, "manifest: HTTP %d", status_code);
        }
    } else {
        ESP_LOGW(TAG, "manifest: %s", esp_err_to_name(err));
    }
    esp_http_client_close(c);
    esp_http_client_cleanup(c);
    return ok;
}

static bool valid_firmware_path(const char *p) {
    size_t pre = strlen(FIRMWARE_PATH_PREFIX);
    if (strncmp(p, FIRMWARE_PATH_PREFIX, pre) != 0) return false;   // só mesmo host
    const char *name = p + pre;
    if (!name[0] || strstr(name, "..")) return false;
    for (const char *c = name; *c; c++)
        if (!isalnum((unsigned char)*c) && *c != '.' && *c != '_' && *c != '-') return false;
    return true;
}

static bool parse_manifest(const char *json, manifest_t *m) {
    cJSON *root = cJSON_Parse(json);
    if (!root) return false;

    const cJSON *project = cJSON_GetObjectItemCaseSensitive(root, "project");
    const cJSON *version = cJSON_GetObjectItemCaseSensitive(root, "version");
    const cJSON *url     = cJSON_GetObjectItemCaseSensitive(root, "url");
    const cJSON *sha     = cJSON_GetObjectItemCaseSensitive(root, "sha256");
    const cJSON *size    = cJSON_GetObjectItemCaseSensitive(root, "size");

    bool ok = cJSON_IsString(project) && cJSON_IsString(version) &&
              cJSON_IsString(url) && cJSON_IsString(sha) && cJSON_IsNumber(size) &&
              strcmp(project->valuestring, esp_app_get_description()->project_name) == 0 &&
              strlen(version->valuestring) < sizeof(m->version) &&
              strlen(url->valuestring) < sizeof(m->path) &&
              strlen(sha->valuestring) == 64 &&
              size->valuedouble > 0 && size->valuedouble < 16 * 1024 * 1024;

    if (ok) {
        strcpy(m->version, version->valuestring);
        strcpy(m->path, url->valuestring);
        for (int i = 0; i < 64; i++) {
            char ch = (char)tolower((unsigned char)sha->valuestring[i]);
            if (!isxdigit((unsigned char)ch)) ok = false;
            m->sha256[i] = ch;
        }
        m->sha256[64] = '\0';
        m->size = (int)size->valuedouble;
        int v[3];
        ok = ok && parse_semver(m->version, v) && valid_firmware_path(m->path);
    }
    cJSON_Delete(root);
    return ok;
}

// ════════════════════ download + verificação ═══════════════════════

static bool download_and_install(const manifest_t *m) {
    const esp_partition_t *part = esp_ota_get_next_update_partition(nullptr);
    if (!part) { ESP_LOGE(TAG, "sem partição OTA livre"); return false; }
    if ((uint32_t)m->size > part->size) {
        ESP_LOGE(TAG, "imagem (%d B) maior que %s (%lu B)", m->size, part->label,
                 (unsigned long)part->size);
        return false;
    }

    char url[sizeof(WATCH_BASE_URL) + sizeof(m->path)];
    snprintf(url, sizeof(url), "%s%s", WATCH_BASE_URL, m->path);

    ESP_LOGI(TAG, "gravando em %s (0x%lx, %lu B)", part->label,
             (unsigned long)part->address, (unsigned long)part->size);

    esp_http_client_handle_t c = watch_http_client(url);
    if (!c) return false;

    bool ok = false;
    esp_ota_handle_t ota = 0;
    bool ota_open = false;
    mbedtls_sha256_context sha;
    mbedtls_sha256_init(&sha);
    uint8_t *buf = (uint8_t *)malloc(4096);

    do {
        if (!buf) break;
        esp_err_t err = esp_http_client_open(c, 0);
        if (err != ESP_OK) { ESP_LOGW(TAG, "download: %s", esp_err_to_name(err)); break; }
        int64_t len = esp_http_client_fetch_headers(c);
        int status_code = esp_http_client_get_status_code(c);
        if (status_code != 200) { ESP_LOGW(TAG, "download: HTTP %d", status_code); break; }
        if (len > 0 && len != m->size) {
            ESP_LOGW(TAG, "download: Content-Length %lld ≠ manifest %d", len, m->size);
            break;
        }

        err = esp_ota_begin(part, OTA_WITH_SEQUENTIAL_WRITES, &ota);
        if (err != ESP_OK) { ESP_LOGE(TAG, "esp_ota_begin: %s", esp_err_to_name(err)); break; }
        ota_open = true;
        mbedtls_sha256_starts(&sha, 0);

        int total = 0, last_pct = -1, r;
        bool io_ok = true;
        while ((r = esp_http_client_read(c, (char *)buf, 4096)) > 0) {
            total += r;
            if (total > m->size) { io_ok = false; break; }
            mbedtls_sha256_update(&sha, buf, r);
            if (esp_ota_write(ota, buf, r) != ESP_OK) { io_ok = false; break; }
            int pct = (int)((int64_t)total * 100 / m->size);
            if (pct / 5 != last_pct / 5) {
                last_pct = pct;
                char txt[24];
                snprintf(txt, sizeof(txt), "UPDATE %d%%", pct);
                status(txt);
            }
        }
        if (!io_ok || r < 0 || total != m->size) {
            ESP_LOGW(TAG, "download interrompido (%d/%d B)", total, m->size);
            break;
        }
        ESP_LOGI(TAG, "download completo: %d B", total);

        // 1) SHA-256 do arquivo inteiro == manifest
        uint8_t digest[32];
        char hex[65];
        mbedtls_sha256_finish(&sha, digest);
        to_hex(digest, sizeof(digest), hex);
        if (strcmp(hex, m->sha256) != 0) {
            ESP_LOGE(TAG, "SHA-256 diverge do manifest");
            break;
        }
        ESP_LOGI(TAG, "SHA-256 confere com o manifest");

        // 2) esp_ota_end valida a imagem (checksum, hash embutido, assinatura RSA)
        ota_open = false;
        err = esp_ota_end(ota);
        if (err != ESP_OK) { ESP_LOGE(TAG, "imagem rejeitada: %s", esp_err_to_name(err)); break; }
        ESP_LOGI(TAG, "imagem validada por esp_ota_end (assinatura RSA OK)");

        // 3) descrição da imagem gravada == manifest
        esp_app_desc_t desc;
        if (esp_ota_get_partition_description(part, &desc) != ESP_OK ||
            strcmp(desc.project_name, esp_app_get_description()->project_name) != 0 ||
            strcmp(desc.version, m->version) != 0) {
            ESP_LOGE(TAG, "esp_app_desc não confere com o manifest");
            break;
        }
        ESP_LOGI(TAG, "esp_app_desc confere: %s %s", desc.project_name, desc.version);

        err = esp_ota_set_boot_partition(part);
        if (err != ESP_OK) { ESP_LOGE(TAG, "set_boot_partition: %s", esp_err_to_name(err)); break; }
        ESP_LOGI(TAG, "partição de boot → %s", part->label);
        ok = true;
    } while (false);

    if (ota_open) esp_ota_abort(ota);
    mbedtls_sha256_free(&sha);
    free(buf);
    esp_http_client_close(c);
    esp_http_client_cleanup(c);
    return ok;
}

// ════════════════════ ciclo de verificação ═════════════════════════

static check_result_t ota_check_once() {
    if (s_hooks.can_update && !s_hooks.can_update()) return CHECK_RETRY;
    if (!watch_http_lock(pdMS_TO_TICKS(60000))) return CHECK_RETRY;

    check_result_t result = CHECK_DONE;
    manifest_t m = {};
    char *json = (char *)malloc(OTA_MANIFEST_MAX_BYTES);

    do {
        if (!json) { result = CHECK_RETRY; break; }
        if (!fetch_manifest(json, OTA_MANIFEST_MAX_BYTES)) { result = CHECK_RETRY; break; }
        if (!parse_manifest(json, &m)) { ESP_LOGW(TAG, "manifest inválido — ignorado"); break; }

        int cur[3], avail[3];
        const char *running = esp_app_get_description()->version;
        if (!parse_semver(running, cur)) { ESP_LOGE(TAG, "versão local inválida: %s", running); break; }
        parse_semver(m.version, avail);

        if (semver_cmp(avail, cur) <= 0) {
            ESP_LOGI(TAG, "atualizado (%s; servidor %s)", running, m.version);
            break;
        }
        char bad[32];
        if (nvs_get_bad_ver(bad, sizeof(bad)) && strcmp(bad, m.version) == 0) {
            ESP_LOGW(TAG, "versão %s já sofreu rollback — ignorada", m.version);
            break;
        }

        ESP_LOGI(TAG, "atualizando %s → %s (%d B)", running, m.version, m.size);
        heap_log(TAG, "antes do OTA");
        status("UPDATE 0%");
        bool installed = download_and_install(&m);
        heap_log(TAG, installed ? "OTA concluído" : "OTA falhou");
        if (installed) {
            status("UPDATE OK");
            ESP_LOGI(TAG, "reiniciando na versão %s", m.version);
            vTaskDelay(pdMS_TO_TICKS(1500));
            esp_restart();
        }
        status(nullptr);
        result = CHECK_RETRY;
    } while (false);

    free(json);
    watch_http_unlock();
    return result;
}

static void ota_task(void *) {
    vTaskDelay(pdMS_TO_TICKS(OTA_FIRST_CHECK_SEC * 1000));
    while (true) {
        int delay_s = OTA_RETRY_SEC;
        if (ota_check_once() == CHECK_DONE) {
            int jitter = (int)(esp_random() % (2 * OTA_JITTER_SEC + 1)) - OTA_JITTER_SEC;
            delay_s = OTA_INTERVAL_SEC + jitter;
        }
        vTaskDelay(pdMS_TO_TICKS((uint32_t)delay_s * 1000));
    }
}

const char *ota_running_summary() {
    static char buf[40];
    const esp_partition_t *running = esp_ota_get_running_partition();
    esp_ota_img_states_t st;
    const char *state = "?";
    if (esp_ota_get_state_partition(running, &st) == ESP_OK) {
        state = st == ESP_OTA_IMG_VALID          ? "VALID"          :
                st == ESP_OTA_IMG_PENDING_VERIFY ? "PENDING_VERIFY" :
                st == ESP_OTA_IMG_UNDEFINED      ? "UNDEFINED"      :
                st == ESP_OTA_IMG_NEW            ? "NEW"            :
                st == ESP_OTA_IMG_INVALID        ? "INVALID"        : "ABORTED";
    } else {
        state = "sem otadata";
    }
    snprintf(buf, sizeof(buf), "%s %s", running->label, state);
    return buf;
}

void ota_start(const ota_hooks_t *hooks) {
    s_hooks = *hooks;
    xTaskCreate(ota_task, "ota", 8192, nullptr, 3, nullptr);
}
