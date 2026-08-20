# Contrato Normativo V3 — Firmware <-> Nube

Version del contrato: **1** (`protocol_version = 1`)
Firmware de referencia: `3.0.0-dev`
Fecha: 19/08/2026
Estado: NORMATIVO (lenguaje DEBE / NO DEBE; verificable con
`tools/conformidad/conformidad.py`)

Este documento es el contrato entre el firmware `riego-control-firmware-v3`
y cualquier implementacion de nube (backend). El equipo de integracion DEBE
cumplir cada clausula antes de integrar, y DEBE entregar el reporte PASS de la
suite de conformidad. El mock de referencia (`tools/mock/mock_cloud.py`) es la
implementacion de ejemplo que cumple este contrato.

---

## 1. Transportes

El firmware usa DOS transportes simultaneos:

| Transporte | Direccion | Proposito |
|---|---|---|
| REST (HTTP/HTTPS) | dispositivo -> nube | Subida de eventos (telemetria, riego, alarmas, comandos) |
| WebSocket | bidireccional | Handshake, keep-alive, comandos, configuracion |

Ambos transportes se configuran en el portal cautivo del dispositivo
(`api_url` y `ws_url`). El firmware DEBE funcionar con `ws_url` vacio (solo
subida REST); el REST es obligatorio.

## 2. Subida REST (PostgREST directo)

### 2.1 Endpoint

```
POST <api_url>/eventos
```

- `<api_url>` es la base configurada (ejemplo: `https://proyecto.supabase.co/rest/v1`).
- El firmware concatena `/eventos` a la base sin doble barra: si la base termina
  en `/`, el firmware la recorta. La nube DEBE aceptar el endpoint con o sin
  barra final en la base.
- El cuerpo es un **array JSON** de eventos (ver seccion 3).

### 2.2 Headers

| Header | Valor | Obligatorio |
|---|---|---|
| `Content-Type` | `application/json` | SI |
| `Prefer` | `resolution=ignore-duplicates,return=minimal` | SI (ver 2.4) |
| `apikey` | la apikey del proyecto | solo si configurada |
| `Authorization` | `Bearer <apikey>` | solo si configurada |

La apikey es OPCIONAL (decision D17): si no esta configurada en el portal, el
firmware NO envia `apikey` ni `Authorization`. La nube DEBE aceptar peticiones
sin apikey, o documentar que su backend requiere autenticacion propia por otro
medio configurable en el portal. La nube NO DEBE rechazar el POST solo porque
falten estos headers si el contrato no define otro mecanismo.

### 2.3 Codigos de respuesta

| Codigo | Significado para el firmware |
|---|---|
| 2xx | Lote aceptado. El firmware confirma (ack) hasta el mayor `client_id` del lote y descarta esos eventos del outbox |
| 4xx | Error de configuracion (tabla, columna, esquema, apikey). El firmware descarta el lote SIN reintentar y lo registra |
| 5xx, timeout, error de red | Transitorio. El firmware conserva el lote y reintenta con backoff (30 s -> 30 min) |

La nube DEBE devolver 2xx para un lote valido. La nube DEBE devolver 4xx (no
5xx) cuando el problema es de configuracion del lado del dispositivo/nube
(tabla inexistente, columnas invalidas, apikey invalida).

### 2.4 Idempotencia (deduplicacion por `client_id`)

Cada evento lleva `client_id`, un entero monotono por dispositivo, persistido
entre reinicios. Ante un POST duplicado (mismos `client_id`), la nube DEBE
comportarse de forma idempotente:

- DEBE aceptar el lote con 2xx aunque todos sus `client_id` ya existan.
- NO DEBE insertar filas duplicadas para un `client_id` repetido
  (el `Prefer: resolution=ignore-duplicates` de PostgREST es el mecanismo
  recomendado).
- La clave de unicidad SUGERIDA es `(device_alias, client_id)`: `client_id`
  solo es unico por dispositivo, no global.

### 2.5 Limites

| Limite | Valor | Obligatorio para la nube |
|---|---|---|
| Eventos por lote | maximo 32 | DEBE aceptar lotes de hasta 32 eventos |
| Tamano del cuerpo | maximo 4096 bytes | DEBE aceptar lotes de hasta 4096 bytes |
| `client_id` | uint32 (0..4294967295) | DEBE almacenarlo sin perder precision (BIGINT) |
| Campos de texto | `device_alias` <= 32 chars | DEBE aceptar al menos 32 chars |

## 3. Eventos (esquema por linea del array)

Campos comunes a TODO evento:

