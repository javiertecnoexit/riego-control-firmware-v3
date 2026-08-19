#include "store.h"

#include <stdio.h>
#include <string.h>

#include "config.h"

// ============================================================================
// store.cpp — Persistencia V3.
// Backend doble: archivos planos en el host (pruebas nativas, simula
// reinicios) y NVS + LittleFS en la placa. Sin Arduino en el backend de
// archivos para que los tests Unity compilen en host.
// ============================================================================

#if defined(STORE_BACKEND_FILE)
#include <sys/stat.h>
#if defined(_WIN32)
#include <direct.h>
#endif
#define STORE_LOCK()   ((void)0)
#define STORE_UNLOCK() ((void)0)
#else
#include <Arduino.h>
#include <LittleFS.h>
#include <Preferences.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
// La tarea de red (net) y el loop principal comparten el outbox: se protege
// cada operacion con un mutex (creado en storeInit).
static SemaphoreHandle_t s_storeMutex = NULL;
static void storeLock() {
    if (s_storeMutex) xSemaphoreTake(s_storeMutex, portMAX_DELAY);
}
static void storeUnlock() {
    if (s_storeMutex) xSemaphoreGive(s_storeMutex);
}
#endif

static const uint32_t SCHEMA_VERSION = STORE_CFG_SCHEMA_VERSION;

// ----------------------------------------------------------------------------
// CRC32 (simple, suficiente para detectar corrupcion)
// ----------------------------------------------------------------------------
static uint32_t crc32(const void* data, size_t len) {
    const uint8_t* p = static_cast<const uint8_t*>(data);
    uint32_t crc = 0xFFFFFFFFu;
    for (size_t i = 0; i < len; ++i) {
        crc ^= p[i];
        for (int b = 0; b < 8; ++b) {
            crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
        }
    }
    return ~crc;
}

// ----------------------------------------------------------------------------
// Backend de configuracion (blob = [crc4][schema4][struct])
// ----------------------------------------------------------------------------
#if defined(STORE_BACKEND_FILE)

static char s_dir[128] = "store_data";

static void blobPath(char* out, size_t cap, const char* name) {
    snprintf(out, cap, "%s/%s", s_dir, name);
}

static bool blobWrite(const char* name, const DeviceConfig& cfg) {
    char path[160];
    blobPath(path, sizeof(path), name);
    uint8_t blob[sizeof(uint32_t) * 2 + sizeof(DeviceConfig)];
    const uint32_t crc = crc32(&cfg, sizeof(cfg));
    memcpy(blob, &crc, 4);
    const uint32_t schema = SCHEMA_VERSION;
    memcpy(blob + 4, &schema, 4);
    memcpy(blob + 8, &cfg, sizeof(cfg));
    FILE* f = fopen(path, "wb");
    if (!f) return false;
    const size_t written = fwrite(blob, 1, sizeof(blob), f);
    fclose(f);
    return written == sizeof(blob);
}

static bool blobRead(const char* name, DeviceConfig& cfg) {
    char path[160];
    blobPath(path, sizeof(path), name);
    uint8_t blob[sizeof(uint32_t) * 2 + sizeof(DeviceConfig)];
    FILE* f = fopen(path, "rb");
    if (!f) return false;
    const size_t n = fread(blob, 1, sizeof(blob), f);
    fclose(f);
    if (n != sizeof(blob)) return false;
    uint32_t crc, schema;
    memcpy(&crc, blob, 4);
    memcpy(&schema, blob + 4, 4);
    if (schema != SCHEMA_VERSION) return false;
    memcpy(&cfg, blob + 8, sizeof(cfg));
    return crc32(&cfg, sizeof(cfg)) == crc;
}

#else  // STORE_BACKEND_NVS (placa)

static Preferences s_prefs;
static const char* NVS_NS = "cfg";

