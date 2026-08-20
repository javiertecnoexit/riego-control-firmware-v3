# Checklist De Entrega Cloud — Contrato V3

El equipo de integracion cloud DEBE completar esta checklist y entregar el
reporte PASS de la suite de conformidad (`tools/conformidad/conformidad.py`)
antes de integrar el firmware. Cada punto DEBE marcarse con si/no/fecha.

Referencias: contrato normativo (`docs/contrato_nube_v3.md`), esquema DB
sugerido (`docs/esquema_db_sugerido.md`), mock de referencia
(`tools/mock/mock_cloud.py`).

## 1. Esquema De Base De Datos

- [ ] Tabla `eventos` creada con las columnas del contrato (seccion 3).
- [ ] Unicidad `(device_alias, client_id)` (o mecanismo equivalente) para
      dedup idempotente.
- [ ] `client_id` almacenado como BIGINT (uint32 del firmware no debe perder
      precision).
- [ ] `recorded_at` interpretado como UTC (timestamptz).
- [ ] Aceptacion de `null` en campos de tipos de evento no aplicables.

## 2. RLS Y Seguridad (aceptacion explicita)

- [ ] **RLS desactivada** en `eventos` (o politica de INSERT con la apikey
      del proyecto) — P6 exige aceptar este riesgo.
- [ ] `apikey` configurable en el portal; el firmware funciona SIN apikey
      (D17): el backend DEBE aceptar POST sin headers de auth o definir otro
      mecanismo configurable.
- [ ] TLS: HTTPS/wss sin CA verificado documentado como limite (D5);
      NO exponer el dispositivo a Internet.

## 3. Subida REST

- [ ] `POST <api_url>/eventos` implementado (cuerpo = array JSON).
- [ ] Respuesta 2xx para lotes validos (hasta 32 eventos / 4096 bytes).
- [ ] Respuesta 4xx para errores de configuracion (tabla/columna/apikey),
      nunca 5xx para esos casos.
- [ ] `Prefer: resolution=ignore-duplicates,return=minimal` aceptado.
- [ ] POST duplicado (mismos `client_id`) devuelve 2xx y NO duplica filas.
- [ ] Headers `apikey`/`Authorization: Bearer` aceptados cuando llegan.

## 4. WebSocket

- [ ] Endpoint WS accesible en `ws_url` configurado en el portal.
- [ ] `hello` (protocol_version=1, firmware_version, device_alias, apikey
      opcional) respondido con `hello_ok` en tiempo razonable (<= 5 s).
- [ ] `ping` JSON enviado al menos cada 120 s; `pong` del dispositivo
      aceptado.
- [ ] Envio de comandos `command` con `command_id` unico (zone/action/
      duration_s) y procesamiento de `command_update`.
- [ ] Push de configuracion `config` con `version` superior y manejo de
      `config_ack` (applied/rejected).
- [ ] Tolerancia a reconexiones frecuentes (el dispositivo reenvia `hello`
      en cada reconexion).
- [ ] `close` con `reason` soportado (opcional).

## 5. Verificacion Ejecutable

- [ ] Mock de referencia arranca y la suite pasa contra el:
      `python tools/mock/mock_cloud.py -p 8091 -w 8767`
      `python tools/conformidad/conformidad.py --api-url http://127.0.0.1:8091 --ws-url ws://127.0.0.1:8767 --check-storage`
- [ ] Suite de conformidad ejecutada contra la implementacion REAL:
      `python tools/conformidad/conformidad.py --api-url <real> --ws-url <real> [--apikey <clave>]`
- [ ] Reporte PASS sin FAIL adjunto a esta checklist.

## 6. Observaciones

| Fecha | Punto | Estado | Nota |
|---|---|---|---|
| | | | |