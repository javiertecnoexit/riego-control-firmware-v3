#include "net.h"

#include <Arduino.h>
#include <ArduinoJson.h>
#include <HTTPClient.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <WebSocketsClient.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/task.h>
#include <esp_task_wdt.h>
#include <string.h>
#include <time.h>

#include "config.h"
#include "hardware.h"
#include "store.h"

// ============================================================================
// net.cpp — Capa de red V3.
// Tarea dedicada (prioridad baja): WiFi STA + subida PostgREST del outbox
// (at-least-once via watermark) + WebSocket bidireccional. Nunca toca relays:
// los comandos remotos se encolan para que el loop principal los aplique.
// ============================================================================

static QueueHandle_t s_flushQueue = NULL;
static QueueHandle_t s_cmdQueue = NULL;

static volatile bool      s_configReloadPending = false;
static volatile int64_t   s_lastBackendSeenEpoch = 0;
static volatile bool      s_connEverOk = false;
static volatile uint32_t  s_netStartMs = 0;
static volatile bool      s_paused = false;   // modo CONFIGURACION: no tocar WiFi

static DeviceConfig s_cfg;                    // copia local (recargable)

// Dedupe de command_id dentro de la sesion (anillo).
static uint32_t s_executedIds[NET_COMMAND_RING_SIZE] = { 0 };
static uint8_t  s_executedIdx = 0;

static void handleCommand(JsonDocument& doc);
static void handleConfig(JsonDocument& doc);

// ----------------------------------------------------------------------------
// Parsing de URLs (host, puerto, path, scheme)
// ----------------------------------------------------------------------------
struct ParsedUrl {
    char     host[96];
    uint16_t port;
    char     path[128];
    bool     secure;
};

static bool parseUrl(const char* url, ParsedUrl& out) {
    const char* schemeEnd = strstr(url, "://");
    if (!schemeEnd) return false;
    out.secure = (strncmp(url, "https://", 8) == 0) ||
                 (strncmp(url, "wss://", 6) == 0);
    const char* hostStart = schemeEnd + 3;
    const char* pathStart = strchr(hostStart, '/');
    const char* colon = strchr(hostStart, ':');
    size_t hostLen;
    if (colon && (!pathStart || colon < pathStart)) {
        hostLen = (size_t)(colon - hostStart);
        out.port = (uint16_t)atoi(colon + 1);
    } else {
        hostLen = pathStart ? (size_t)(pathStart - hostStart) : strlen(hostStart);
        out.port = out.secure ? 443 : 80;
    }
    if (hostLen == 0 || hostLen >= sizeof(out.host)) return false;
    memcpy(out.host, hostStart, hostLen);
    out.host[hostLen] = '\0';
    if (pathStart) {
        snprintf(out.path, sizeof(out.path), "%s", pathStart);
    } else {
        out.path[0] = '\0';
    }
    return true;
}

// ----------------------------------------------------------------------------
// WiFi STA
// ----------------------------------------------------------------------------
static uint32_t s_wifiRetryAtMs = 0;

static bool wifiEnsure() {
    if (s_cfg.ssid[0] == '\0') return false;
    if (WiFi.status() == WL_CONNECTED) return true;
    // No reiniciar el intento mientras se esta conectando (el primer begin()
    // puede tardar varios segundos); solo reintentar cuando el intento previo
    // haya terminado o fallado. WL_NO_SHIELD (255) es el estado inicial.
    const uint32_t now = millis();
    if (s_wifiRetryAtMs > now) return false;
    s_wifiRetryAtMs = now + NET_WIFI_RETRY_MS;
    // Cada reintento parte de un estado limpio: sin disconnect() previo, el
    // stack WPA del ESP32 queda atascado en AUTH_FAIL/NO_AP_FOUND tras la
    // perdida del AP (observado en HIL E1). disconnect(true) descarta la
    // config/PMK cacheada y el siguiente begin() escanea de cero.
    if (WiFi.getMode() & WIFI_STA) WiFi.disconnect(true);
    WiFi.mode(WIFI_STA);
    WiFi.begin(s_cfg.ssid, s_cfg.wifiPass);
    Serial.printf("[NET] WiFi conectando a %s (estado %d)\n", s_cfg.ssid, (int)WiFi.status());
    return false;
}