| Campo | Tipo | Descripcion |
|---|---|---|
| `client_id` | entero | Id monotono por dispositivo (dedup, seccion 2.4) |
| `device_alias` | texto | Alias configurado en el portal (identidad visible; NO es MAC) |
| `event_type` | texto | Uno de: `reading`, `irrigation_start`, `irrigation_stop`, `alarm`, `alarm_cleared`, `command` |
| `recorded_at` | texto | Marca de tiempo UTC ISO 8601: `YYYY-MM-DDTHH:MM:SSZ` (sin fraccion) |

La nube DEBE aceptar cada evento como JSON con estos campos y los de su tipo
(abajo), ignorando campos extra que agreguen versiones futuras.

### 3.1 `reading` — telemetria de sensores

| Campo | Tipo | Presente |
|---|---|---|
| `zone_type` | `"substrate"` o `"sprinkler"` | solo humedad/temperatura de sustrato |
| `zone_index` | entero 0-based | solo sustrato |
| `reading_type` | `"soil_humidity"`, `"soil_temp"`, `"air_temp"`, `"air_humidity"` | siempre |
| `value` | numero decimal | siempre |

Ejemplo:
```json
{"client_id":64,"device_alias":"Prototipo_1","event_type":"reading",
 "recorded_at":"2026-08-19T23:45:21Z","zone_type":"substrate",
 "zone_index":0,"reading_type":"soil_humidity","value":44.8}
```

### 3.2 `irrigation_start` / `irrigation_stop`

| Campo | Tipo | Descripcion |
|---|---|---|
| `zone_type` | `"substrate"` o `"sprinkler"` | tipo de zona |
| `zone_index` | entero 0-based | indice de zona |
| `trigger` | entero | Origen del riego: 1=MANUAL, 2=OVERRIDE (remoto), 3=SCHEDULE (horario), 4=THRESHOLD (umbral) |
| `duration_s` | entero | solo `irrigation_start`: duracion en segundos |

### 3.3 `alarm` / `alarm_cleared`

| Campo | Tipo | Descripcion |
|---|---|---|
| `condition` | entero | 1=aire demasiado caliente, 2=aire demasiado frio, 3=sin conexion 60 min, 4=humedad de sustrato persistentemente baja |

### 3.4 `command` — registro de comando remoto ejecutado

| Campo | Tipo | Descripcion |
|---|---|---|
| `command_id` | entero | Id del comando recibido por WS (dedup del comando) |
| `status` | `"accepted"` | unico estado emitido (ver 4.4) |
| `zone` | `"S0"`..`"S3"`, `"A0"`..`"A1"` | zona objetivo |
| `action` | `"on"` o `"off"` | accion |
| `duration_s` | entero | duracion en segundos (relevante solo si `on`) |

## 4. WebSocket

### 4.1 Conexion

- URL configurada en el portal (`ws_url`), con o sin path (por defecto `/`).
- Cifrado: `wss` sin verificacion de CA (limite documentado, D5).
- Envelope JSON: TODO mensaje DEBE tener `"type"`.

### 4.2 Mensajes

| type | Origen | Campos | Descripcion |
|---|---|---|---|
| `hello` | dispositivo | `protocol_version` (1), `firmware_version`, `device_alias`, `apikey` (opcional) | Primer mensaje al conectar |
| `hello_ok` | nube | `protocol_version` | Respuesta al `hello`; la nube DEBE enviarla (<= 5 s razonables) |
| `ping` | nube | - | Keep-alive; la nube DEBE enviarlo periodicamente (ver 4.5) |
| `pong` | dispositivo | - | Respuesta a `ping` |
| `command` | nube | `command_id`, `zone`, `action`, `duration_s` (ver 4.4) | Orden de riego/stop |
| `command_update` | dispositivo | `command_id`, `status` (`accepted` o `rejected`) | Resultado del comando |
| `config` | nube | ver 4.6 | Politica de configuracion completa o parcial |
| `config_ack` | dispositivo | `version`, `status` (`applied` o `rejected`), `reason` (si `rejected`) | Resultado del push de config |
| `close` | nube | `reason` | Aviso de cierre por la nube (el dispositivo lo registra; la reconexion es automatica) |

### 4.3 Handshake

1. El dispositivo conecta y envia `hello`.
2. La nube DEBE responder `hello_ok`.
3. La nube NO DEBE considerar autenticado al dispositivo por `apikey` si esta
   ausente: la apikey es opcional (D17) y viaja en `hello` cuando esta
   configurada.

### 4.4 Comandos

Mensaje `command` (nube -> dispositivo):

```json
{"type":"command","command_id":7,"zone":"S0","action":"on","duration_s":60}
```

