# Plan De Firmware V3

Proyecto: Riego Control - Firmware V3
Estado del documento: PLAN (sin implementacion ni cambios de codigo)
Fecha: 19/08/2026
Revision: 2 (incorpora prioridades del usuario del 19/08/2026)

---

## 1. Contexto Y Motivacion

Firmware V2 esta publicado y funcionando en la placa (ESP32 DevKit V1, commit
`4d10efd`, repo privado `riego-control-firmware-v2`). Su contrato de
integracion definia 6 operaciones HTTP contra una Edge Function, con polling
cada 5 minutos. La integracion real con el equipo cloud fallo: se rompieron
los endpoints, el dispositivo quedo huerfano de token y el equipo cloud termino
escribiendo directamente en tablas con RLS desactivado.

V3 concentra el aprendizaje en una sola responsabilidad: **el firmware**. La
integracion cloud la ejecutara otro equipo; este repositorio entrega firmware,
contrato normativo, mock de referencia y suite de conformidad que ese equipo
DEBE pasar antes de integrar.

### 1.1 Prioridades Del Usuario (Preceden Sobre Decisiones Anteriores)

El usuario establecio estas prioridades; si alguna entra en conflicto con
decisiones previas, estas ganan:

| # | Prioridad | Conflicto resuelto |
|---|---|---|
| P1 | Portal cautivo con restablecimiento a valores de fabrica O acceso a configuraciones avanzadas (URL API con valor por defecto, configuracion de zonas, tiempos de lectura y de subida de datos) | Prioridad nueva: se agrega al alcance |
| P2 | Primer pulso del selector: no cambia la pantalla pero si participa en el flujo | Se preserva el comportamiento de V2 |
| P3 | Mostrar en pantalla si la conexion inicial fue exitosa | Se agrega al alcance de la UI |
| P4 | WebSocket como protocolo de comunicacion (bidireccionalidad) | Refuerza D1 |
| P5 | Seguridad minima para evitar complicaciones; NO usar direcciones MAC | **Reemplaza D4**: se elimina el registro por MAC |
| P6 | Conexion cloud simple y directa: POST HTTP con JSON a la API REST de Supabase (PostgREST), RLS desactivada, header `apikey` | **Reemplaza el modelo de Edge Function/registro** del plan anterior |

### 1.2 Insumo De Diseno: Reflexion "Si Tuviera Que Empezar Desde Cero"

El usuario autorizo de forma explicita usar como insumo la reflexion tecnica
almacenada en el repo V2 (`riego-control-firmware-v2/Si tuviera que empezar
desde cero/README.md`). V3 adopta sus ideas compatibles con las prioridades
P1-P6 y descarta las que las contradicen:

| Idea de la reflexion | Estado en V3 | Donde |
|---|---|---|
| Configuracion cloud persistida completa (snapshots dobles) | Adoptada | 6.3 / Fase 2 |
| Tarea de red aislada del control fisico | Adoptada | 6.2 |
| Concurrencia explicita (`max_active_zones`) + interlock | Adoptada | 6.2 |
| IDs idempotentes en todo mensaje reintentable | Adoptada (watermark + `command_id`) | 7.3 / 7.4 |
| Protocolo versionado independiente de la config | Adoptada (`protocol_version`) | 7 |
| Reduccion a una sola operacion de red por ciclo | Adaptada: WebSocket para bidireccionalidad (P4) + PostgREST simple para subida (P6) | 7.1 / 7.2 |
| TLS con CA/pinning | Descartada (P5: seguridad minima; se mantiene HTTPS/wss sin CA, documentado como limite) | 3 / 7.6 |
| Aprovisionamiento con codigo de reclamo | Descartada (P5: seguridad minima) | - |
| Registro por MAC | Descartada (P5: NO usar MAC) | 7.1 |
| Re-registro automatico ante 401/403 | Descartada (P6: no hay token de dispositivo que re-registrar; la apikey es la credencial) | - |
| Persistencia de overrides/comandos ejecutados | Adoptada (`command_id` persistido) | 7.3 |
| Definir el protocolo antes de base de datos/frontend | Adoptada (contrato + mock + suite en este repo) | 8 |

## 2. Objetivos

