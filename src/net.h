#ifndef NET_H
#define NET_H

#include <stdbool.h>
#include <stdint.h>

// ============================================================================
// net.h — Capa de red V3 (estructura reducida, P8; Fase 3).
// Tarea dedicada (FreeRTOS, prioridad baja): WiFi STA, subida PostgREST del
// outbox (watermark) y WebSocket bidireccional (hello, ping/pong, comandos,
// config push). La tarea JAMAS toca relays: los comandos se entregan al loop
// principal via cola y este los aplica en su evaluacion.
// ============================================================================

// Comando remoto recibido por WebSocket (downlink).
struct NetCommand {
    uint32_t commandId;   // idempotencia (persistido antes de ejecutar)
    uint8_t  zoneType;    // 0 = sustrato, 1 = aspiracion
    uint8_t  zone;        // indice 0-based
    uint8_t  action;      // 0 = off, 1 = on
    uint16_t durationS;   // relevante solo si action = on
};

// setup(): crea la tarea de red (lee la configuracion de store).
void netInit();

// Pausa/reanuda la tarea: en modo CONFIGURACION debe quedar pausada para que
// la tarea no revierta el AP del portal a WIFI_STA. El reinicio la reanuda.
void netSetPaused(bool paused);

// Pide una subida del outbox (non-blocking). El loop principal la llama cada
// uploadIntervalS.
void netRequestFlush();

// Drena un comando del cloud; false si no hay ninguno. Solo el loop principal
// debe llamarla.
bool netTakeCommand(NetCommand* out);

// true si la nube empujo una configuracion nueva que el control debe recargar.
bool netConfigReloadPending();
void netClearConfigReload();

// Epoch (s) del ultimo contacto exitoso con el backend (alarma de no-conexion).
int64_t netLastBackendSeenEpoch();

#endif // NET_H