// ----------------------------------------------------------------------------
// Estado de conexion inicial (P3) y contacto con el backend
// ----------------------------------------------------------------------------
static void connEvaluate() {
    const uint32_t now = millis();
    if (!s_connEverOk && (now - s_netStartMs) >= NET_CONN_EVALUATE_MS) {
        hwDisplaySetConnStatus(ConnStatus::FAIL);
    }
}

static void markBackendSeen() {
    s_connEverOk = true;
    hwDisplaySetConnStatus(ConnStatus::OK);
    s_lastBackendSeenEpoch = hwRtcTmToEpoch(hwRtcNow());
}

// ----------------------------------------------------------------------------
// Subida PostgREST (outbox -> POST /eventos)
// ----------------------------------------------------------------------------
static uint32_t s_httpBackoffAtMs = 0;
static uint32_t s_httpBackoffMs = CLOUD_BACKOFF_BASE_S * 1000UL;

static bool outboxBatch(char* body, size_t bodyCap, size_t* bodyLen,
                        uint32_t* watermark) {
    const size_t count = storeOutboxCount();
    if (count == 0) return false;
    size_t used = 1;
    body[0] = '[';
    uint32_t maxId = 0;
    char line[768];
    for (size_t i = 0; i < count && i < NET_OUTBOX_BATCH_MAX; ++i) {
        if (!storeOutboxReadLine(i, line, sizeof(line))) break;
        const size_t ll = strlen(line);
        if (ll == 0) continue;
        if (used + ll + 2 > bodyCap) break;   // + "," + "]"
        if (used > 1) body[used++] = ',';
        memcpy(body + used, line, ll);
        used += ll;
        const char* p = strstr(line, "\"client_id\":");
        if (p) {
            const uint32_t id = (uint32_t)strtoul(p + 12, NULL, 10);
            if (id > maxId) maxId = id;
        }
    }
    if (used == 1) return false;
    body[used++] = ']';
    body[used] = '\0';
    *bodyLen = used;
    *watermark = maxId;
    return true;
}

static void httpFlush() {
    if (!wifiEnsure()) return;
    const uint32_t now = millis();
    if (s_httpBackoffAtMs > now) return;

    // El lote y la URL van al heap: en la pila de la tarea de red (12 KB)
    // el body[4 KB] + HTTPClient causaban "stack smashing protect failure"
    // al drenar lotes grandes (Fase 5 HIL E9).
    char* body = (char*)malloc(NET_OUTBOX_BATCH_BYTES + 2);
    if (!body) return;
    size_t bodyLen = 0;
    uint32_t watermark = 0;
    if (!outboxBatch(body, NET_OUTBOX_BATCH_BYTES + 2, &bodyLen, &watermark)) {
        free(body);
        return;
    }

    ParsedUrl u;
    if (!parseUrl(s_cfg.apiUrl, u)) {
        free(body);
        return;
    }

    // Path final: <base>/eventos (sin doble barra).
    size_t baseLen = strlen(u.path);
    while (baseLen > 1 && u.path[baseLen - 1] == '/') baseLen--;
    char* full = (char*)malloc(256);
    if (!full) {
        free(body);
        return;
    }
    snprintf(full, 256, "%s://%s:%u%.*s/eventos",
             u.secure ? "https" : "http", u.host, u.port, (int)baseLen, u.path);

    HTTPClient http;
    WiFiClient* client = NULL;
    bool ok = false;
    if (u.secure) {
        client = new WiFiClientSecure();
        ((WiFiClientSecure*)client)->setInsecure();   // D5: TLS sin CA
        ok = http.begin(*(WiFiClientSecure*)client, full);
    } else {
        client = new WiFiClient();
        ok = http.begin(*client, full);
    }
    if (!ok) {
        Serial.printf("[NET] POST %s: no se pudo iniciar\n", full);
        delete client;
        free(full);
        free(body);
        return;
    }
    // setTimeout toma MILISEGUNDOS en arduino-esp32 3.x (en 2.x eran
    // segundos): pasar CLOUD_HTTP_TIMEOUT_MS / 1000 daba un timeout de
    // lectura de 10 ms -> READ_TIMEOUT (-11) en toda respuesta.
    http.setTimeout(CLOUD_HTTP_TIMEOUT_MS);
    http.addHeader("Content-Type", "application/json");
    http.addHeader("Prefer", "resolution=ignore-duplicates,return=minimal");
    if (s_cfg.apiKey[0] != '\0') {
        http.addHeader("apikey", s_cfg.apiKey);
        http.addHeader("Authorization", String("Bearer ") + s_cfg.apiKey);
    }

    const int code = http.POST((uint8_t*)body, bodyLen);
    http.end();
    // HTTPClient guarda una referencia al cliente pasado a begin(): el
    // llamador es quien lo libera. Sin esto se fugaba un WiFiClient por POST.
    delete client;
    free(full);
    free(body);

    if (code >= 200 && code < 300) {
        storeOutboxAckUpTo(watermark);
        s_httpBackoffMs = CLOUD_BACKOFF_BASE_S * 1000UL;
        s_httpBackoffAtMs = 0;
        markBackendSeen();
        Serial.printf("[NET] POST /eventos %d: ack hasta %lu, outbox %u\n",
                      code, (unsigned long)watermark, (unsigned)storeOutboxCount());
    } else if (code >= 400 && code < 500) {
        // Error de configuracion (tabla/columna/apikey): sin reintento del lote.
        storeOutboxAckUpTo(watermark);
        Serial.printf("[NET] POST /eventos %d: error de configuracion, lote "
                      "descartado\n", code);
    } else {
        s_httpBackoffMs = min(s_httpBackoffMs * 2, (uint32_t)CLOUD_BACKOFF_MAX_MS);
        s_httpBackoffAtMs = millis() + s_httpBackoffMs;
        Serial.printf("[NET] POST /eventos %d: backoff %lums\n", code,
                      (unsigned long)s_httpBackoffMs);
    }
}