1. Firmware robusto, simple y con control local 100 % independiente de la
   nube.
2. Subida de datos a Supabase de la forma mas simple: POST JSON directo a
   PostgREST con `apikey` (sin librerias complejas, sin Edge Function, sin
   registro de dispositivo, sin MAC).
3. Canal WebSocket bidireccional para comandos y configuracion en tiempo real.
4. Persistencia de politica cloud completa: la placa arranca con la ultima
   configuracion valida, no con defaults.
5. Eventos con garantia at-least-once a traves de reinicios (outbox critica).
6. Comandos remotos idempotentes (IDs persistidos antes de ejecutar).
7. Red aislada del control fisico (tarea dedicada).
8. Portal cautivo con restablecimiento a fabrica y configuraciones avanzadas
   (URL API por defecto, zonas, tiempos de lectura y de subida).
9. Pantalla que informa si la conexion inicial fue exitosa.
10. Documentacion cloud + mock de referencia + suite de conformidad
    ejecutable por el equipo de integracion.

## 3. No-Objetivos

- NO implementar el backend ni el frontend: lo hace otro equipo, con la
  documentacion y la suite de este repositorio.
- NO cambiar hardware: ESP32 DevKit V1, sensores, RTC, OLED, botones y relays
  actuales.
- NO agregar OTA: se mantiene la carga por USB.
- NO implementar seguridad avanzada: sin MAC como identidad, sin tokens de
  dispositivo, sin JWT de usuario, sin claim codes, sin rotacion. La seguridad
  minima es transporte cifrado (HTTPS/wss sin CA) + `apikey` del proyecto.
- NO exponer el dispositivo a Internet (TLS sin CA/pinning): se documenta como
  limite.
- NO persistir identidad de dispositivo: la credencial (apikey) se configura
  en el portal y puede cambiarse manualmente.

## 4. Principios De Diseno

1. El dispositivo es la autoridad sobre la accion fisica; la nube expresa
   politica, nunca deadlines.
2. Perder WiFi o la conexion WebSocket nunca deja una valvula abierta.
3. Una configuracion nueva se valida completa antes de reemplazar la anterior
   (dos snapshots).
4. Todo mensaje reintentable tiene identidad idempotente (watermark simple).
5. El protocolo se versiona independientemente de la configuracion.
6. Backend y frontend pueden cambiar sin cambiar el firmware.
7. La concurrencia de valvulas es politica explicita.
8. La conformidad del equipo cloud se verifica con herramientas ejecutables,
   no con confianza.
9. Simplicidad sobre sofisticacion: si hay dos disenos equivalentes, se elige
   el mas simple.

## 5. Alcance Funcional

### 5.1 Hardware Conservado

- Sensores: DHT21 (aire), OneWire (sustrato y aire), humedad ADC por zona.
- RTC con NTP, OLED, botones (Selector/Confirmar/Config).
- Relays active-low para zonas de sustrato y aspiracion + alarma.
- Riego manual, remoto (override), horario y por umbral.
- Precedencia: `MANUAL > OVERRIDE > PAUSA > HORARIO > UMBRAL`.
- Una vez iniciado un riego, se conserva trigger y deadline; no se corta por
  desaparicion del candidato en la siguiente evaluacion.
- Alarmas: temp max/min, sin conexion, humedad persistente baja.
- Control local completo sin nube.

### 5.2 Portal Cautivo (P1)

- Pagina de configuracion con:
  - WiFi (SSID/password).
  - Zonas (cantidad de sustrato y aspiracion).
  - Calibracion de humedad por zona (vivo).
  - **Configuraciones avanzadas**: URL de la API de datos (con valor por
    defecto pre-cargado), URL del WebSocket (con valor por defecto), apikey de
    Supabase, tiempo de lectura (seg), tiempo de subida (seg).
  - **Boton "Restablecer valores de fabrica"** (con confirmacion): borra
    configuracion, calibracion, snapshots y outbox; reinicia con defaults.
- La URL de la API y la del WebSocket pueden venir pre-cargadas por defecto
  para no obligar a escribirlas en cada placa nueva.

### 5.3 UI / Pantalla (P2, P3)

- **Primer pulso del Selector**: entra en el flujo de seleccion de zona pero
  no altera la pantalla en ese primer instante (comportamiento heredado de V2,
  se preserva).
