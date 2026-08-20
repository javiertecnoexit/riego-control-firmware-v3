# Guía de Implementación Cloud — Contrato V3 (v3.0.2)

> Documento **autosuficiente**: el equipo cloud puede implementar el backend completo sin consultar otros archivos. Contiene: todos los mensajes que emite la placa, todos los que recibe, ejemplos JSON reales, esquemas de BD, casos de uso UI y credenciales requeridas.

---

## 0. Mapa Rápido

| Sección | Qué encontrarás |
|---------|-----------------|
| 1       | Arquitectura y credenciales que el cloud debe entregar |
| 2       | REST — endpoint, headers, códigos, idempotencia, límites, ejemplos de payload |
| 3       | Eventos — esquema completo por tipo con ejemplos reales |
| 4       | WebSocket — handshake, keep-alive, comandos (envío y respuesta), config push (campos + validaciones exactas), cierre |
| 5       | Comportamiento del dispositivo que la nube **debe tolerar** (reconexiones, backoff, at-least-once, dedup) |
| 6       | Modelo de datos sugerido (SQL listo para crear) |
| 7       | Casos de uso para la UI (dashboard, control, alarmas, configuración) |
| 8       | Entregables del equipo cloud (credenciales para configurar la placa) |
| 9       | Verificación (suite de conformidad + checklist) |
| A       | Glosario de códigos / reasons |

---

## 1. Arquitectura y Credenciales que el Cloud Debe Entregar

### 1.1 Dos transportes simultáneos

| Transporte | Dirección | Propósito | Obligatorio |
|------------|-----------|-----------|-------------|
| REST (HTTP/HTTPS) | Placa → Nube | Subida de eventos (telemetría, riego, alarmas, comandos) | **Sí** |
| WebSocket | Bidireccional | Handshake, keep-alive, comandos remotos, configuración | No (funciona solo REST si `ws_url` vacío) |

### 1.2 Configuración en el portal cautivo de la placa

La placa se configura en **dos etapas** (orden fijo, validado en HIL):

1. **Etapa 1 — URLs**  
   - `api_url` (base REST, ej. `https://proyecto.supabase.co/rest/v1`)  
   - `ws_url` (URL WS, ej. `wss://proyecto.supabase.co/realtime/v1` o vacío)

2. **Etapa 2 — Red WiFi**  
   - `ssid` + `wifi_pass` (router del usuario)  
   - Tras guardar, la placa reinicia, conecta al router y **hace flush del outbox** (ver §5).

### 1.3 Credenciales que el equipo cloud DEBE entregar al usuario final

| Parámetro | Ejemplo | Dónde se usa | Obligatorio |
|-----------|---------|--------------|-------------|
| `api_url` | `https://abc123.supabase.co/rest/v1` | Portal → Etapa 1 | **Sí** |
| `ws_url` | `wss://abc123.supabase.co/realtime/v1` | Portal → Etapa 1 | No (puede quedar vacío) |
| `apikey` | `eyJhbGciOiJIUzI1NiIs...` | Portal → Etapa 1 (opcional) | No (D17: opcional) |
| TLS / CA | Certificado público válido | `wss://` / `https://` | **Sí** (la placa **no verifica CA** — D5: debe ser CA pública real) |