static bool blobWrite(const char* name, const DeviceConfig& cfg) {
    uint8_t blob[sizeof(uint32_t) * 2 + sizeof(DeviceConfig)];
    const uint32_t crc = crc32(&cfg, sizeof(cfg));
    memcpy(blob, &crc, 4);
    const uint32_t schema = SCHEMA_VERSION;
    memcpy(blob + 4, &schema, 4);
    memcpy(blob + 8, &cfg, sizeof(cfg));
    storeLock();
    s_prefs.begin(NVS_NS, false);
    const size_t n = s_prefs.putBytes(name, blob, sizeof(blob));
    s_prefs.end();
    storeUnlock();
    return n == sizeof(blob);
}

static bool blobRead(const char* name, DeviceConfig& cfg) {
    uint8_t blob[sizeof(uint32_t) * 2 + sizeof(DeviceConfig)];
    storeLock();
    s_prefs.begin(NVS_NS, true);
    const size_t n = s_prefs.getBytes(name, blob, sizeof(blob));
    s_prefs.end();
    storeUnlock();
    if (n != sizeof(blob)) return false;
    uint32_t crc, schema;
    memcpy(&crc, blob, 4);
    memcpy(&schema, blob + 4, 4);
    if (schema != SCHEMA_VERSION) return false;
    memcpy(&cfg, blob + 8, sizeof(cfg));
    return crc32(&cfg, sizeof(cfg)) == crc;
}

#endif

// ----------------------------------------------------------------------------
// Backend de outbox (archivo JSONL)
// ----------------------------------------------------------------------------
#if defined(STORE_BACKEND_FILE)

static void outboxPath(char* out, size_t cap) {
    snprintf(out, cap, "%s/outbox.jsonl", s_dir);
}

static bool outboxReadAll(char* buf, size_t cap, size_t* outLen, size_t* outCount) {
    char path[160];
    outboxPath(path, sizeof(path));
    FILE* f = fopen(path, "rb");
    if (!f) return true;   // archivo inexistente = outbox vacio
    const size_t n = fread(buf, 1, cap - 1, f);
    fclose(f);
    buf[n] = '\0';
    *outLen = n;
    size_t count = 0;
    for (size_t i = 0; i < n; ++i) {
        if (buf[i] == '\n') count++;
    }
    *outCount = count;
    return true;
}

static bool outboxWriteAll(const char* buf, size_t len) {
    char path[160];
    outboxPath(path, sizeof(path));
    FILE* f = fopen(path, "wb");
    if (!f) return false;
    const size_t written = fwrite(buf, 1, len, f);
    fclose(f);
    return written == len;
}

static bool outboxFileSize(size_t* outSize) {
    char path[160];
    outboxPath(path, sizeof(path));
    FILE* f = fopen(path, "rb");
    if (!f) {
        *outSize = 0;
        return true;
    }
    fseek(f, 0, SEEK_END);
    *outSize = (size_t)ftell(f);
    fclose(f);
    return true;
}

#else  // LittleFS (placa)

static const char* OUTBOX_PATH = "/outbox.jsonl";

static bool outboxReadAll(char* buf, size_t cap, size_t* outLen, size_t* outCount) {
    storeLock();
    const bool exists = LittleFS.exists(OUTBOX_PATH);
    bool ok = true;
    size_t n = 0;
    if (exists) {
        File f = LittleFS.open(OUTBOX_PATH, "r");
        if (f) {
            n = f.readBytes(buf, cap - 1);
            f.close();
        } else {
            ok = false;
        }
    }
    storeUnlock();
    if (!ok) return false;
    buf[n] = '\0';
    *outLen = n;
    size_t count = 0;
    for (size_t i = 0; i < n; ++i) {
        if (buf[i] == '\n') count++;
    }
    *outCount = count;
    return true;
}

static bool outboxWriteAll(const char* buf, size_t len) {
    storeLock();
    File f = LittleFS.open(OUTBOX_PATH, "w");
    bool ok = false;
    if (f) {
        ok = f.write((const uint8_t*)buf, len) == len;
        f.close();
    }
    storeUnlock();
    return ok;
}

static bool outboxFileSize(size_t* outSize) {
    storeLock();
    bool ok = true;
    size_t size = 0;
    if (LittleFS.exists(OUTBOX_PATH)) {
        File f = LittleFS.open(OUTBOX_PATH, "r");
        if (f) {
            size = (size_t)f.size();
            f.close();
        } else {
            ok = false;
        }
    }
    storeUnlock();
    if (!ok) return false;
    *outSize = size;
    return true;
}