- **Indicador de conexion inicial**: al arrancar, la pantalla muestra si la
  conexion inicial (WiFi + primer contacto con la nube) fue exitosa o fallida,
  con estado persistente durante la sesion.
- Pantallas existentes: normal (lecturas, hora, alarma), manual, config,
  alarma.

## 6. Arquitectura Propuesta Del Firmware

### 6.1 Modulos

| Modulo | Responsabilidad | Notas |
|---|---|---|
| `hal/` | GPIO, sensores, RTC, OLED, botones, relays | Misma base que V2 |
| `domain/` | Reglas puras de riego, prioridades, transiciones | Reutilizar `lib/domain` de V2 (ya probado) con revision menor |
| `runtime/` | Estado activo, deadlines, aplicacion sobre relays | Concurrencia explicita y watchdog |
| `persist/` | Snapshots de configuracion, outbox critica, configuracion de portal | NVS + particion LittleFS para cola |
| `cloud/` | Subida PostgREST + WebSocket en tarea dedicada | Sin acceso directo a GPIO |
| `portal/` | AP + servidor web de configuracion y restablecimiento | Amplia el portal de V2 |

### 6.2 Concurrencia

- Tarea de control (loop principal): ciclo local de lectura configurable
  (default 30 s), serviceTimeouts en cada pasada, watchdog.
- Tarea de red (FreeRTOS, prioridad baja): WiFi, subida PostgREST, WebSocket,
  reconexion con backoff. Comunicacion con el control via cola de mensajes.
- La tarea de red JAMAS toca relays; el runtime procesa las intenciones
  (comandos) en su propia evaluacion.
- Politica explicita en configuracion local segura:
  `max_active_zones = 1` (configurable, con interlock documentado).

### 6.3 Persistencia

- NVS namespace unico `cfg`: WiFi, zonas, calibracion, URL API (default),
  URL WS (default), apikey, tiempos de lectura/subida, `config_version`
  aplicada.
- Snapshots de politica cloud: `config_current` y `config_previous` (version
  de esquema + checksum). La configuracion completa se persiste al aplicar.
- Outbox critica en LittleFS (particion dedicada, p. ej. 64 KB): eventos de
  riego no confirmados, alarmas no confirmadas, IDs de comandos ejecutados.
- Boot: relays OFF -> cargar snapshot valido -> RTC/sensores -> habilitar
  evaluacion local -> iniciar tarea de red. Sin ventana de defaults.
- Restablecimiento a fabrica: borra `cfg` completo + outbox (desde el portal,
  con confirmacion).

### 6.4 Estado De Configuracion

- Arranque con la ultima politica valida: duraciones, pausas, umbrales,
  horarios, limites de alarma, zone_ids.
- `config_version` monotona; config nueva se aplica solo si es superior y
  valida.
- Si no existe snapshot: defaults de fabrica + estado "sin politica cloud"
  documentado (fail-open conservador como en V2).

## 7. Protocolo Cloud Propuesto (V3)

### 7.1 Subida De Datos: PostgREST Directo (P6)

La via de subida mas simple, sin Edge Function, sin registro y sin MAC:

```http
POST https://<proyecto>.supabase.co/rest/v1/<tabla>
Headers:
  apikey: <SUPABASE_API_KEY>
  Authorization: Bearer <SUPABASE_API_KEY>
  Content-Type: application/json

Body (JSON):
{"device_alias":"invernadero-1","reading_type":"soil_humidity","zone_index":1,"value":42.5,"recorded_at":"2026-08-19T12:00:00Z"}
```

- RLS desactivada en la tabla (responsabilidad del equipo cloud; se documenta
  como limite de seguridad).
- La apikey es la unica credencial; se configura en el portal (P1).
- Sin `device_id`, sin MAC: la diferenciacion por placa se hace con una
  columna libre (p. ej. `device_alias`) que el usuario escribe en el portal.
- Retry simple: si el POST falla (5xx/timeout), se conserva el lote en RAM/outbox
  y se reintenta con backoff (30 s -> 30 min). Un `4xx` indica error de
  configuracion (tabla/columna/apikey) y se loguea sin reintentar.
