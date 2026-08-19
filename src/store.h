#ifndef STORE_H
#define STORE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// ============================================================================
// store.h — Persistencia V3 (estructura reducida, P8).
// Configuracion en NVS con snapshots dobles (config_current/previous) con
// checksum, y outbox de eventos en LittleFS (JSONL). En pruebas nativas usa
// archivos (STORE_BACKEND_FILE) para simular reinicios; en placa usa NVS y
// LittleFS. Sin dependencias de Arduino para poder probarse en host.
// ============================================================================

#define STORE_CFG_SCHEMA_VERSION 1u

// Limites del outbox (particion LittleFS de 64 KB).
#define STORE_OUTBOX_MAX_BYTES   (32u * 1024u)
#define STORE_OUTBOX_MAX_LINES   512u

struct DeviceConfig {
    uint32_t version;              // monotona; la nube la marca (Fase 3)
    char     ssid[33];
    char     wifiPass[65];
    char     deviceAlias[33];
    char     apiUrl[129];          // base PostgREST (P6), default pre-cargado (D12)
    char     wsUrl[129];           // endpoint WebSocket (P4), puede estar vacio
    char     apiKey[129];
    uint8_t  substrateZones;       // 1..MAX_SUBSTRATE_ZONES
    uint8_t  sprinklerZones;       // 0..MAX_SPRINKLER_ZONES
    uint16_t readIntervalS;        // ciclo local de lectura
    uint16_t uploadIntervalS;      // intervalo de subida (Fase 3)
};

// ----------------------------------------------------------------------------
// Configuracion — snapshots dobles
// ----------------------------------------------------------------------------
DeviceConfig storeConfigDefaults();
bool storeConfigValidate(const DeviceConfig& cfg, char* err, size_t errLen);

// Boot: current -> previous -> defaults. Nunca deja estado invalido.
DeviceConfig storeConfigLoad();
bool storeHasValidConfig();

// Valida y persiste como current (el anterior pasa a previous). Rechaza
// versiones no superiores a la vigente. Devuelve false si la config no es
// aplicable (el current queda intacto).
bool storeConfigApply(const DeviceConfig& cfg);

// ----------------------------------------------------------------------------
// Outbox de eventos (JSONL) — at-least-once via watermark
// ----------------------------------------------------------------------------
// Cada linea debe contener "client_id":<n> para el acuse (Fase 3).
bool     storeOutboxAppend(const char* line);
size_t   storeOutboxCount();
bool     storeOutboxAckUpTo(uint32_t watermark);
void     storeOutboxClear();
size_t   storeOutboxUsedBytes();

// ----------------------------------------------------------------------------
// General
// ----------------------------------------------------------------------------
// Inicializa el backend (monta LittleFS en placa; fija el directorio en
// nativo). Idempotente. En placa storagePath se ignora (puede ser NULL).
void storeInit(const char* storagePath);

// Borra config, snapshots y outbox completos (restablecimiento a fabrica).
void storeFactoryReset();

#endif // STORE_H