- `zone`: `"S"` + indice de sustrato (0..3) o `"A"` + indice de aspiracion
  (0..1).
- `action`: `"on"` (riego con `duration_s`) u `"off"` (corte inmediato).
- El dispositivo responde SIEMPRE `command_update` con `status`:
  - `accepted`: el comando fue persistido y se aplicara en la evaluacion del
    control (un comando duplicado —mismo `command_id`— se responde `accepted`
    sin re-ejecutarse: idempotencia por `command_id`).
  - `rejected`: `command_id` ausente/cero, `zone` invalida, `action` invalida
    o indice fuera de rango.
- Estados: el firmware solo emite `accepted`/`rejected` (simplificacion V3;
  no hay `started`/`completed`). El registro `command` en la subida REST
  (seccion 3.4) confirma la ejecucion aceptada.
- `command_id` es uint32; la nube DEBE generar ids unicos (no reutilizar).

### 4.5 Keep-alive

- La nube DEBE enviar `ping` (JSON) al menos cada 120 s.
- El dispositivo responde `pong` (JSON).
- Si el dispositivo no recibe NINGUN mensaje JSON de la nube durante 120 s,
  desconecta y reconecta (intervalo 5 s). Los pings a nivel de protocolo WS
  (opcode ping) los responde la libreria, pero NO cuentan para el keep-alive
  JSON del contrato.
- La nube DEBE tolerar reconexiones frecuentes (el dispositivo reenvia `hello`
  en cada reconexion).

### 4.6 Push de configuracion

Mensaje `config` (nube -> dispositivo). Campos (todos opcionales salvo
`version`; los omitidos conservan el valor actual):

| Campo | Tipo | Rango |
|---|---|---|
| `version` | entero | DEBE ser superior a la configuracion vigente para aplicarse |
| `ssid` | texto | nombre WiFi |
| `wifi_pass` | texto | clave WiFi |
| `device_alias` | texto | alias visible |
| `api_url` | texto | base REST (http(s)://) |
| `ws_url` | texto | URL WS (ws(s)://) o vacio |
| `api_key` | texto | apikey opcional |
| `substrate_zones` | entero | 1..4 |
| `sprinkler_zones` | entero | 0..2 |
| `read_interval_s` | entero | 5..3600 |
| `upload_interval_s` | entero | 10..3600 |

Respuestas (dispositivo -> nube):

- `{"type":"config_ack","version":N,"status":"applied"}` si es valida y la
  version es superior a la vigente.
- `{"type":"config_ack","version":N,"status":"rejected","reason":"..."}` si no
  valida o la version no es superior.

Si cambian `ssid` o `ws_url`, el dispositivo desconecta WiFi/WS y reconecta
con la nueva configuracion; la nube DEBE tolerar la interrupcion.

### 4.7 Cierre

La nube puede enviar `close` con `reason` antes de cerrar la conexion. El
dispositivo lo registra; la reconexion es automatica (intervalo 5 s).

## 5. Seguridad (minima, documentada — D5, P5, P6)

- Transporte: HTTPS y wss SIN verificacion de CA (igual que V2). Limite
  documentado: NO exponer el dispositivo a Internet.
- Unica credencial: la apikey del proyecto (opcional, seccion 2.2/4.2).
- Sin MAC, sin tokens de dispositivo, sin JWT de usuario, sin claim codes.
- RLS: el firmware asume RLS desactivada en la tabla de eventos; el equipo
  cloud DEBE aceptar explicitamente este riesgo en el checklist de entrega.

## 6. Comportamiento ante fallos (resumen normativo)

| Situacion | Comportamiento DEBE |
|---|---|
| POST 2xx | ack del lote (watermark = max `client_id`) |
| POST 4xx | descartar lote, registrar, sin reintento |
| POST 5xx/timeout/red | conservar lote, backoff 30 s -> 30 min |
| WS caido | reconexion cada 5 s; el resto del sistema sigue funcionando |
| Sin WiFi | nada se envia; el outbox conserva los eventos |
| Reinicio | el outbox y el contador `client_id` sobreviven; se reenvia todo lo no confirmado |
| Comando duplicado | `command_update accepted` sin re-ejecutar |

## 7. Referencias

- Mock de referencia: `tools/mock/mock_cloud.py`
- Suite de conformidad: `tools/conformidad/conformidad.py`
- Checklist de entrega: `docs/checklist_entrega_cloud.md`
- Esquema DB sugerido: `docs/esquema_db_sugerido.md`
- Decisiones D5, D10, D11, D13, D16, D17: `docs/decisiones.md`