- Intervalo de subida configurable (P1), default 60 s.

### 7.2 Canal Bidireccional: WebSocket (P4)

Conexion persistente para recibir comandos y configuracion en tiempo real:

```text
wss://<ws_endpoint>/device/v1/ws
```

- URL del endpoint configurable en el portal (con valor por defecto).
- Al conectar, el dispositivo envia un `hello` con `protocol_version` y
  `firmware_version` (sin MAC, sin token: autenticacion minima por apikey en
  el upgrade o primer mensaje, segun decision D10).
- Mensajes JSON con envelope `{"type":"...", ...}`:

| type (device -> cloud) | type (cloud -> device) |
|---|---|
| `hello` | `hello_ok` |
| `pong` | `ping` (keep-alive) |
| `command_update` (resultado de comando) | `command` (override, config_push) |
| `config_ack` | `config` (politica completa) |
| - | `close` (motivo) |

- La nube puede enviar comandos en cualquier momento: bidireccionalidad real.
- Keep-alive: la nube envia `ping`; el dispositivo responde `pong`. Sin
  mensajes durante N segundos, el dispositivo reconecta.
- Reconexion con backoff (5 s -> 5 min), sin afectar el control local.
- Si el WebSocket esta caido, los comandos se pierden (no hay cola de
  downlink en el firmware): el control local y las subidas PostgREST siguen
  funcionando.

### 7.3 Idempotencia Simple (Watermark)

- Cada lote de telemetria/eventos lleva el mayor `client_id` generado; la
  respuesta (o el exito del POST) confirma hasta ese ID y el dispositivo
  descarta lo confirmado.
- Comandos: la placa persiste `command_id` antes de ejecutar; un reboot no
  repite un comando ejecutado.

### 7.4 Comandos Remotos

- Estados: `pending -> accepted -> started -> completed` y `accepted ->
  rejected`.
- La placa responde `command_update` con el estado final.
- El cloud no marca `completed` sin confirmacion del dispositivo.

### 7.5 Fallos Y Reconexion

| Situacion | Comportamiento |
|---|---|
| POST PostgREST 200/201 | Lote confirmado; se descarta el watermark |
| POST PostgREST 4xx | Error de configuracion; log y sin reintento del lote |
| POST PostgREST 5xx / timeout | Backoff 30 s -> 30 min; se conserva el lote |
| WS `hello` aceptado | Flujo normal |
| WS caido | Reconexion 5 s -> 5 min; el resto del sistema sigue |
| Sin WiFi | Nada se envia; buffer local conserva datos |

### 7.6 Seguridad (Minima, Documentada)

- Transporte cifrado: HTTPS y wss (sin CA/pinning, igual que V2). Se
  documenta como limite: NO exponer a Internet.
- Unica credencial: la apikey del proyecto (configurada en el portal).
- Sin MAC, sin tokens de dispositivo, sin JWT de usuario, sin claim codes.
- RLS desactivada por decision de simplicidad (P6): se documenta que cualquier
  poseedor de la apikey puede leer/escribir; el equipo cloud debe evaluar el
  riesgo en produccion.

## 8. Documentacion Cloud A Entregar (En Este Repositorio)

1. **Contrato normativo V3** (`docs/contrato_nube_v3.md`): subida PostgREST
   (tablas, columnas, headers, codigos) + protocolo WebSocket (envelope,
   tipos, watermark, comandos, keep-alive, codigos de cierre); lenguaje
   DEBO/NO DEBO; limites de tamano.
2. **Mock de referencia** (`tools/mock/`): servidor local que simula PostgREST
   (recibe POST y guarda en JSON) y el WebSocket (envia comandos). Sirve para
   HIL y para que el equipo cloud lo reemplace por la implementacion real.
3. **Suite de conformidad** (`tools/conformidad/`): script ejecutable que
   prueba la subida y el canal WS contra cualquier implementacion (mock o
   real) y emite un reporte PASS/FAIL. El equipo cloud debe entregar el
   reporte PASS antes de integrar.
4. **Checklist de entrega cloud** (`docs/checklist_entrega_cloud.md`):
   esquema de tablas, RLS (y su aceptacion explicita del riesgo), apikey,
   keep-alive, codigos de cierre, dedup por `client_id`.
