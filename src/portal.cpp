#include "portal.h"

#include <Arduino.h>
#include <DNSServer.h>
#include <WebServer.h>
#include <WiFi.h>
#include <esp_random.h>

#include <stdio.h>
#include <string.h>

#include "config.h"
#include "hardware.h"
#include "store.h"

// ============================================================================
// portal.cpp — Portal cautivo V3 (P1). AP + DNS + HTTP.
// Pagina principal simple: WiFi (con redes disponibles detectadas), alias y
// zonas. Configuraciones avanzadas (URL API, URL WS, apikey, tiempos) en una
// pagina aparte, accesible solo desde un enlace, para evitar cambios
// accidentales. Toda ruta responde 200 con el portal (auto-apertura del
// navegador en telefonos). Calibracion de humedad y restablecimiento a
// fabrica en paginas dedicadas.
// ============================================================================

static WebServer s_server(80);
static DNSServer s_dns;
static bool s_readyToReboot = false;

// ----------------------------------------------------------------------------
// Escaneo de redes WiFi (async, cacheado; sin bloquear el servidor)
// ----------------------------------------------------------------------------
#define MAX_SCAN_RESULTS 20

static char s_networks[MAX_SCAN_RESULTS][33];
static uint8_t s_networkCount = 0;
static bool s_scanInProgress = false;

static void scanStart() {
    s_scanInProgress = true;
    WiFi.scanNetworks(true);   // asincronico
}

static void scanPoll() {
    if (!s_scanInProgress) return;
    const int16_t n = WiFi.scanComplete();
    if (n < 0) return;   // aun escaneando (o fallo: se reintenta al recargar)
    s_scanInProgress = false;

    uint8_t count = 0;
    for (int16_t i = 0; i < n && count < MAX_SCAN_RESULTS; ++i) {
        const String ssid = WiFi.SSID(i);
        if (ssid.length() > 0) {
            strncpy(s_networks[count], ssid.c_str(), sizeof(s_networks[0]) - 1);
            s_networks[count][sizeof(s_networks[0]) - 1] = '\0';
            count++;
        }
    }
    s_networkCount = count;
    WiFi.scanDelete();
}

// ----------------------------------------------------------------------------
// HTML
// ----------------------------------------------------------------------------
static void htmlEscape(const char* in, char* out, size_t cap) {
    size_t o = 0;
    for (size_t i = 0; in[i] != '\0' && o + 6 < cap; ++i) {
        const char c = in[i];
        switch (c) {
            case '&': memcpy(out + o, "&amp;", 5); o += 5; break;
            case '<': memcpy(out + o, "&lt;", 4); o += 4; break;
            case '>': memcpy(out + o, "&gt;", 4); o += 4; break;
            case '"': memcpy(out + o, "&quot;", 6); o += 6; break;
            default:  out[o++] = c; break;
        }
    }
    out[o] = '\0';
}

static const char* PAGE_HEAD =
    "<!DOCTYPE html><html lang=\"es\"><head><meta charset=\"utf-8\">"
    "<meta name=\"viewport\" content=\"width=device-width, initial-scale=1\">"
    "<title>Riego Control</title><style>"
    "body{font-family:sans-serif;max-width:560px;margin:16px auto;padding:0 12px;"
    "color:#222}label{display:block;margin-top:10px;font-weight:bold}"
    "input,select{width:100%;box-sizing:border-box;padding:8px;margin-top:4px}"
    "button{margin-top:18px;padding:10px;width:100%;font-size:16px;border:0;"
    "color:#fff;background:#1a7f37;cursor:pointer}"
    "button.danger{background:#d1242f}a{color:#1a7f37}.note{color:#666;font-size:13px}"
    ".err{border:1px solid #d1242f;background:#fdecec;padding:10px}"
    ".links{margin-top:14px;font-size:14px;line-height:1.8}"
    "table{width:100%;border-collapse:collapse}td,th{border:1px solid #ccc;"
    "padding:6px;text-align:center}</style></head><body>";

static const char* PAGE_FOOT = "</body></html>";

static String optionsHtml(uint8_t min, uint8_t max, uint8_t selected) {
    String html;
    for (uint8_t v = min; v <= max; ++v) {
        html += "<option value=\"" + String(v) + "\"";
        if (v == selected) html += " selected";
        html += ">" + String(v) + "</option>";
    }
    return html;
}

