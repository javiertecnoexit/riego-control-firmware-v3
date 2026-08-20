# Validacion HIL — Fase 5 (Escenarios De Fallo En Placa)

Procedimiento de los escenarios de fallo de la seccion 9 del PLAN. Se ejecutan
contra la placa real (COM11, 115200) y el mock local en 192.168.1.10.

Entorno: SSID `Speedy-Fibra-066185`, placa 192.168.1.90, mock REST 8081 / WS 8766.
Criterio comun: sin resets por watchdog (TG1WDT_SYS_RESET) y recuperacion
automatica sin intervencion.

| ID | Escenario | Como se simula | Criterio | Estado |
|---|---|---|---|---|
| E1 | WiFi interrumpido | Apagar el router ~45 s | Log de desconexion (motivo), reintentos cada 15 s, sin reset; al volver: reconexion + flush del outbox | Pendiente |
| E2 | Backend caido (WS + REST) | Matar el mock ~4 min | WS reconecta cada 5 s, POST conn refused -> backoff 30 s->60 s, outbox retiene; mock vuelve: lote reenviado, dedup 201, watermark avanza | Pendiente |
| E3 | Servidor lento / respuesta perdida | Mock con `--rest-delay-ms 20000` (> timeout 10 s del cliente) | POST con timeout -> backoff; mock sin retardo: recuperacion y flush | Pendiente |
| E4 | Reinicio durante subida | Con outbox pendiente, pulsar RST | Boot deterministico (relay OFF, snapshot), outbox persistido, subida tras WiFi con dedup | Pendiente |
| E5 | RTC incorrecto | Sin NTP el timestamp es best-effort | La subida no se rompe con timestamp local; formato ISO 8601 respetado (C1 lo cubre) | Pendiente (verificacion parcial) |
| E6 | Config push invalida | Push `{"version":901,"substrate_zones":9}` | `config_ack` `rejected` con reason, config anterior intacta | Pendiente |
| E7 | Varias zonas simultaneas | Comandos S0 ON 10 s y A0 ON 10 s casi simultaneos | Ambos `accepted` (max_active_zones=2), apagado correcto, eventos de riego | Pendiente |
| E8 | Apikey invalida (4xx) | Mock con `--require-apikey`, placa sin apikey (o incorrecta) | 401 -> lote descartado sin reintento (diseno documentado); apikey correcta -> 201 | Pendiente |
| E9 | Outbox llena | Mock caido + `read_interval_s=2` hasta 512 lineas / 32 KB | Descarte de eventos antiguos con log, sin reset; mock vuelve: flush | Pendiente |
| E10 | WS caido | Cubierto por E2 (reconexion 5 s + keep-alive 120 s) | - | Cubierto por E2 |
| E11 | Restablecimiento de fabrica | Boton en el portal (usuario) | Config a defaults (v1), outbox borrado, boot normal | Pendiente |

Notas:
- E5: el firmware no tiene NTP; `recorded_at` usa el reloj local. El contrato
  exige ISO 8601 UTC; la nube DEBE validar el formato, no la exactitud.
- E8: descartar en 4xx es un diseno aceptado (D17); la perdida de datos en ese
  caso es deliberada y documentada en el contrato (seccion de fallos).
- E9: los limites del outbox son 32 KB / 512 lineas (STORE_OUTBOX_MAX_BYTES /
  STORE_OUTBOX_MAX_LINES); al superarlos se conservan las ultimas lineas.