> **Importante**: La placa usa **HTTPS + wss SIN verificación de CA** (D5). El backend DEBE usar un certificado firmado por CA pública (Let's Encrypt, etc.) para que navegadores y otros clientes funcionen, aunque la placa no lo valide. NO exponer la placa a Internet directo.

---

## 2. REST — Especificación Completa

### 2.1 Endpoint

```
POST <api_url>/eventos
```

- `<api_url>` es la base configurada (ej. `https://proyecto.supabase.co/rest/v1`).  
- La placa concatena `/eventos` a la base **sin doble barra**: si la base termina en `/`, la recorta.  
- La nube DEBE aceptar el endpoint con o sin barra final en la base.  
- Cuerpo: **array JSON** de eventos (ver §3).

### 2.2 Headers

| Header | Valor | Obligatorio |
|--------|-------|-------------|
| `Content-Type` | `application/json` | **Sí** |
| `Prefer` | `resolution=ignore-duplicates,return=minimal` | **Sí** (ver §2.4) |
| `apikey` | la apikey del proyecto | Solo si configurada en el portal |
| `Authorization` | `Bearer <apikey>` | Solo si configurada en el portal |

> **Apikey opcional** (D17): si no está configurada, la placa **NO envía** `apikey` ni `Authorization`. La nube DEBE aceptar peticiones sin estos headers o documentar su propio mecanismo configurable.

### 2.3 Códigos de Respuesta

| Código | Significado para la placa | Acción de la placa |
|--------|---------------------------|--------------------|
| **2xx** | Lote aceptado | ACK hasta el mayor `client_id` del lote; descarta esos eventos del outbox |
| **4xx** | Error de configuración (tabla, columna, esquema, apikey) | **Descarta el lote SIN reintentar** y lo registra |
| **5xx, timeout, error de red** | Transitorio | Conserva el lote; reintenta con **backoff exponencial 30 s → 30 min** |

> La nube DEBE devolver 4xx (no 5xx) cuando el problema es configuración del lado dispositivo/nube (tabla inexistente, columnas inválidas, apikey inválida).

### 2.4 Idempotencia (deduplicación por `client_id`)

- Cada evento lleva `client_id` (entero monótono por dispositivo, persistido entre reinicios).  
- Ante POST duplicado (mismos `client_id`), la nube DEBE:  
  1. Aceptar con 2xx aunque todos los `client_id` ya existan.  
  2. **NO insertar filas duplicadas** (`Prefer: resolution=ignore-duplicates` de PostgREST es el mecanismo recomendado).  
- Clave de unicidad sugerida: **`(device_alias, client_id)`** — `client_id` solo es único por dispositivo, no global.

### 2.5 Límites (la nube DEBE aceptarlos)

| Límite | Valor |
|--------|-------|
| Eventos por lote | **máx. 32** |
| Tamaño del cuerpo | **máx. 4096 bytes** |
| `client_id` | `uint32` (0..4,294,967,295) — almacenar como `BIGINT` |
| `device_alias` | ≤ 32 caracteres |

### 2.6 Ejemplo real de petición (capturado en HIL)

```http
POST /eventos HTTP/1.1
Host: 192.168.1.10:8081
Content-Type: application/json
Prefer: resolution=ignore-duplicates,return=minimal
Content-Length: 4069

[
  {"client_id":1906,"device_alias":"","event_type":"reading","recorded_at":"2026-08-20T23:09:00Z","zone_type":"substrate","zone_index":0,"reading_type":"soil_temp","value":20.2},
  {"client_id":1907,"device_alias":"","event_type":"reading","recorded_at":"2026-08-20T23:09:00Z","reading_type":"air_temp","value":20.9},
  {"client_id":1908,"device_alias":"","event_type":"reading","recorded_at":"2026-08-20T23:09:00Z","reading_type":"air_humidity","value":56.9},
  ...
  {"client_id":1933,"device_alias":"","event_type":"command","recorded_at":"2026-08-20T23:13:30Z","command_id":7,"status":"accepted","zone":"S0","action":"on","duration_s":10},
  {"client_id":1934,"device_alias":"","event_type":"reading","recorded_at":"2026-08-20T23:13:30Z","zone_type":"substrate","zone_index":0,"reading_type":"soil_temp","value":20.1},
  {"client_id":1937,"device_alias":"","event_type":"irrigation_start","recorded_at":"2026-08-20T23:13:30Z","zone_type":"substrate","zone_index":0,"trigger":2,"duration_s":10},
  {"client_id":1938,"device_alias":"","event_type":"irrigation_stop","recorded_at":"2026-08-20T23:13:40Z","zone_type":"substrate","zone_index":0,"trigger":2}
]
```

> **Nota**: `device_alias` vacío (`""`) ocurre cuando no se configuró alias en el portal.  
> **Nota**: `recorded_at` es **best-effort** (sin NTP, reloj local) — formato ISO 8601 UTC obligatorio, exactitud no garantizada (E5).

---

## 3. Eventos — Esquema Completo por Tipo

Todos los eventos comparten **campos comunes**:

| Campo | Tipo | Descripción |
|-------|------|-------------|
| `client_id` | entero | Id monótono por dispositivo (dedup, §2.4) |
| `device_alias` | texto | Alias configurado en el portal (identidad visible; NO es MAC) |
| `event_type` | texto | Uno de: `reading`, `irrigation_start`, `irrigation_stop`, `alarm`, `alarm_cleared`, `command` |
| `recorded_at` | texto | Marca UTC ISO 8601: `YYYY-MM-DDTHH:MM:SSZ` (sin fracción) |

> La nube DEBE aceptar cada evento como JSON con estos campos + los de su tipo, **ignorando campos extra** que agreguen versiones futuras.

---

### 3.1 `reading` — Telemetría de sensores

| Campo | Tipo | Presente |
|-------|------|----------|
| `zone_type` | `"substrate"` o `"sprinkler"` | Solo sustrato |
| `zone_index` | entero 0-based | Solo sustrato |
| `reading_type` | `"soil_humidity"`, `"soil_temp"`, `"air_temp"`, `"air_humidity"` | Siempre |
| `value` | número decimal | Siempre |

**Ejemplos reales:**

```json
{"client_id":1906,"device_alias":"","event_type":"reading","recorded_at":"2026-08-20T23:09:00Z","zone_type":"substrate","zone_index":0,"reading_type":"soil_temp","value":20.2}
{"client_id":1907,"device_alias":"","event_type":"reading","recorded_at":"2026-08-20T23:09:00Z","reading_type":"air_temp","value":20.9}
{"client_id":1908,"device_alias":"","event_type":"reading","recorded_at":"2026-08-20T23:09:00Z","reading_type":"air_humidity","value":56.9}
```

> **Lecturas de aire** (`air_temp`, `air_humidity`) **no llevan** `zone_type` ni `zone_index`.  
> **Lecturas de sustrato** llevan `zone_type:"substrate"` y `zone_index` (0..3).

---

### 3.2 `irrigation_start` / `irrigation_stop`

| Campo | Tipo | Descripción |
|-------|------|-------------|
| `zone_type` | `"substrate"` o `"sprinkler"` | Tipo de zona |
| `zone_index` | entero 0-based | Índice de zona |
| `trigger` | entero | Origen: **1=MANUAL, 2=OVERRIDE (remoto), 3=SCHEDULE (horario), 4=THRESHOLD (umbral)** |
| `duration_s` | entero | Solo `irrigation_start`: duración en segundos |

**Ejemplos reales (trigger=2 = OVERRIDE remoto):**

```json
{"client_id":1937,"device_alias":"","event_type":"irrigation_start","recorded_at":"2026-08-20T23:13:30Z","zone_type":"substrate","zone_index":0,"trigger":2,"duration_s":10}
{"client_id":1938,"device_alias":"","event_type":"irrigation_stop","recorded_at":"2026-08-20T23:13:40Z","zone_type":"substrate","zone_index":0,"trigger":2}
```

---

### 3.3 `alarm` / `alarm_cleared`

| Campo | Tipo | Descripción |
|-------|------|-------------|
| `condition` | entero | 1=aire demasiado caliente, 2=aire demasiado frío, 3=sin conexión 60 min, 4=humedad de sustrato persistentemente baja |

```json
{"client_id":120,"device_alias":"Prototipo_1","event_type":"alarm","recorded_at":"2026-08-20T14:30:00Z","condition":3}
{"client_id":125,"device_alias":"Prototipo_1","event_type":"alarm_cleared","recorded_at":"2026-08-20T15:15:00Z","condition":3}
```

---

### 3.4 `command` — Registro de comando remoto ejecutado

| Campo | Tipo | Descripción |
|-------|------|-------------|
| `command_id` | entero | Id del comando recibido por WS (dedup del comando) |
| `status` | `"accepted"` | Único estado emitido (ver §4.4) |
| `zone` | `"S0"`..`"S3"`, `"A0"`..`"A1"` | Zona objetivo |
| `action` | `"on"` o `"off"` | Acción |
| `duration_s` | entero | Duración en segundos (relevante solo si `on`) |

**Ejemplo real (formato corregido v3.0.2 — incluye `device_alias` + `recorded_at`):**

```json
{"client_id":1933,"device_alias":"","event_type":"command","recorded_at":"2026-08-20T23:13:30Z","command_id":7,"status":"accepted","zone":"S0","action":"on","duration_s":10}
```

> **Importante**: el firmware **solo emite `status="accepted"`** (simplificación V3; no hay `started`/`completed`). El registro `command` en REST confirma la ejecución aceptada.

---

## 4. WebSocket — Protocolo Completo

### 4.1 Conexión

- URL: `ws_url` configurada en el portal (con o sin path; default `/`).  
- Cifrado: **`wss` sin verificación de CA** (igual que V2, D5).  
- Envelope JSON: **TODO mensaje DEBE tener `"type"`**.

### 4.2 Tabla de Mensajes

| type | Origen | Campos | Descripción |
|------|--------|--------|-------------|
| `hello` | Dispositivo | `protocol_version` (1), `firmware_version`, `device_alias`, `apikey` (opcional) | Primer mensaje al conectar |
| `hello_ok` | Nube | `protocol_version` | Respuesta al `hello`; **debe enviarse ≤ 5 s** |
| `ping` | Nube | — | Keep-alive JSON; **debe enviarse al menos cada 120 s** |
| `pong` | Dispositivo | — | Respuesta a `ping` |
| `command` | Nube | `command_id`, `zone`, `action`, `duration_s` | Orden de riego/stop |
| `command_update` | Dispositivo | `command_id`, `status` (`accepted`/`rejected`) | Resultado del comando |
| `config` | Nube | ver §4.6 | Push de configuración (completa o parcial) |
| `config_ack` | Dispositivo | `version`, `status` (`applied`/`rejected`), `reason` (si `rejected`) | Resultado del push |
| `close` | Nube | `reason` | Aviso de cierre (la placa lo registra; reconexión automática) |

---

### 4.3 Handshake (obligatorio)

```
DISPOSITIVO → NUBE:  {"type":"hello","protocol_version":1,"firmware_version":"3.0.0-dev","device_alias":"Prototipo_1","apikey":"opcional"}
NUBE → DISPOSITIVO:  {"type":"hello_ok","protocol_version":1}
```

- La nube **NO DEBE** considerar autenticado al dispositivo por `apikey` si está ausente (es opcional, D17).  
- `firmware_version` es informativo (`"3.0.0-dev"` en código; no se bumpa por release).  
- Si `hello_ok` no llega en ≤ 5 s, la placa reconecta.

---

### 4.4 Comandos (Nube → Dispositivo)

**Formato:**

```json
{"type":"command","command_id":7,"zone":"S0","action":"on","duration_s":10}
```

| Campo | Reglas |
|-------|--------|
| `command_id` | `uint32` > 0, único (la nube no debe reutilizar) |
| `zone` | `"S0"`..`"S3"` (sustrato, máx 4) o `"A0"`..`"A1"` (aspersión, máx 2) |
| `action` | `"on"` (riego con `duration_s`) u `"off"` (corte inmediato) |
| `duration_s` | entero ≥ 0 (solo relevante si `action="on"`) |

**Respuesta del dispositivo (`command_update`):**

```json
{"type":"command_update","command_id":7,"status":"accepted"}
```

| `status` | Significado |
|----------|-------------|
| `"accepted"` | Comando persistido y se aplicará en la evaluación del control. **Comando duplicado (mismo `command_id`) → `accepted` sin re-ejecutar** (idempotencia por `command_id`). |
| `"rejected"` | `command_id`=0 o ausente, `zone` inválida, `action` inválida, índice fuera de rango. |

> **Ejemplo real capturado:**
> ```
> NUBE →: {"type":"command","command_id":7,"zone":"S0","action":"on","duration_s":10}
> DISP →: {"type":"command_update","command_id":7,"status":"accepted"}
> ```

---

### 4.5 Keep-alive (obligatorio)

- La nube **DEBE** enviar `ping` (JSON) **al menos cada 120 s**.  
- El dispositivo responde `pong` (JSON).  
- Si el dispositivo no recibe **NINGÚN mensaje JSON** de la nube durante 120 s, desconecta y reconecta (intervalo 5 s).  
- Los pings a nivel de protocolo WS (opcode ping) los responde la librería, **pero NO cuentan** para el keep-alive JSON del contrato.  
- La nube DEBE tolerar reconexiones frecuentes (el dispositivo reenvía `hello` en cada reconexión).

---

### 4.6 Push de Configuración (Nube → Dispositivo)

**Mensaje `config`** — todos los campos **opcionales** salvo `version`; los omitidos conservan el valor actual.

| Campo | Tipo | Rango / Validación |
|-------|------|--------------------|
| `version` | entero | **DEBE ser superior** a la configuración vigente para aplicarse |
| `ssid` | texto | Nombre WiFi |
| `wifi_pass` | texto | Clave WiFi |
| `device_alias` | texto | Alias visible (≤ 32 chars) |
| `api_url` | texto | Base REST `http(s)://` |
| `ws_url` | texto | URL WS `ws(s)://` o vacío |
| `api_key` | texto | Apikey opcional |
| `substrate_zones` | entero | **1..4** |
| `sprinkler_zones` | entero | **0..2** |
| `read_interval_s` | entero | **5..3600** |
| `upload_interval_s` | entero | **10..3600** |

> **`version` omitida** → la placa usa `config_actual.version + 1` (se aplica aunque no se envíe).

**Respuestas (`config_ack`):**

```json
// Aceptada
{"type":"config_ack","version":902,"status":"applied"}

// Rechazada
{"type":"config_ack","version":902,"status":"rejected","reason":"Zonas de sustrato fuera de rango (1..4)"}
```

**Reasons exactos de rechazo (copiar tal cual para tests):**

| Motivo | Texto exacto del `reason` |
|--------|---------------------------|
| SSID vacío | `"SSID de WiFi obligatorio"` |
| `substrate_zones` fuera de rango | `"Zonas de sustrato fuera de rango (1..4)"` |
| `sprinkler_zones` fuera de rango | `"Zonas de aspiracion fuera de rango (0..2)"` |
| `read_interval_s` fuera de rango | `"Tiempo de lectura fuera de rango (5..3600 s)"` |
| `upload_interval_s` fuera de rango | `"Tiempo de subida fuera de rango (10..3600 s)"` |
| `api_url` no `http(s)://` | `"URL de API debe ser http(s)://"` |
| `ws_url` no `ws(s)://` (ni vacío) | `"URL de WS debe ser ws(s)://"` |
| `version` no superior | `"version no superior"` |

> **Si cambian `ssid` o `ws_url`**, el dispositivo desconecta WiFi/WS y reconecta con la nueva config; la nube DEBE tolerar la interrupción.

---

### 4.7 Cierre

La nube puede enviar `close` con `reason` antes de cerrar la conexión.

```json
{"type":"close","reason":"maintenance"}
```

El dispositivo lo registra; la reconexión es automática (intervalo 5 s).

---

## 5. Comportamiento del Dispositivo que la Nube DEBE Tolerar

| Situación | Comportamiento DEBE |
|-----------|---------------------|
| POST 2xx | ACK del lote (watermark = max `client_id`) |
| POST 4xx | Descartar lote, registrar, **sin reintento** |
| POST 5xx / timeout / red | Conservar lote, **backoff 30 s → 30 min** (exponencial, máx 30 min) |
| WS caído | Reconexión cada **5 s**; el resto del sistema sigue funcionando |
| Sin WiFi | Nada se envía; el **outbox conserva** los eventos |
| Reinicio (power-cycle / watchdog) | Outbox y contador `client_id` sobreviven; se reenvía todo lo no confirmado |
| Comando duplicado (mismo `command_id`) | `command_update accepted` **sin re-ejecutar** |
| POST 4xx → lote descartado | **Diseño deliberado** (D17) — pérdida de datos documentada en contrato |

### 5.1 Outbox y garantías

- **Outbox**: partición LittleFS 64 KB, máx **512 líneas / 32 KB**. Al superar: conserva las últimas líneas (trim).  
- **At-least-once**: eventos no ack’d se reenvían tras reinicio o reconexión.  
- **Dedup**: `client_id` monótono por dispositivo; nube usa `Prefer: resolution=ignore-duplicates`.  
- **Flush**: cada `upload_interval_s` (configurable 10..3600 s, default 30 s) + al recibir comando/ACK.

---

## 6. Modelo de Datos Sugerido (SQL Listo)

```sql
-- Tabla única para todos los eventos (D16)
create table eventos (
  client_id    bigint           not null,
  device_alias text             not null,
  event_type   text             not null,
  recorded_at  timestamptz      not null,
  -- reading
  zone_type    text             null,      -- 'substrate' | 'sprinkler'
  zone_index   integer          null,      -- 0-based
  reading_type text             null,      -- 'soil_humidity'|'soil_temp'|'air_temp'|'air_humidity'
  value        double precision null,
  -- irrigation_start / irrigation_stop
  trigger      integer          null,      -- 1=MANUAL 2=OVERRIDE 3=SCHEDULE 4=THRESHOLD
  duration_s   integer          null,
  -- alarm / alarm_cleared
  condition    integer          null,      -- 1..4
  -- command
  command_id   bigint           null,
  status       text             null,      -- 'accepted' | 'rejected'
  zone         text             null,      -- 'S0'..'S3' | 'A0'..'A1'
  action       text             null,      -- 'on' | 'off'
  created_at   timestamptz      not null default now()
);

-- Unicidad por dispositivo (client_id solo es único por device_alias)
alter table eventos
  add constraint eventos_pk primary key (device_alias, client_id);

-- Dedup idempotente (PostgREST: Prefer resolution=ignore-duplicates)
-- Índices sugeridos
create index eventos_recording_time_idx on eventos (recorded_at desc);
create index eventos_device_idx        on eventos (device_alias, recorded_at desc);
create index eventos_type_idx          on eventos (event_type);

-- Vista de comandos (sugerida)
create view comandos as
select device_alias, command_id, zone, action, duration_s, status, recorded_at
from eventos
where event_type = 'command';
```

### 6.1 RLS (aceptación explícita requerida)

- El firmware asume **RLS desactivada** en `eventos` (P6).  
- La nube DEBE aceptar explícitamente este riesgo en el checklist de entrega.  
- Si en producción se activa RLS, el equipo cloud DEBE configurar una política que permita `INSERT` con la apikey del proyecto y documentar el cambio (no es parte del contrato).

### 6.2 Notas de tipo

- `recorded_at` llega en UTC sin fracción (`YYYY-MM-DDTHH:MM:SSZ`); `timestamptz` lo interpreta automáticamente como UTC.  
- `value` es decimal (porcentajes de humedad y temperaturas en °C).  
- Los campos de un tipo de evento no presentes en otros **DEBEN ser `null`**, no valores inventados.

---

## 7. Casos de Uso para la Interfaz de Usuario

La UI se alimenta **exclusivamente** de la tabla `eventos` (y la vista `comandos`). No hay otro canal.

### 7.1 Dashboard Principal (estado actual)

| Widget | Query / Datos |
|--------|---------------|
| Última lectura de humedad por zona sustrato | `select value from eventos where event_type='reading' and reading_type='soil_humidity' and zone_index=? order by recorded_at desc limit 1` |
| Última temperatura sustrato / aire | Igual con `reading_type='soil_temp'` / `air_temp` |
| Humedad aire | `reading_type='air_humidity'` |
| Estado de conexión | WS: `recorded_at` del último evento **cualquiera** < 120 s → "Conectado"; sino "Desconectado" |
| Próximo riego programado | Si hay `event_type='irrigation_start'` con `trigger=3` futuro (o tabla de horarios aparte) |

### 7.2 Historial de Riego

```sql
select recorded_at, zone_type, zone_index, trigger, duration_s
from eventos
where event_type in ('irrigation_start','irrigation_stop')
order by recorded_at desc;
```

- `trigger`: 1=Manual (botón físico), 2=Override (remoto), 3=Schedule (horario), 4=Threshold (umbral).

### 7.3 Alarmas

```sql
select recorded_at, condition,
       case condition
         when 1 then 'Aire demasiado caliente'
         when 2 then 'Aire demasiado frío'
         when 3 then 'Sin conexión 60 min'
         when 4 then 'Humedad sustrato persistentemente baja'
       end as descripcion
from eventos
where event_type in ('alarm','alarm_cleared')
order by recorded_at desc;
```

### 7.4 Control Remoto (UI envía comando)

La UI **no habla directo con la placa** — envía el comando a la **nube**, y la nube lo reenvía por WS al dispositivo.

**Flujo UI → Nube → Placa:**

1. Usuario pulsa "Regar S0 10 min" en la UI.  
2. UI llama `POST /api/commands` (endpoint interno de la nube).  
3. Nube genera `command_id` único (monótono global o por dispositivo) y envía por WS al dispositivo:

   ```json
   {"type":"command","command_id":42,"zone":"S0","action":"on","duration_s":600}
   ```

4. Placa responde `command_update` (accepted/rejected).  
5. Nube persiste el `command_id` + estado y responde a la UI.  
6. Cuando la placa ejecuta, sube evento `command` + `irrigation_start` por REST (confirmación final).

**Idempotencia**: si la UI reintenta (red inestable), la nube reenvía el mismo `command_id` → la placa responde `accepted` sin re-ejecutar.

### 7.5 Configuración desde la UI

La UI puede enviar `config` push (parcial o completo):

```json
{"type":"config","version":903,"read_interval_s":60,"upload_interval_s":300,"device_alias":"Jardín Norte"}
```

- La placa responde `config_ack` (applied/rejected).  
- Si cambia `ssid`/`ws_url` → placa se reconecta (tolerar interrupción).

### 7.6 Datos que la UI NO tiene (y no debe inventar)

- No hay MAC, ni token de dispositivo, ni JWT de usuario.  
- `recorded_at` es best-effort (validar formato, no exactitud).  
- No hay estado "en ejecución" / "completado" para comandos — solo `accepted` (registro) y luego `irrigation_start/stop`.

---

## 8. Entregables del Equipo Cloud (Credenciales para Configurar la Placa)

Tras implementar y verificar con la suite, el equipo cloud **DEBE entregar al usuario final** (para que configure la placa por el portal):

| Parámetro | Formato | Ejemplo | Dónde va en el portal |
|-----------|---------|---------|----------------------|
| `api_url` | `https://<host>/rest/v1` (sin `/eventos`) | `https://abc123.supabase.co/rest/v1` | Etapa 1 — URL API |
| `ws_url` | `wss://<host>/realtime/v1` (o path que la nube use) | `wss://abc123.supabase.co/realtime/v1` | Etapa 1 — URL WS (opcional) |
| `apikey` | string (anon key o service role) | `eyJhbGciOiJIUzI1NiIs...` | Etapa 1 — Apikey (opcional) |

> **Orden de configuración obligatorio**:  
> 1. Usuario accede al portal cautivo de la placa (AP `RIEGO-XXXXXX`).  
> 2. **Etapa 1**: ingresa `api_url`, `ws_url`, `apikey` → guarda.  
> 3. **Etapa 2**: ingresa `ssid` + `wifi_pass` del router → guarda.  
> 4. Placa reinicia, conecta al router, **hace flush del outbox** (POST 201), WS conecta → operativo.

---

## 9. Verificación — Suite de Conformidad + Checklist

### 9.1 Suite de conformidad (ejecutable)

```bash
# 1. Mock de referencia (terminal 1)
python tools/mock/mock_cloud.py -p 8091 -w 8767 --ping-s 20

# 2. Suite contra el mock (debe dar PASS)
python tools/conformidad/conformidad.py --api-url http://127.0.0.1:8091 --ws-url ws://127.0.0.1:8767 --check-storage --out reporte_mock.md

# 3. Suite contra la implementación REAL (debe dar PASS)
python tools/conformidad/conformidad.py --api-url https://<cloud>/rest/v1 --ws-url wss://<cloud>/realtime/v1 [--apikey <clave>] --out reporte_real.md
```

- Exit code **0 = sin FAIL**.  
- Casos: C1..C5 (REST), W1..W4 (WS). C3 requiere `--apikey`.  
- Reporte plano (`--out`) → anexar al PR de integración como evidencia.

### 9.2 Checklist de entrega (docs/checklist_entrega_cloud.md)

El equipo cloud DEBE completar **todos** los puntos:

- [ ] Tabla `eventos` con columnas del contrato + PK `(device_alias, client_id)`  
- [ ] `client_id` como `BIGINT`, `recorded_at` como `timestamptz` (UTC)  
- [ ] RLS desactivada (o política INSERT con apikey) — **aceptación explícita**  
- [ ] POST `/eventos` array JSON, 2xx lote válido, 4xx error config, 5xx/timeout backoff  
- [ ] `Prefer: resolution=ignore-duplicates,return=minimal` aceptado  
- [ ] POST duplicado (mismo `client_id`) → 2xx, no duplica filas  
- [ ] WS `hello` → `hello_ok` ≤ 5 s  
- [ ] WS `ping` JSON cada ≤ 120 s, `pong` aceptado  
- [ ] WS `command` con `command_id` único + `command_update` processed  
- [ ] WS `config` push con `version` superior + `config_ack` handled  
- [ ] Tolerancia a reconexiones (re-hello en cada reconexión)  
- [ ] Suite PASS contra implementación real + reporte adjunto

---

## A. Glosario de Códigos / Reasons

| Código / Reason | Significado |
|-----------------|-------------|
| **WiFi disconnect reason** (evento `ARDUINO_EVENT_WIFI_STA_DISCONNECTED`) | 1=UNSPECIFIED, 3=AUTH_LEAVE, 201=NO_AP_FOUND, 202=AUTH_FAIL, 203=AP_NOT_FOUND |
| `POST 4xx` | Error config (tabla/columna/apikey) → lote descartado sin reintento |
| `POST 5xx / -1 / timeout` | Transitorio → backoff 30 s → 60 s → ... → 30 min |
| `config_ack rejected` reasons | Ver tabla exacta en §4.6 |
| `command_update rejected` | Sin log en placa; causas: `command_id`=0, zone vacía, prefijo no S/A, action no on/off, índice ≥ maxZonas |
| `hello_ok` timeout | > 5 s sin respuesta → placa reconecta |

---

## Changelog del Documento

| Versión | Fecha | Cambios |
|---------|-------|---------|
| 1.0 | 20/08/2026 | Primera versión completa basada en HIL v3.0.2 (contrato normativo, mock, capturas reales, SQL, UI cases, credenciales) |

---

> **Fin del documento**. Con este documento + `tools/conformidad/conformidad.py` + `docs/checklist_entrega_cloud.md`, el equipo cloud tiene **todo lo necesario** para implementar, probar y entregar el backend sin consultas adicionales.