static String esc(const char* value) {
    char buf[300];
    htmlEscape(value ? value : "", buf, sizeof(buf));
    return String(buf);
}

static String networksDatalistHtml() {
    String html = "<datalist id=\"redes\">";
    for (uint8_t i = 0; i < s_networkCount; ++i) {
        html += "<option value=\"" + esc(s_networks[i]) + "\">";
    }
    html += "</datalist>";
    return html;
}

static void sendResult(const char* title, const char* message, bool ok) {
    String page = PAGE_HEAD;
    page += String(ok ? "<h1>" : "<h1 class=\"err\">") + title
            + "</h1><p>" + message + "</p>";
    page += "<p><a href=\"/\">Volver a la configuracion</a></p>";
    page += PAGE_FOOT;
    s_server.send(200, "text/html; charset=utf-8", page);
}

// ----------------------------------------------------------------------------
// Pagina principal (WiFi + alias + zonas)
// ----------------------------------------------------------------------------
static String renderIndexPage() {
    DeviceConfig cfg = storeConfigLoad();
    if (!storeHasValidConfig()) cfg = storeConfigDefaults();

    String page = PAGE_HEAD;
    page += "<h1>Riego Control</h1>"
            "<p>Configuracion basica del dispositivo. Al guardar, el equipo "
            "se reinicia.</p>";
    page += "<form method=\"post\" action=\"/save\">";

    page += "<label>Red WiFi</label><input name=\"ssid\" required maxlength=\"32\" "
            "list=\"redes\" value=\"" + esc(cfg.ssid) + "\">"
            + networksDatalistHtml();
    page += "<span class=\"note\">Las redes disponibles se completan solas "
            "mientras escribe; si no aparece, escríbala manualmente.</span>";

    page += "<label>Password de la red</label><input name=\"wifiPass\" maxlength=\"64\" "
            "type=\"password\" value=\"" + esc(cfg.wifiPass) + "\">";
    page += "<label>Alias del dispositivo (opcional)</label><input name=\"deviceAlias\" "
            "maxlength=\"32\" value=\"" + esc(cfg.deviceAlias) + "\">";

    page += "<p><b>Zonas activas:</b> " + String(cfg.substrateZones)
            + " de sustrato / " + String(cfg.sprinklerZones)
            + " de aspiracion.<br>"
            "<span class=\"note\">La cantidad de zonas activas se configura en "
            "la pagina avanzada (afecta la inicializacion de sensores).</span></p>";

    if (!storeHasValidConfig()) {
        page += "<p class=\"err\">Primera configuracion: revise la pagina "
                "avanzada (URL de la API que le entregue el desarrollador y "
                "credenciales, si las usa) y luego guarde aqui.</p>";
    }

    page += "<button type=\"submit\">Guardar y reiniciar</button></form>";

    page += "<div class=\"links\">"
            "<a href=\"/avanzado\">Configuracion avanzada (zonas activas, URL "
            "API, WebSocket, apikey, tiempos)</a><br>"
            "<a href=\"/calibrar\">Calibracion de humedad por zona</a><br>"
            "<a href=\"/reset\">Restablecer valores de fabrica</a><br>"
            "<a href=\"/status\">Estado (JSON)</a></div>";
    page += PAGE_FOOT;
    return page;
}

static void handleIndex() {
    s_server.send(200, "text/html; charset=utf-8", renderIndexPage());
}

static void handleSave() {
    DeviceConfig cfg = storeConfigLoad();
    if (!storeHasValidConfig()) cfg = storeConfigDefaults();
    cfg.version++;

    strncpy(cfg.ssid, s_server.arg("ssid").c_str(), sizeof(cfg.ssid) - 1);
    strncpy(cfg.wifiPass, s_server.arg("wifiPass").c_str(), sizeof(cfg.wifiPass) - 1);
    strncpy(cfg.deviceAlias, s_server.arg("deviceAlias").c_str(), sizeof(cfg.deviceAlias) - 1);

    char err[128] = "";
    if (!storeConfigValidate(cfg, err, sizeof(err))) {
        sendResult("Configuracion invalida", err, false);
        return;
    }
    if (!storeConfigApply(cfg)) {
        sendResult("No se pudo aplicar", "La version de configuracion no es "
                   "superior a la vigente. Intente nuevamente.", false);
        return;
    }
    s_readyToReboot = true;
    sendResult("Guardado", "Configuracion aplicada. Reiniciando...", true);
}

