# Arquitectura (Resumen Del Plan)

Fuente normativa: `PLAN_V3.md` (secciones 6 y 7). Este documento es el resumen
operativo; ante discrepancia, prevalece el plan.

## Firmware

- **Tarea de control** (loop principal): lectura local configurable (default
  30 s), `serviceTimeouts` en cada pasada, watchdog. Autoridad sobre los
  relays.
- **Tarea de red** (FreeRTOS, prioridad baja): WiFi, subida PostgREST,
  WebSocket, reconexion con backoff. JAMAS toca GPIO; se comunica con el
  control via cola de mensajes.
- **Modulos**: `hal` (hardware), `domain` (reglas puras, base V2), `runtime`
  (deadlines/concurrencia), `persist` (snapshots + outbox), `cloud`
  (PostgREST + WebSocket), `portal` (AP de configuracion).
- **Persistencia**: NVS `cfg` (configuracion operativa y de portal);
  snapshots `config_current`/`config_previous` (politica cloud completa);
  outbox LittleFS 64 KB (eventos y comandos pendientes).
- **Boot deterministico**: relays OFF -> snapshot valido -> RTC/sensores ->
  evaluacion local -> tarea de red. Sin ventana de defaults.

## Cloud (Responsabilidad Del Equipo De Integracion)

- **Subida (uplink)**: POST JSON a PostgREST
  (`https://<proyecto>.supabase.co/rest/v1/<tabla>`) con headers `apikey` y
  `Authorization: Bearer <apikey>`, RLS desactivada. Retry con backoff
  30 s -> 30 min; los 4xx indican error de configuracion (sin reintento).
- **Canal (downlink)**: WebSocket persistente `wss://<endpoint>`; `hello` con
  `apikey` y `protocol_version`; la nube envia `command`/`config` en cualquier
  momento; keep-alive por `ping`/`pong`; reconexion 5 s -> 5 min.
- **Idempotencia**: watermark `client_id` en subida; `command_id` persistido
  antes de ejecutar.

## Linderos

| Elemento | Responsable |
|---|---|
| Firmware (todo) | Este repositorio |
| Tablas PostgREST, RLS, apikey | Equipo cloud |
| Endpoint WebSocket | Equipo cloud |
| Dashboard/frontend | Equipo cloud |
| Suite de conformidad | Este repositorio (la ejecuta el equipo cloud) |