#endif

// ----------------------------------------------------------------------------
// Configuracion — snapshots
// ----------------------------------------------------------------------------
DeviceConfig storeConfigDefaults() {
    DeviceConfig cfg = {};
    cfg.version = 0;
    cfg.substrateZones = DEFAULT_SUBSTRATE_ZONES;
    cfg.sprinklerZones = DEFAULT_SPRINKLER_ZONES;
    cfg.readIntervalS = DEFAULT_READ_INTERVAL_S;
    cfg.uploadIntervalS = DEFAULT_UPLOAD_INTERVAL_S;
    strncpy(cfg.apiUrl, DEFAULT_API_URL, sizeof(cfg.apiUrl) - 1);
    strncpy(cfg.wsUrl, DEFAULT_WS_URL, sizeof(cfg.wsUrl) - 1);
    return cfg;
}

static bool validUrl(const char* url, const char* httpPrefix, const char* wsPrefix) {
    const size_t n = strlen(url);
    if (n == 0) return (wsPrefix != NULL);   // wsUrl puede estar vacio
    if (n > 127) return false;
    if (httpPrefix && strncmp(url, httpPrefix, strlen(httpPrefix)) == 0) return true;
    if (wsPrefix && strncmp(url, wsPrefix, strlen(wsPrefix)) == 0) return true;
    return false;
}

bool storeConfigValidate(const DeviceConfig& cfg, char* err, size_t errLen) {
    if (cfg.ssid[0] == '\0') {
        snprintf(err, errLen, "SSID de WiFi obligatorio");
        return false;
    }
    if (cfg.substrateZones < 1 || cfg.substrateZones > MAX_SUBSTRATE_ZONES) {
        snprintf(err, errLen, "Zonas de sustrato fuera de rango (1..%d)",
                 MAX_SUBSTRATE_ZONES);
        return false;
    }
    if (cfg.sprinklerZones > MAX_SPRINKLER_ZONES) {
        snprintf(err, errLen, "Zonas de aspiracion fuera de rango (0..%d)",
                 MAX_SPRINKLER_ZONES);
        return false;
    }
    if (cfg.readIntervalS < 5 || cfg.readIntervalS > 3600) {
        snprintf(err, errLen, "Tiempo de lectura fuera de rango (5..3600 s)");
        return false;
    }
    if (cfg.uploadIntervalS < 10 || cfg.uploadIntervalS > 3600) {
        snprintf(err, errLen, "Tiempo de subida fuera de rango (10..3600 s)");
        return false;
    }
    if (!validUrl(cfg.apiUrl, "https://", NULL) &&
        !validUrl(cfg.apiUrl, "http://", NULL)) {
        snprintf(err, errLen, "URL de API debe ser http(s)://");
        return false;
    }
    if (!validUrl(cfg.wsUrl, NULL, "wss://") &&
        !validUrl(cfg.wsUrl, NULL, "ws://")) {
        snprintf(err, errLen, "URL de WebSocket debe ser ws(s):// o vacia");
        return false;
    }
    // La apikey es OPCIONAL: si el backend del desarrollador usa otra
    // tecnologia (API abierta, otro header de autenticacion), el firmware
    // funciona igual; solo envia headers de auth si la clave esta seteada.
    if (errLen > 0) err[0] = '\0';
    return true;
}

static bool cfgIsValid(const DeviceConfig& cfg) {
    char err[64];
    return storeConfigValidate(cfg, err, sizeof(err));
}

bool storeHasValidConfig() {
    DeviceConfig cfg;
    if (blobRead("cur", cfg) && cfgIsValid(cfg)) return true;
    if (blobRead("prev", cfg) && cfgIsValid(cfg)) return true;
    return false;
}

DeviceConfig storeConfigLoad() {
    DeviceConfig cfg;
    if (blobRead("cur", cfg) && cfgIsValid(cfg)) return cfg;
    if (blobRead("prev", cfg) && cfgIsValid(cfg)) return cfg;
    return storeConfigDefaults();
}

