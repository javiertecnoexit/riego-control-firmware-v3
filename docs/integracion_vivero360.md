# Integracion Vivero360 (Supabase) — v3.0.4

Fecha: 20/10/2026
Estado: ingesta verificada en placa. Pendientes: lectura del frontend y
autenticacion por dispositivo.

## 1. Backend de prueba

| Dato | Valor |
|---|---|
| Proyecto | `qdzjtejgbafrfbmuggjo` |
| URL base REST | `https://qdzjtejgbafrfbmuggjo.supabase.co/rest/v1` |
| Endpoint | `POST /eventos` |
| Tabla | `public.eventos` |
| Clave primaria | `eventos_pkey` sobre `(device_alias, client_id)` |
| RLS | Activa, politica **solo de INSERT** para `anon` |
| Politica de SELECT | No existe: el SELECT con la clave publicable devuelve `[]` |
| Autenticacion | `apikey` publishable en el portal (temporal) |

El desarrollador preve reemplazar la clave publica por autenticacion por
dispositivo en una etapa posterior. No commitear claves en el repositorio.

## 2. Contrato aplicado

- Headers: `Content-Type: application/json`, `Prefer: return=minimal`,
  `apikey` y `Authorization: Bearer <apikey>` si la clave esta configurada.
- Cada evento lleva el superset de 15 claves; las no aplicables van en `null`.
- Respuestas: `2xx` -> ack del lote; `409` -> ya insertado; `4xx` (excepto 409)
  -> se descarta el lote; `5xx`/timeout -> backoff 30 s a 30 min.
- `device_alias` propio: `esp32-riego-01` (hoy la placa tiene
  `ESP32-SIM-01`; hay que cambiarlo en el portal).
- `client_id`: `uint32` monotono por dispositivo. La PK compuesta evita
  colisiones con el simulador de Vivero360.
- `recorded_at`: UTC ISO 8601 (`...Z`); la base tambien acepta `-03:00`.

## 3. Cambios de firmware (v3.0.4)

- `src/net.cpp`:
  - `httpPostJson()`: header `Prefer: return=minimal` (sin
    `resolution=ignore-duplicates`, que exige SELECT bajo RLS).
  - `httpFlushOneByOne()`: ante `409`, reintenta evento por evento.
    `2xx` insertado; `409` ya insertado (ambos se confirman); `4xx` descarta
    ese evento; `5xx`/timeout corta y aplica backoff.
  - `httpFlush()`: usa el lote normal y deriva a `httpFlushOneByOne()` en 409.
  - Log de arranque: `[NET] API <url> | alias "<alias>" | apikey si/no | WS ...`.
- `tools/conformidad/conformidad.py`: payload homogeneo (15 claves),
  `Prefer: return=minimal`, caso C2 acepta `2xx` o `409`, C5 pasa a SKIP si no
  hay lectura visible.
- Documentacion: contrato, implementacion cloud, checklist, esquema,
  decisiones (D18), validacion y PLAN_V3.

## 4. Evidencia de la prueba

| Prueba | Resultado |
|---|---|
| `curl` lote de 4 lecturas, `device_alias=esp32-riego-01`, `return=minimal` | `201` |
| Placa, lote normal | `POST /eventos 201: ack hasta 55090, outbox 3` |
| Duplicado forzado del `client_id` 55097 | Lote `409`; fallback: `409 ya insertado, ack hasta 55097`; `201` hasta 55105; outbox drenado |
| Estabilidad | 0 reinicios por watchdog |

El duplicado se inserto manualmente con `curl` antes del siguiente flush para
forzar el 409. Los eventos nuevos del mismo lote se insertaron uno por uno
(55098 a 55105), lo que valida que el fallback no pierde datos.

## 5. Configuracion de la placa

| Campo | Valor |
|---|---|
| `api_url` | `https://qdzjtejgbafrfbmuggjo.supabase.co/rest/v1` |
| `ws_url` | vacio (solo subida; sin comandos remotos por ahora) |
| `apikey` | publishable del proyecto (configurada) |
| `device_alias` | `esp32-riego-01` (pendiente; actualmente `ESP32-SIM-01`) |

Para cambiar el alias: pulsar el boton **Configuracion** en operacion normal.
La placa detiene el riego, pausa la red y levanta el AP
`RiegoControl-XXXX` (sin clave) en `192.168.4.1`. Guardar el alias y reiniciar.

## 6. Pendientes

1. Cambiar el alias a `esp32-riego-01` en el portal.
2. Confirmar como lee Vivero360 la tabla:
   - backend con `service_role` o rol autenticado -> no requiere cambios; o
   - navegador con la clave publicable -> necesita politica de SELECT o una
     vista/RPC de lectura.
3. Autenticacion por dispositivo (plan del desarrollador).
4. Confirmar con el frontend que acepta `client_id` en el rango `uint32`.
5. Borrar filas de prueba por SQL: `client_id` 999301, 999501 a 999504 y 55097
   con sus alias de prueba.
6. Opcional: agregar politica de SELECT temporal para el dashboard.

## 7. Como reproducir

```bash
# Insercion normal (esperado 201)
curl -i -X POST "https://qdzjtejgbafrfbmuggjo.supabase.co/rest/v1/eventos" \
  -H "apikey: <APIKEY>" -H "Authorization: Bearer <APIKEY>" \
  -H "Content-Type: application/json" -H "Prefer: return=minimal" \
  --data '[{"client_id":999601,"device_alias":"esp32-riego-01","event_type":"reading","recorded_at":"2026-10-20T18:30:00Z","zone_type":"substrate","zone_index":0,"reading_type":"soil_temp","value":20.1,"trigger":null,"duration_s":null,"condition":null,"command_id":null,"status":null,"zone":null,"action":null}]'

# Repetir el mismo cuerpo (esperado 409)
```

En el monitor serie (115200) se espera:

```text
[NET] POST /eventos 201: ack hasta N, outbox M
[NET] POST /eventos 409: duplicados, reintento individual
[NET] POST individual 409: ya insertado, ack hasta N
[NET] POST individual 201: ack hasta N+1
...
```

## 8. Referencias

- `docs/contrato_nube_v3.md` (normativo)
- `docs/implementacion_cloud.md`
- `docs/esquema_db_sugerido.md`
- `docs/checklist_entrega_cloud.md`
- `docs/validacion.md`
- `docs/decisiones.md` (D18)
- `tools/conformidad/conformidad.py`
