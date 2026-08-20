# Validacion HIL — Fase 5 (Escenarios De Fallo En Placa)

Procedimiento de los escenarios de fallo de la seccion 9 del PLAN. Se ejecutan
contra la placa real (COM11, 115200) y el mock local en 192.168.1.10.

Entorno: SSID `Speedy-Fibra-066185`, placa 192.168.1.90, mock REST 8081 / WS 8766.
Criterio comun: sin resets por watchdog (TG1WDT_SYS_RESET) y recuperacion
automatica sin intervencion.

| ID | Escenario | Como se simula | Criterio | Estado |
|---|---|---|---|---|
| E1 | WiFi interrumpido | Apagar el router ~45 s | Log de desconexion (motivo), reintentos cada 15 s, sin reset; al volver: reconexion + flush del outbox | Cubierto (parcial) 20/08: al cortar la energia del router la placa perdio alimentacion (USB del router) -> POWERON_RESET; boot deterministico tras el corte, control local OK, WS reintentos + backoff REST mientras el backend inalcanzable, sin panics. El path explicito de "WiFi desconectado" se repetira al final con fuente de alimentacion separada |
| E2 | Backend caido (WS + REST) | Matar el mock ~4 min | WS reconecta cada 5 s, POST conn refused -> backoff 30 s->60 s, outbox retiene; mock vuelve: lote reenviado, dedup 201, watermark avanza | PASS 20/08 |
| E3 | Servidor lento / respuesta perdida | Mock con `--rest-delay-ms 20000` (> timeout 10 s del cliente) | POST con timeout -> backoff; mock sin retardo: recuperacion y flush | PASS 20/08 |
| E4 | Reinicio durante subida | Con outbox pendiente, pulsar RST | Boot deterministico (relay OFF, snapshot), outbox persistido, subida tras WiFi con dedup | PASS 20/08 |
| E5 | RTC incorrecto | Sin NTP el timestamp es best-effort | La subida no se rompe con timestamp local; formato ISO 8601 respetado (C1 lo cubre) | Verificado (parcial) 20/08: formatos aceptados por el mock en todos los lotes; sin NTP la exactitud queda a cargo del cloud |
| E6 | Config push invalida | Push `{"version":901,"substrate_zones":9}` | `config_ack` `rejected` con reason, config anterior intacta | PASS 20/08 (rechazo "Zonas de sustrato fuera de rango (1..4)", v901 intacta) |
| E7 | Varias zonas simultaneas | Comandos S0 ON 10 s y A0 ON 10 s casi simultaneos | Ambos `accepted` (max_active_zones=2), apagado correcto, eventos de riego | PASS 20/08 |
| E8 | Apikey invalida (4xx) | Mock con `--require-apikey`, placa sin apikey (o incorrecta) | 401 -> lote descartado sin reintento (diseno documentado); apikey correcta -> 201 | PASS 20/08 (401 descartado; push v903 con apikey -> 201 e insercion) |
| E9 | Outbox llena | Mock caido + `read_interval_s=5` hasta 512 lineas / 32 KB | Descarte de eventos antiguos con log, sin reset; mock vuelve: flush | PASS 20/08 (outbox al tope con 5 s de ciclo, drenado sin panics; tope 512/32 KB; trim conserva ultimas) |
| E10 | WS caido | Cubierto por E2 (reconexion 5 s + keep-alive 120 s) | - | Cubierto por E2 |
| E11 | Restablecimiento de fabrica | Boton en el portal (usuario) | Config a defaults (v1), outbox borrado, boot normal | PASS 20/08 (reset -> "Sin config valida: defaults de fabrica", SSID vacio/WS no, outbox vacio; reconfigurado en 2 etapas: URLs y luego red; placa operativa de nuevo) |

Notas:
- E5: el firmware no tiene NTP; `recorded_at` usa el reloj local. El contrato
  exige ISO 8601 UTC; la nube DEBE validar el formato, no la exactitud.
- E8: descartar en 4xx es un diseno aceptado (D17); la perdida de datos en ese
  caso es deliberada y documentada en el contrato (seccion de fallos).
- E9: los limites del outbox son 32 KB / 512 lineas (STORE_OUTBOX_MAX_BYTES /
  STORE_OUTBOX_MAX_LINES); al superarlos se conservan las ultimas lineas.
  `read_interval_s` valido: 5..3600 s (el push de 2 s fue rechazado con reason).

### Fallos reales encontrados en HIL (Fase 5) y corregidos

| Fecha | Sintoma | Causa raiz | Fix |
|---|---|---|---|
| 20/08 | POST -11 siempre, outbox no drenaba, mock recibia el body | `HTTPClient::setTimeout()` en arduino-esp32 3.x toma ms; se pasaba `/1000` -> timeout de lectura de 10 ms | `setTimeout(CLOUD_HTTP_TIMEOUT_MS)` |
| 20/08 | WiFiClient fugado por POST (heap caia ~2 KB/flush) -> tras horas la tarea de red degradaba | HTTPClient guarda la referencia; el llamador debe liberar el cliente | `delete client;` tras `http.end()` (+ en el early return de begin) |
| 20/08 | "Stack smashing protect failure" al drenar lotes grandes | `body[4 KB]` + `full[256]` en la pila de la tarea de red (12 KB) | Buffers a heap; `NET_TASK_STACK_SIZE` 16384 |