5. **Esquema DB sugerido** (`docs/esquema_db_sugerido.md`): tablas y columnas
   orientativas para telemetria, eventos y comandos (no vinculante).

## 9. Estrategia De Pruebas

Niveles:

1. **Nativas** (Unity en PC): dominio de riego, runtime, persistencia de
   snapshots y outbox.
2. **De contrato** (host): subida PostgREST y WebSocket contra el mock; casos
   de borde (reconexion, watermark, 4xx/5xx, timeout, IDs duplicados).
3. **De integracion en placa**: firmware contra mock local via WiFi.
4. **HIL**: placa + sensores + relays reales; escenarios de fallo:
   - WiFi interrumpido; servidor lento; respuesta perdida.
   - Reinicio durante riego, durante comando, durante subida.
   - RTC incorrecto; configuracion invalida; varias zonas simultaneas.
   - apikey invalida (4xx de configuracion); outbox llena; WS caido.
   - Restablecimiento de fabrica desde el portal.

Criterio de aceptacion de cada fase: pruebas del nivel correspondiente en
verde + checklist de la fase completado.

## 10. Fases Y Entregables

### Fase 0 — Fundacion
- Crear carpeta `riego-control-firmware-v3` y repositorio privado nuevo
  (GitHub, rama `main`).
- Confirmar decisiones pendientes (seccion 11).
- `README.md`, estructura de carpetas, `platformio.ini` (espressif32@7.0.1,
  board esp32dev, particiones con LittleFS), dependencias (ArduinoJson,
  WebSockets).
- Criterio: estructura estable y decisiones registradas en `docs/decisiones.md`.

### Fase 1 — Dominio, Runtime Y UI
- Copiar y revisar `lib/domain` de V2; ajustes menores documentados.
- Runtime con deadlines, `max_active_zones`, interlock y watchdog.
- UI: indicador de conexion inicial (P3), primer pulso de selector (P2).
- Pruebas nativas en verde.
- Criterio: suite nativa 100 % + comportamiento de UI verificado en placa.

### Fase 2 — Persistencia Y Portal
- Snapshots `config_current`/`config_previous` con checksum.
- Outbox LittleFS con limites y wear-leveling.
- Boot deterministico: relay OFF -> snapshot -> evaluacion local.
- Portal cautivo completo: WiFi, zonas, calibracion, avanzado (URL API default,
  URL WS default, apikey, tiempos lectura/subida) y restablecimiento a fabrica.
- Pruebas nativas de persistencia (reboot simulado).
- Criterio: reinicios simulados no pierden politica ni eventos; portal
  configurado y restablecido sin dejar estados inconsistentes.

### Fase 3 — Cliente Cloud
- Subida PostgREST (headers, JSON, watermark, backoff) + WebSocket
  (envelope, hello, ping/pong, comandos, config push, reconexion).
- Tarea de red dedicada con cola de mensajes.
- Telemetria RAM + outbox critica.
- Pruebas de contrato contra mock (nivel 2).
- Criterio: suite de contrato en verde contra el mock.

### Fase 4 — Documentacion Cloud
- Contrato normativo V3, mock de referencia, suite de conformidad, checklist
  y esquema DB sugerido.
- Validacion cruzada de la documentacion contra el codigo.
- Criterio: documentacion revisada y sin discrepancias con el firmware.

### Fase 5 — HIL Y Validacion En Placa
- Firmware en placa real contra mock local (WiFi).
- Escenarios de fallo de la seccion 9.
- Criterio: checklist HIL completo sin regresiones.

### Fase 6 — Release
- Tag `v3.0.0` en el repositorio nuevo.
- Publicacion del repositorio (privado) con firmware + documentacion cloud.
- Entrega formal de la suite de conformidad al equipo cloud.
- Criterio: tag publicado y suite de conformidad entregada.

## 11. Decisiones

### Prioridades Del Usuario (Vigentes)

- **P1** Portal cautivo: restablecimiento a fabrica + configuraciones
  avanzadas (URL API por defecto, zonas, tiempos lectura/subida).
- **P2** Primer pulso de selector: afecta el flujo, no la pantalla.
- **P3** Indicador de conexion inicial en pantalla.
- **P4** WebSocket bidireccional.
- **P5** Seguridad minima; NO usar MAC.
- **P6** Subida simple: PostgREST directo con apikey, RLS desactivada.