// ----------------------------------------------------------------------------
// WebSocket (envelope {"type": ...})
// ----------------------------------------------------------------------------
static WebSocketsClient s_ws;
static bool s_wsEnabled = false;
static bool s_wsConnected = false;
static uint32_t s_lastCloudMsgMs = 0;

static void wsEvent(WStype_t type, uint8_t* payload, size_t length);

static void wsStart() {
    ParsedUrl u;
    if (s_cfg.wsUrl[0] == '\0' || !parseUrl(s_cfg.wsUrl, u)) {
        s_wsEnabled = false;
        return;
    }
    s_wsEnabled = true;
    const char* path = (u.path[0] != '\0') ? u.path : "/";
    if (u.secure) {
        s_ws.beginSSL(u.host, u.port, path);   // sin fingerprint -> setInsecure (D5)
    } else {
        s_ws.begin(u.host, u.port, path);
    }
    s_ws.setReconnectInterval(NET_WS_RECONNECT_MS);
    s_ws.onEvent(wsEvent);
    Serial.printf("[NET] WebSocket %s://%s:%u%s\n", u.secure ? "wss" : "ws",
                  u.host, u.port, path);
}

static void wsSendUpdate(uint32_t commandId, const char* status) {
    JsonDocument doc;
    doc["type"] = "command_update";
    doc["command_id"] = commandId;
    doc["status"] = status;
    String s;
    serializeJson(doc, s);
    s_ws.sendTXT(s);
}

static void wsSendConfigAck(uint32_t version, const char* status, const char* reason) {
    JsonDocument doc;
    doc["type"] = "config_ack";
    doc["version"] = version;
    doc["status"] = status;
    if (reason) doc["reason"] = reason;
    String s;
    serializeJson(doc, s);
    s_ws.sendTXT(s);
}