bool storeConfigApply(const DeviceConfig& cfg) {
    if (!cfgIsValid(cfg)) return false;

    DeviceConfig cur;
    if (blobRead("cur", cur) && cfgIsValid(cur)) {
        if (cfg.version <= cur.version) return false;   // version no regresiva
        blobWrite("prev", cur);
    }
    return blobWrite("cur", cfg);
}

// ----------------------------------------------------------------------------
// Outbox
// ----------------------------------------------------------------------------
static uint32_t lineClientId(const char* line, size_t len) {
    static const char needle[] = "\"client_id\":";
    const size_t needleLen = sizeof(needle) - 1;
    for (size_t i = 0; i + needleLen <= len; ++i) {
        if (memcmp(line + i, needle, needleLen) == 0) {
            uint32_t id = 0;
            size_t j = i + needleLen;
            while (j < len && line[j] >= '0' && line[j] <= '9') {
                id = id * 10u + (uint32_t)(line[j] - '0');
                j++;
            }
            return id;
        }
    }
    return 0;
}

static bool trimOutbox(char* buf, size_t len, size_t count) {
    if (len <= STORE_OUTBOX_MAX_BYTES && count <= STORE_OUTBOX_MAX_LINES) {
        return true;
    }
    // Conservar las ultimas STORE_OUTBOX_MAX_LINES lineas dentro del limite.
    const char* keep = buf + len;
    size_t kept = 0;
    for (size_t i = len; i > 0; --i) {
        if (buf[i - 1] == '\n') {
            kept++;
            keep = buf + i;
            if (kept >= STORE_OUTBOX_MAX_LINES) break;
        }
    }
    const size_t keepLen = (size_t)((buf + len) - keep);
    if (keepLen > STORE_OUTBOX_MAX_BYTES) {
        keep = buf + len - STORE_OUTBOX_MAX_BYTES;
        while (keep < buf + len && *keep != '\n') keep++;   // no cortar linea
        if (keep < buf + len) keep++;
    }
    return outboxWriteAll(keep, (size_t)((buf + len) - keep));
}

bool storeOutboxAppend(const char* line) {
    char buf[STORE_OUTBOX_MAX_BYTES + 1];
    size_t len = 0, count = 0;
    if (!outboxReadAll(buf, sizeof(buf), &len, &count)) return false;

    const size_t lineLen = strlen(line);
    if (len + lineLen + 1 > sizeof(buf)) {
        if (!trimOutbox(buf, len, count)) return false;
        if (!outboxReadAll(buf, sizeof(buf), &len, &count)) return false;
    }
    if (len + lineLen + 1 > sizeof(buf)) return false;

    memcpy(buf + len, line, lineLen);
    len += lineLen;
    buf[len++] = '\n';
    if (!outboxWriteAll(buf, len)) return false;

    if (len > STORE_OUTBOX_MAX_BYTES || count + 1 > STORE_OUTBOX_MAX_LINES) {
        trimOutbox(buf, len, count + 1);
    }
    return true;
}

size_t storeOutboxCount() {
    char buf[STORE_OUTBOX_MAX_BYTES + 1];
    size_t len = 0, count = 0;
    if (!outboxReadAll(buf, sizeof(buf), &len, &count)) return 0;
    return count;
}

bool storeOutboxAckUpTo(uint32_t watermark) {
    char buf[STORE_OUTBOX_MAX_BYTES + 1];
    size_t len = 0, count = 0;
    if (!outboxReadAll(buf, sizeof(buf), &len, &count)) return false;
    if (count == 0) return true;

    // Reescribir solo las lineas con client_id > watermark.
    char out[STORE_OUTBOX_MAX_BYTES + 1];
    size_t outLen = 0;
    size_t start = 0;
    for (size_t i = 0; i <= len; ++i) {
        if (i == len || buf[i] == '\n') {
            const size_t lineLen = i - start;
            if (lineLen > 0) {
                const uint32_t id = lineClientId(buf + start, lineLen);
                if (id > watermark) {
                    if (outLen + lineLen + 1 > sizeof(out)) return false;
                    memcpy(out + outLen, buf + start, lineLen);
                    outLen += lineLen;
                    out[outLen++] = '\n';
                }
            }
            start = i + 1;
        }
    }
    return outboxWriteAll(out, outLen);
}