### Pendientes (recomendacion por defecto, confirmar en Fase 0)

| # | Decision | Recomendacion por defecto | Impacto |
|---|---|---|---|
| D3 | Outbox en LittleFS | Si, particion dedicada 64 KB | Particiones y Fase 2 |
| D5 | TLS sin CA (como V2) | Si, documentado como limite | Sin cambios de librerias |
| D6 | Nombre del repositorio nuevo | `riego-control-firmware-v3` | Repositorio remoto |
| D7 | Dominio de V2: copiar tal cual vs revisar | Copiar con revision menor y cambios documentados | Fase 1 |
| D8 | `max_active_zones` por configuracion | Si, default 1 | Runtime y config |
| D9 | Libreria WebSocket | `arduinoWebSockets` (Links2004), version fija | Fase 0, dependencias |
| D10 | Autenticacion del WebSocket | Apikey en el primer mensaje (`hello`); sin token por dispositivo | Protocolo WS |
| D11 | Tipo de subida | PostgREST directo (P6); se documenta el POST con JSON exacto | Protocolo |
| D12 | Defaults de URL en portal | Pre-cargados con la URL del proyecto actual | Portal (P1) |

## 12. Riesgos Y Mitigaciones

| Riesgo | Mitigacion |
|---|---|
| El equipo cloud no pasa la suite de conformidad | Entrega formal + criterio de aceptacion previo a integrar; reporte PASS obligatorio |
| RLS desactivada expone datos con la apikey | Se documenta como limite aceptado (P6); checklist exige confirmacion explicita del equipo cloud |
| WebSocket con libreria ajena introduce bugs | Version fija, pruebas de reconexion, keep-alive, watchdog de conexion |
| Cambios de particiones/FS en placa | Pruebas de rebote (reboot loop), limites de escritura, wear-leveling |
| Payload grande sobre WS o POST | Limites de tamano documentados; cap en firmware; telemetria priorizada |
| Sin downlink persistente si WS cae | Comandos solo en tiempo real; se documenta (control local sigue siendo autoridad) |
| Regresion de reglas de riego probadas | Dominio copiado con pruebas nativas como red de seguridad |
| Deadline de valvula vs red lenta | Tarea de red separada + watchdog + serviceTimeouts en cada pasada |
| Restablecimiento de fabrica borra datos por error | Confirmacion en el portal y registro en pantalla; no disponible fuera del modo config |

## 13. Estructura Del Repositorio Nuevo (Propuesta)

```text
riego-control-firmware-v3/
  platformio.ini
  partitions/no_ota_with_littlefs.csv
  src/
    main.cpp
    hal/        (sensors, rtc, oled, buttons, relay, wifi)
    domain/     (motor de riego puro)
    runtime/    (deadlines, concurrencia, watchdog)
    persist/    (snapshots, outbox, config)
    cloud/      (postgrest, websocket, backoff, watermark)
    portal/     (AP de configuracion + avanzado + restablecimiento)
  test/
    domain/  (Unity nativas)
    persist/
    cloud/
  docs/
    README.md
    arquitectura.md
    decisiones.md
    contrato_nube_v3.md
    checklist_entrega_cloud.md
    esquema_db_sugerido.md
    operacion.md
    validacion.md
  tools/
    mock/        (servidor PostgREST simulado + WS de referencia)
    conformidad/ (suite de pruebas ejecutable)
```

## 14. Proximo Paso

Confirmar las decisiones pendientes D3-D12 de la seccion 11. Con eso se ejecuta
la Fase 0: creacion de la carpeta/repositorio nuevo y el commit inicial de
estructura y documentacion. Ninguna implementacion de firmware ocurre antes de
la aprobacion de este plan.

## 15. Fuente De Insumo

- `riego-control-firmware-v2/Si tuviera que empezar desde cero/README.md`
  (reflexion hipotetica autorizada por el usuario como insumo para V3; ver
  1.2 para el mapeo adopcion/descarte).
- Prioridades P1-P6 del usuario (19/08/2026), que preceden a decisiones
  anteriores en caso de conflicto.