// ----------------------------------------------------------------------------
// Pagina avanzada (URL API, URL WS, apikey, tiempos) — solo por enlace
// ----------------------------------------------------------------------------
static String renderAvanzadoPage() {
    DeviceConfig cfg = storeConfigLoad();
    if (!storeHasValidConfig()) cfg = storeConfigDefaults();

    String page = PAGE_HEAD;
    page += "<h1>Configuracion avanzada</h1>"
            "<p>Cambie estos parametros solo si sabe lo que hace. Al guardar, "
            "el equipo se reinicia.</p>";
    page += "<form method=\"post\" action=\"/avanzado\">";

    page += "<label>Zonas activas de sustrato</label><select name=\"substrateZones\">"
            + optionsHtml(1, MAX_SUBSTRATE_ZONES, cfg.substrateZones) + "</select>"
            "<span class=\"note\">Cada zona activa tiene su sensor de humedad "
            "de suelo; influye en la inicializacion de sensores.</span>";
    page += "<label>Zonas activas de aspiracion</label><select name=\"sprinklerZones\">"
            + optionsHtml(0, MAX_SPRINKLER_ZONES, cfg.sprinklerZones) + "</select>";

    page += "<label>URL de la API (PostgREST)</label><input name=\"apiUrl\" required "
            "maxlength=\"128\" value=\"" + esc(cfg.apiUrl) + "\">"
            "<span class=\"note\">Valor por defecto pre-cargado.</span>";
    page += "<label>URL del WebSocket (opcional)</label><input name=\"wsUrl\" "
            "maxlength=\"128\" value=\"" + esc(cfg.wsUrl) + "\">"
            "<span class=\"note\">Vacio = sin canal WebSocket.</span>";
    page += "<label>apikey (opcional)</label><input name=\"apiKey\" "
            "maxlength=\"128\" value=\"" + esc(cfg.apiKey) + "\">"
            "<span class=\"note\">Solo si su backend la usa (ej. Supabase). "
            "Con otra tecnologia puede dejarse vacia.</span>";
    page += "<label>Tiempo de lectura (seg)</label><input name=\"readIntervalS\" "
            "type=\"number\" min=\"5\" max=\"3600\" value=\""
            + String(cfg.readIntervalS) + "\">";
    page += "<label>Tiempo de subida (seg)</label><input name=\"uploadIntervalS\" "
            "type=\"number\" min=\"10\" max=\"3600\" value=\""
            + String(cfg.uploadIntervalS) + "\">";

    page += "<button type=\"submit\">Guardar y reiniciar</button></form>";
    page += "<p><a href=\"/\">Volver a la configuracion basica</a></p>";
    page += PAGE_FOOT;
    return page;
}

static void handleAvanzado() {
    s_server.send(200, "text/html; charset=utf-8", renderAvanzadoPage());
}

static void handleAvanzadoSave() {
    DeviceConfig cfg = storeConfigLoad();
    if (!storeHasValidConfig()) cfg = storeConfigDefaults();
    cfg.version++;

    strncpy(cfg.apiUrl, s_server.arg("apiUrl").c_str(), sizeof(cfg.apiUrl) - 1);
    strncpy(cfg.wsUrl, s_server.arg("wsUrl").c_str(), sizeof(cfg.wsUrl) - 1);
    strncpy(cfg.apiKey, s_server.arg("apiKey").c_str(), sizeof(cfg.apiKey) - 1);
    cfg.substrateZones = (uint8_t)s_server.arg("substrateZones").toInt();
    cfg.sprinklerZones = (uint8_t)s_server.arg("sprinklerZones").toInt();
    cfg.readIntervalS = (uint16_t)s_server.arg("readIntervalS").toInt();
    cfg.uploadIntervalS = (uint16_t)s_server.arg("uploadIntervalS").toInt();

    char err[128] = "";
    if (!storeConfigValidate(cfg, err, sizeof(err))) {
        sendResult("Configuracion invalida", err, false);
        return;
    }
    if (!storeConfigApply(cfg)) {
        sendResult("No se pudo aplicar", "La version de configuracion no es "
                   "superior a la vigente. Intente nuevamente.", false);
        return;
    }
    s_readyToReboot = true;
    sendResult("Guardado", "Configuracion avanzada aplicada. Reiniciando...", true);
}