static void wsEvent(WStype_t type, uint8_t* payload, size_t length) {
    switch (type) {
    case WStype_CONNECTED: {
        s_wsConnected = true;
        s_lastCloudMsgMs = millis();
        JsonDocument doc;
        doc["type"] = "hello";
        doc["protocol_version"] = PROTOCOL_VERSION;
        doc["firmware_version"] = FIRMWARE_VERSION;
        doc["device_alias"] = s_cfg.deviceAlias;
        if (s_cfg.apiKey[0] != '\0') doc["apikey"] = s_cfg.apiKey;
        String s;
        serializeJson(doc, s);
        s_ws.sendTXT(s);
        Serial.println("[NET] WS conectado, hello enviado");
        break;
    }
    case WStype_DISCONNECTED:
        s_wsConnected = false;
        Serial.println("[NET] WS desconectado (reconexion automatica)");
        break;
    case WStype_ERROR:
        Serial.println("[NET] WS error");
        break;
    case WStype_TEXT: {
        s_lastCloudMsgMs = millis();
        JsonDocument doc;
        const DeserializationError err = deserializeJson(doc, (const char*)payload, length);
        if (err) {
            Serial.printf("[NET] WS mensaje no JSON: %s\n", err.c_str());
            break;
        }
        const char* typeStr = doc["type"] | "";
        if (strcmp(typeStr, "ping") == 0) {
            Serial.println("[NET] WS ping recibido, pong enviado");
            JsonDocument pong;
            pong["type"] = "pong";
            String s;
            serializeJson(pong, s);
            s_ws.sendTXT(s);
            markBackendSeen();
        } else if (strcmp(typeStr, "hello_ok") == 0) {
            Serial.println("[NET] WS hello aceptado");
            markBackendSeen();
        } else if (strcmp(typeStr, "command") == 0) {
            handleCommand(doc);
            markBackendSeen();
        } else if (strcmp(typeStr, "config") == 0) {
            handleConfig(doc);
            markBackendSeen();
        } else if (strcmp(typeStr, "close") == 0) {
            Serial.printf("[NET] WS cierre pedido por la nube: %s\n",
                          doc["reason"] | "");
        } else {
            Serial.printf("[NET] WS tipo desconocido: %s\n", typeStr);
        }
        break;
    }
    default:
        break;
    }
}

static bool commandInOutbox(uint32_t id) {
    const size_t n = storeOutboxCount();
    char line[768];
    char needle[40];
    snprintf(needle, sizeof(needle), "\"command_id\":%lu", (unsigned long)id);
    for (size_t i = 0; i < n; ++i) {
        if (storeOutboxReadLine(i, line, sizeof(line)) && strstr(line, needle)) {
            return true;
        }
    }
    return false;
}

static bool commandInRing(uint32_t id) {
    for (uint8_t i = 0; i < NET_COMMAND_RING_SIZE; ++i) {
        if (s_executedIds[i] == id) return true;
    }
    return false;
}

static void handleCommand(JsonDocument& doc) {
    const uint32_t id = doc["command_id"] | 0u;
    const char* zone = doc["zone"] | "";
    const char* action = doc["action"] | "";
    const uint16_t durationS = doc["duration_s"] | 0u;

    if (id == 0 || zone[0] == '\0') {
        wsSendUpdate(id, "rejected");
        return;
    }

    NetCommand cmd = {};
    cmd.commandId = id;
    if (zone[0] == 'S' || zone[0] == 's') {
        cmd.zoneType = 0;
    } else if (zone[0] == 'A' || zone[0] == 'a') {
        cmd.zoneType = 1;
    } else {
        wsSendUpdate(id, "rejected");
        return;
    }
    cmd.zone = (uint8_t)atoi(zone + 1);
    if (strcmp(action, "on") == 0) {
        cmd.action = 1;
    } else if (strcmp(action, "off") == 0) {
        cmd.action = 0;
    } else {
        wsSendUpdate(id, "rejected");
        return;
    }
    cmd.durationS = durationS;

    const uint8_t maxZ = (cmd.zoneType == 0) ? MAX_SUBSTRATE_ZONES
                                             : MAX_SPRINKLER_ZONES;
    if (cmd.zone >= maxZ) {
        wsSendUpdate(id, "rejected");
        return;
    }

    if (commandInRing(id) || commandInOutbox(id)) {
        wsSendUpdate(id, "accepted");   // duplicado: ya aceptado antes
        return;
    }

    // Aceptar: se persiste en el outbox ANTES de encolar (un reinicio no
    // repite el comando; el resultado llega al cloud via at-least-once).
    const uint32_t clientId = storeNextClientId();
    char line[320];
    snprintf(line, sizeof(line),
             "{\"client_id\":%lu,\"event_type\":\"command\","
             "\"command_id\":%lu,\"status\":\"accepted\",\"zone\":\"%s\","
             "\"action\":\"%s\",\"duration_s\":%u}",
             (unsigned long)clientId, (unsigned long)id, zone, action,
             durationS);
    storeOutboxAppend(line);

    s_executedIds[s_executedIdx] = id;
    s_executedIdx = (uint8_t)((s_executedIdx + 1) % NET_COMMAND_RING_SIZE);

    if (xQueueSend(s_cmdQueue, &cmd, 0) != pdTRUE) {
        Serial.printf("[NET] Cola de comandos llena: comando %lu descartado\n",
                      (unsigned long)id);
        return;
    }
    wsSendUpdate(id, "accepted");
    Serial.printf("[NET] Comando %lu aceptado: %s %s (%us)\n",
                  (unsigned long)id, zone, action, durationS);
}

