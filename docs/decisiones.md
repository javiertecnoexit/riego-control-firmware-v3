# Registro De Decisiones

Regla: cada decision se registra aqui con su estado. Las decisiones del
usuario (prioridades P1-P6) preceden a cualquier decision anterior en caso de
conflicto.

## Decisiones Del Usuario (Vigentes)

| # | Decision | Estado |
|---|---|---|
| P1 | Portal cautivo con restablecimiento a fabrica y configuraciones avanzadas (URL API por defecto, zonas, tiempos de lectura y subida) | Aprobada |
| P2 | Primer pulso del selector: participa en el flujo sin cambiar la pantalla | Aprobada |
| P3 | Indicador en pantalla de exito/fallo de la conexion inicial | Aprobada |
| P4 | WebSocket como protocolo de comunicacion (bidireccionalidad) | Aprobada |
| P5 | Seguridad minima; NO usar direcciones MAC | Aprobada (reemplaza registro por MAC) |
| P6 | Subida simple y directa: POST JSON a PostgREST, RLS desactivada, header `apikey` | Aprobada (reemplaza modelo de Edge Function) |

## Decisiones De Diseno (Vigentes)

| # | Decision | Estado |
|---|---|---|
| D1 | WebSocket bidireccional persistente + unico POST HTTP de registro | **Reemplazada por P5/P6**: sin registro; subida PostgREST directa |
| D2 | Seguridad minima, sin complicaciones | Aprobada y superada por P5 |
| D3 | Outbox critica en LittleFS (particion dedicada 64 KB) | Aprobada (19/08/2026) |
| D4 | Registro automatico por MAC | **Reemplazada por P5** (no usar MAC) |
| D5 | TLS/HTTPS sin verificar CA (igual que V2), documentado como limite | Aprobada (19/08/2026) |
| D6 | Repositorio nuevo privado `riego-control-firmware-v3` (rama `main`) | Aprobada (19/08/2026) |
| D7 | `lib/domain` de V2 como base: copiar con revision menor y cambios documentados | Aprobada (19/08/2026) |
| D8 | `max_active_zones` configurable, default 1, con interlock | Aprobada (19/08/2026) |
| D9 | Libreria WebSocket `links2004/WebSockets@2.7.3` (version fija; paquete `arduinoWebSockets` no existe en el registro) | Aprobada (19/08/2026) |
| D10 | Autenticacion del WebSocket: `apikey` en el primer mensaje (`hello`); sin token por dispositivo | Aprobada (19/08/2026) |
| D11 | Subida por PostgREST directo (P6), con el POST JSON exacto documentado | Aprobada (19/08/2026) |
| D12 | URLs de API y WebSocket pre-cargadas por defecto en el portal | Aprobada (19/08/2026) |

## Registro De Cambios

| Fecha | Cambio |
|---|---|
| 19/08/2026 | Revision 2 del plan: prioridades P1-P6; se elimina el registro por MAC y el modelo de Edge Function; se agregan portal cautivo, indicador de conexion, selector y tiempos configurables |
| 19/08/2026 | Aprobacion de D3, D5-D12 con recomendacion por defecto |
| 19/08/2026 | Fase 0 ejecutada: estructura, `platformio.ini`, particiones, docs base |