// ----------------------------------------------------------------------------
// Calibracion de humedad (viva)
// ----------------------------------------------------------------------------
static void handleCalibrar() {
    const DeviceConfig cfg = storeConfigLoad();
    const uint8_t activeZones = storeHasValidConfig()
                                    ? cfg.substrateZones
                                    : DEFAULT_SUBSTRATE_ZONES;
    if (activeZones == 0) {
        sendResult("Sin zonas activas",
                   "No hay zonas de sustrato activas para calibrar. "
                   "Configurelas en la pagina avanzada.", false);
        return;
    }

    String page = PAGE_HEAD;
    page += "<h1>Calibracion de humedad</h1>"
            "<p>Coloque la sonda de la zona en el medio a medir y presione el "
            "boton correspondiente. Cada zona de sustrato activa tiene su "
            "propio sensor de humedad. La lectura viva se actualiza sola.</p>"
            "<meta http-equiv=\"refresh\" content=\"2\">"
            "<table><tr><th>Zona</th><th>ADC vivo</th><th>Seco</th>"
            "<th>Humedo</th><th>Estado</th></tr>";
    for (uint8_t z = 0; z < activeZones; ++z) {
        const uint16_t adc = hwSensorsReadSoilRawAdc(z);
        const ZoneCalibration cal = hwCalibrationGet(z);
        page += "<tr><td>" + String(z + 1) + "</td><td>" + String(adc)
                + "</td><td>" + String(cal.dryRaw) + "</td><td>"
                + String(cal.wetRaw) + "</td><td>"
                + String(cal.calibrated ? "OK" : "PENDIENTE") + "</td></tr>";
    }
    page += "</table>";
    for (uint8_t z = 0; z < activeZones; ++z) {
        page += "<form method=\"post\" action=\"/calibrar\" style=\"display:inline\">"
                "<input type=\"hidden\" name=\"zone\" value=\"" + String(z) + "\">"
                "<button type=\"submit\" name=\"point\" value=\"dry\">Zona "
                + String(z + 1) + ": SECO</button>"
                "<button type=\"submit\" name=\"point\" value=\"wet\">Zona "
                + String(z + 1) + ": HUMEDO</button></form>";
    }
    page += "<p><a href=\"/\">Volver</a></p>";
    page += PAGE_FOOT;
    s_server.send(200, "text/html; charset=utf-8", page);
}

static void handleCalibrarSet() {
    const uint8_t zone = (uint8_t)s_server.arg("zone").toInt();
    const String point = s_server.arg("point");
    const DeviceConfig cfg = storeConfigLoad();
    const uint8_t activeZones = storeHasValidConfig()
                                    ? cfg.substrateZones
                                    : DEFAULT_SUBSTRATE_ZONES;
    if (zone >= activeZones || (point != "dry" && point != "wet")) {
        sendResult("Error", "Zona de calibracion invalida o no activa.", false);
        return;
    }
    ZoneCalibration cal = hwCalibrationGet(zone);
    const uint16_t adc = hwSensorsReadSoilRawAdc(zone);
    if (point == "dry") {
        cal.dryRaw = adc;
    } else {
        cal.wetRaw = adc;
    }
    cal.calibrated = (cal.dryRaw > cal.wetRaw);
    hwCalibrationSet(zone, cal);
    s_server.sendHeader("Location", "/calibrar");
    s_server.send(302, "text/html", "");
}

// ----------------------------------------------------------------------------
// Restablecimiento a fabrica
// ----------------------------------------------------------------------------
static void handleReset() {
    String page = PAGE_HEAD;
    page += "<h1 class=\"err\">Restablecer valores de fabrica</h1>"
            "<p>Se borraran: configuracion, calibracion, snapshots y outbox "
            "de eventos. Esta accion NO se puede deshacer.</p>"
            "<form method=\"post\" action=\"/reset\">"
            "<button class=\"danger\" type=\"submit\">Confirmar restablecimiento</button>"
            "</form><p><a href=\"/\">Cancelar</a></p>";
    page += PAGE_FOOT;
    s_server.send(200, "text/html; charset=utf-8", page);
}