static void handleConfig(JsonDocument& doc) {
    DeviceConfig cfg = s_cfg;
    cfg.version = doc["version"] | (s_cfg.version + 1u);
    // Campos omitidos: conservan el valor actual (push parcial seguro).
    strncpy(cfg.ssid, doc["ssid"] | s_cfg.ssid, sizeof(cfg.ssid) - 1);
    strncpy(cfg.wifiPass, doc["wifi_pass"] | s_cfg.wifiPass,
            sizeof(cfg.wifiPass) - 1);
    strncpy(cfg.deviceAlias, doc["device_alias"] | s_cfg.deviceAlias,
            sizeof(cfg.deviceAlias) - 1);
    strncpy(cfg.apiUrl, doc["api_url"] | s_cfg.apiUrl, sizeof(cfg.apiUrl) - 1);
    strncpy(cfg.wsUrl, doc["ws_url"] | s_cfg.wsUrl, sizeof(cfg.wsUrl) - 1);
    strncpy(cfg.apiKey, doc["api_key"] | s_cfg.apiKey, sizeof(cfg.apiKey) - 1);
    cfg.substrateZones = (uint8_t)(doc["substrate_zones"] | cfg.substrateZones);
    cfg.sprinklerZones = (uint8_t)(doc["sprinkler_zones"] | cfg.sprinklerZones);
    cfg.readIntervalS = (uint16_t)(doc["read_interval_s"] | cfg.readIntervalS);
    cfg.uploadIntervalS = (uint16_t)(doc["upload_interval_s"] | cfg.uploadIntervalS);

    char err[64];
    if (!storeConfigValidate(cfg, err, sizeof(err))) {
        Serial.printf("[NET] Config push rechazada: %s\n", err);
        wsSendConfigAck(cfg.version, "rejected", err);
        return;
    }
    if (!storeConfigApply(cfg)) {
        Serial.println("[NET] Config push no aplicada (version no superior)");
        wsSendConfigAck(cfg.version, "rejected", "version no superior");
        return;
    }
    const bool wifiChanged = strcmp(cfg.ssid, s_cfg.ssid) != 0;
    const bool wsChanged = strcmp(cfg.wsUrl, s_cfg.wsUrl) != 0;
    s_cfg = cfg;
    s_configReloadPending = true;
    if (wifiChanged) {
        WiFi.disconnect();
        s_wifiRetryAtMs = 0;
    }
    if (wsChanged) wsStart();
    wsSendConfigAck(cfg.version, "applied", NULL);
    Serial.printf("[NET] Config push aplicada v%lu\n", (unsigned long)cfg.version);
}