void storeOutboxClear() {
    outboxWriteAll("", 0);
}

size_t storeOutboxUsedBytes() {
    size_t size = 0;
    outboxFileSize(&size);
    return size;
}

bool storeOutboxReadLine(size_t index, char* buf, size_t cap) {
    char all[STORE_OUTBOX_MAX_BYTES + 1];
    size_t len = 0, count = 0;
    if (!outboxReadAll(all, sizeof(all), &len, &count)) return false;
    if (index >= count) return false;
    size_t start = 0;
    size_t found = 0;
    for (size_t i = 0; i <= len && found <= index; ++i) {
        if (i == len || all[i] == '\n') {
            if (found == index) {
                const size_t lineLen = i - start;
                if (lineLen == 0 || lineLen >= cap) return false;
                memcpy(buf, all + start, lineLen);
                buf[lineLen] = '\0';
                return true;
            }
            found++;
            start = i + 1;
        }
    }
    return false;
}

// ----------------------------------------------------------------------------
// Watermark de client_id (monotono entre reinicios)
// ----------------------------------------------------------------------------
#if defined(STORE_BACKEND_FILE)

static uint32_t wmarkRead() {
    char path[160];
    blobPath(path, sizeof(path), "wmark");
    uint32_t v = 0;
    FILE* f = fopen(path, "rb");
    if (!f) return 0;
    const size_t n = fread(&v, 1, 4, f);
    fclose(f);
    return (n == 4) ? v : 0;
}

static bool wmarkWrite(uint32_t v) {
    char path[160];
    blobPath(path, sizeof(path), "wmark");
    FILE* f = fopen(path, "wb");
    if (!f) return false;
    const size_t n = fwrite(&v, 1, 4, f);
    fclose(f);
    return n == 4;
}

#else

static uint32_t wmarkRead() {
    storeLock();
    s_prefs.begin(NVS_NS, true);
    const uint32_t v = s_prefs.getUInt("wmark", 0);
    s_prefs.end();
    storeUnlock();
    return v;
}

static bool wmarkWrite(uint32_t v) {
    storeLock();
    s_prefs.begin(NVS_NS, false);
    const bool ok = s_prefs.putUInt("wmark", v);
    s_prefs.end();
    storeUnlock();
    return ok;
}

#endif

uint32_t storeNextClientId() {
    uint32_t v = wmarkRead();
    const uint32_t next = (v == UINT32_MAX) ? 1u : v + 1u;
    if (!wmarkWrite(next)) return wmarkRead() + 1u;   // reintento unico
    return next;
}

// ----------------------------------------------------------------------------
// Inicializacion y restablecimiento
// ----------------------------------------------------------------------------
void storeInit(const char* storagePath) {
#if defined(STORE_BACKEND_FILE)
    if (storagePath) {
        strncpy(s_dir, storagePath, sizeof(s_dir) - 1);
        s_dir[sizeof(s_dir) - 1] = '\0';
    }
#if defined(_WIN32)
    _mkdir(s_dir);
#else
    mkdir(s_dir, 0755);
#endif
#else
    // Particion "spiffs" (subtipo spiffs) de partitions/no_ota_with_littlefs.csv.
    // formatOnFail=true: formatea si el primer montaje encuentra basura (particion
    // nueva); el outbox es una cola tolerante a perdida.
    LittleFS.begin(true, "/littlefs", 10, "spiffs");
    if (!s_storeMutex) s_storeMutex = xSemaphoreCreateMutex();
#endif
}

void storeFactoryReset() {
#if defined(STORE_BACKEND_FILE)
    char path[160];
    blobPath(path, sizeof(path), "cur");
    remove(path);
    blobPath(path, sizeof(path), "prev");
    remove(path);
    blobPath(path, sizeof(path), "wmark");
    remove(path);
    outboxWriteAll("", 0);
#else
    storeLock();
    s_prefs.begin(NVS_NS, false);
    s_prefs.clear();
    s_prefs.end();
    LittleFS.remove(OUTBOX_PATH);
    storeUnlock();
#endif
}