static void handleResetDo() {
    storeFactoryReset();
    hwCalibrationClearAll();
    s_readyToReboot = true;
    sendResult("Restablecido", "Valores de fabrica restaurados. Reiniciando...", true);
}

// ----------------------------------------------------------------------------
// Estado (JSON)
// ----------------------------------------------------------------------------
static void handleStatus() {
    DeviceConfig cfg = storeConfigLoad();
    const bool valid = storeHasValidConfig();
    String json = "{";
    json += "\"valid\":" + String(valid ? "true" : "false") + ",";
    json += "\"version\":" + String(cfg.version) + ",";
    json += "\"ssid\":\"" + esc(cfg.ssid) + "\",";
    json += "\"alias\":\"" + esc(cfg.deviceAlias) + "\",";
    json += "\"substrateZones\":" + String(cfg.substrateZones) + ",";
    json += "\"sprinklerZones\":" + String(cfg.sprinklerZones) + ",";
    json += "\"readIntervalS\":" + String(cfg.readIntervalS) + ",";
    json += "\"uploadIntervalS\":" + String(cfg.uploadIntervalS) + ",";
    json += "\"outboxLines\":" + String((unsigned long)storeOutboxCount()) + ",";
    json += "\"outboxBytes\":" + String((unsigned long)storeOutboxUsedBytes());
    json += "}";
    s_server.send(200, "application/json", json);
}

// ----------------------------------------------------------------------------
// Cautivo: TODA ruta responde 200 con el portal (auto-apertura del navegador
// en telefonos; los probes de Apple/Android no reciben 302 sino contenido).
// ----------------------------------------------------------------------------
static void handleNotFound() {
    s_server.send(200, "text/html; charset=utf-8", renderIndexPage());
}

// ----------------------------------------------------------------------------
// API publica
// ----------------------------------------------------------------------------
void portalStart() {
    char ssid[32];
    snprintf(ssid, sizeof(ssid), "%s%04u", AP_SSID_PREFIX,
             (unsigned)(esp_random() % 10000u));

    WiFi.mode(WIFI_AP);
    WiFi.softAP(ssid, AP_PASSWORD);
    delay(200);

    s_dns.start(53, "*", IPAddress(192, 168, 4, 1));

    s_server.on("/", HTTP_GET, handleIndex);
    s_server.on("/save", HTTP_POST, handleSave);
    s_server.on("/avanzado", HTTP_GET, handleAvanzado);
    s_server.on("/avanzado", HTTP_POST, handleAvanzadoSave);
    s_server.on("/calibrar", HTTP_GET, handleCalibrar);
    s_server.on("/calibrar", HTTP_POST, handleCalibrarSet);
    s_server.on("/reset", HTTP_GET, handleReset);
    s_server.on("/reset", HTTP_POST, handleResetDo);
    s_server.on("/status", HTTP_GET, handleStatus);
    s_server.on("/scan", HTTP_GET, []() {
        scanStart();
        s_server.sendHeader("Location", "/", true);
        s_server.send(302, "text/html", "");
    });

    // Sondas de deteccion de portal cautivo (Android, iOS, Windows): se
    // responde 200 con el portal para que el sistema operativo abra el
    // navegador automaticamente al conectarse al AP.
    const char* probePaths[] = {
        "/generate_204", "/hotspot-detect.html", "/success.txt",
        "/ncsi.txt", "/canonical.html", "/library/test/success.html",
        "/connecttest.txt", "/gen_204", "/favicon.ico"
    };
    for (size_t i = 0; i < sizeof(probePaths) / sizeof(probePaths[0]); ++i) {
        s_server.on(probePaths[i], HTTP_GET, handleNotFound);
    }
    s_server.onNotFound(handleNotFound);
    s_server.begin();

    scanStart();

    Serial.printf("[PORTAL] AP %s en 192.168.4.1\n", ssid);
}

void portalHandle() {
    s_dns.processNextRequest();
    s_server.handleClient();
    scanPoll();
}

bool portalSavedAndReadyToReboot() {
    return s_readyToReboot;
}