// ----------------------------------------------------------------------------
// Tarea de red
// ----------------------------------------------------------------------------
static void netTask(void*) {
    // Suscribe la tarea de red al watchdog de tarea: un cuelgue en la red
    // tampoco deja la placa colgada; alimenta cada iteracion.
    esp_task_wdt_add(xTaskGetCurrentTaskHandle());
    uint32_t lastLoopMs = 0;
    uint32_t lastHeapLogMs = 0;
    for (;;) {
        esp_task_wdt_reset();
        const uint32_t now = millis();
        if (s_paused) {
            vTaskDelay(100 / portTICK_PERIOD_MS);
            continue;
        }
        if ((now - lastHeapLogMs) >= 60000UL) {
            lastHeapLogMs = now;
            Serial.printf("[NET] heap libre %u B\n",
                          (unsigned)ESP.getFreeHeap());
        }
        if ((now - lastLoopMs) >= NET_TASK_LOOP_MS) {
            lastLoopMs = now;
            connEvaluate();
            if (wifiEnsure()) {
                if (s_wsEnabled) {
                    s_ws.loop();
                    if (s_wsConnected &&
                        (millis() - s_lastCloudMsgMs) > NET_WS_KEEPALIVE_MS) {
                        Serial.println("[NET] WS sin mensajes: reconexion");
                        s_ws.disconnect();
                    }
                }
                uint32_t dummy;
                while (xQueueReceive(s_flushQueue, &dummy, 0) == pdTRUE) {
                    esp_task_wdt_reset();
                    httpFlush();
                    esp_task_wdt_reset();
                    if (!wifiEnsure()) break;
                }
            }
        }
        vTaskDelay(NET_TASK_LOOP_MS / portTICK_PERIOD_MS);
    }
}

// ----------------------------------------------------------------------------
// API publica
// ----------------------------------------------------------------------------
void netInit() {
    s_cfg = storeConfigLoad();
    // No persistir credenciales/NVS en cada connect/disconnect (disconnect(true)
    // en wifiEnsure escribiria flash en cada reintento).
    WiFi.persistent(false);
    if (s_cfg.ssid[0] == '\0') {
        hwDisplaySetConnStatus(ConnStatus::FAIL);   // sin red configurada
    }
    WiFi.onEvent([](arduino_event_id_t ev, arduino_event_info_t info) {
        if (ev == ARDUINO_EVENT_WIFI_STA_CONNECTED) {
            Serial.printf("[NET] WiFi conectado a %s\n", s_cfg.ssid);
        } else if (ev == ARDUINO_EVENT_WIFI_STA_DISCONNECTED) {
            Serial.printf("[NET] WiFi desconectado, motivo %d\n",
                          info.wifi_sta_disconnected.reason);
        }
    }, ARDUINO_EVENT_WIFI_STA_CONNECTED);
    WiFi.onEvent([](arduino_event_id_t ev, arduino_event_info_t info) {
        if (ev == ARDUINO_EVENT_WIFI_STA_DISCONNECTED) {
            Serial.printf("[NET] WiFi desconectado, motivo %d\n",
                          info.wifi_sta_disconnected.reason);
        }
    }, ARDUINO_EVENT_WIFI_STA_DISCONNECTED);
    s_flushQueue = xQueueCreate(4, sizeof(uint32_t));
    s_cmdQueue = xQueueCreate(NET_COMMAND_QUEUE_LEN, sizeof(NetCommand));
    if (s_cfg.wsUrl[0] != '\0') wsStart();
    xTaskCreatePinnedToCore(netTask, "net", NET_TASK_STACK_SIZE, NULL,
                            NET_TASK_PRIORITY, NULL, 0);
    Serial.printf("[NET] Tarea de red iniciada (SSID %s, pass %u chars, WS %s)\n",
                  s_cfg.ssid[0] ? s_cfg.ssid : "(sin configurar)",
                  (unsigned)strlen(s_cfg.wifiPass),
                  s_cfg.wsUrl[0] ? "si" : "no");
}

void netRequestFlush() {
    if (!s_flushQueue) return;
    const uint32_t dummy = 0;
    xQueueSend(s_flushQueue, &dummy, 0);
}

void netSetPaused(bool paused) {
    s_paused = paused;
    if (paused) {
        s_ws.disconnect();
        WiFi.disconnect();
        WiFi.mode(WIFI_OFF);
        Serial.println("[NET] Tarea de red pausada (modo CONFIGURACION)");
    }
}

bool netTakeCommand(NetCommand* out) {
    if (!s_cmdQueue) return false;
    return xQueueReceive(s_cmdQueue, out, 0) == pdTRUE;
}

bool netConfigReloadPending() {
    return s_configReloadPending;
}

void netClearConfigReload() {
    s_configReloadPending = false;
}

int64_t netLastBackendSeenEpoch() {
    return s_lastBackendSeenEpoch;
}