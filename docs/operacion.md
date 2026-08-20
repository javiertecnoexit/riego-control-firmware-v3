# Operacion Y Mantenimiento

Proyecto: Riego Control - Firmware V3 (repositorio privado
`riego-control-firmware-v3`, release `v3.0.1`).

## 1. Instalacion Del Firmware

- Build: `platformio run` (PlatformIO, espressif32@7.0.1, board esp32dev).
- Flash: `platformio run -t upload --upload-port COM11`.
- Los binarios release estan etiquetados en git (`v3.0.0`, `v3.0.1`).
- El flash de la aplicacion NO borra la configuracion (NVS/LittleFS).

## 2. Configuracion Inicial (Portal Cautivo)

1. Pulsar el boton Config: la placa entra en modo AP
   (`RiegoControl-<xxxx>` en 192.168.4.1).
2. Conectarse a esa red y abrir `http://192.168.4.1`.
3. Guardar (se puede hacer en varias etapas; cada guardado reinicia):
   - Red: SSID + contrasena.
   - Zonas: sustrato (1-4) y aspiracion (0-1).
   - Avanzado: `apiUrl`, `wsUrl`, `apikey` (opcional), `read_interval_s`
     (5-3600, default 30), `upload_interval_s` (default 60).
4. Tras guardar, la placa reinicia y aplica la configuracion completa
   (snapshot `config_current` con checksum).

## 3. Restablecimiento A Fabrica

En el portal: boton "Restablecer valores de fabrica" + confirmacion.
Borra configuracion, calibracion, snapshots y outbox; la placa reinicia con
defaults y queda sin red configurada. Hay que reconfigurarla para volver a
usarla.

## 4. Comunicacion Cloud

- **Subida**: POST JSON a `{apiUrl}/eventos` cada `upload_interval_s` con
  header `apikey` (y `Authorization: Bearer`). Dedup por `client_id`
  (watermark).
- **WebSocket**: `{wsUrl}` para comandos y config en tiempo real. Keep-alive:
  la nube envia ping JSON cada N s; el dispositivo responde pong.
- **Reintentos**: backoff 30 s -> 30 min en 5xx/timeout; 4xx descarta el lote
  (error de configuracion).
- **Fuera de linea**: los eventos se acumulan en la outbox (LittleFS, max
  512 lineas / 32 KB) y se suben al recuperar la conexion.

## 5. Alimentacion Y Mantencion Fisica

- La placa se alimenta tipicamente del USB del router: apagar el router apaga
  la placa. Para pruebas de desconexion de red usar una fuente separada.
- Los relays son active-low; al arrancar quedan SIEMPER apagados (boot
  deterministico).

## 6. Diagnosticos

- Log por serial (115200 baud) con prefijos: `[RIEGO]`, `[STORE]`, `[NET]`,
  `[STATE]`, `[PORTAL]`.
- `[NET] POST /eventos 201: ack hasta N, outbox M` = subida confirmada.
- `[NET] POST /eventos -11` = timeout de lectura; -1 = conexion rechazada.
- `[NET] heap libre` cada 60 s: si cae de forma sostenida, hay fuga.
- Watchdog de tarea (30 s): un cuelgue del loop o de la tarea de red reinicia
  la placa automaticamente.

## 7. Seguridad (Limites Documentados)

- HTTPS/wss SIN CA/pinning: NO exponer el dispositivo a Internet.
- RLS desactivada en la tabla de eventos (P6): cualquier poseedor de la apikey
  puede leer/escribir.
- Sin MAC como identidad: la diferenciacion es por `device_alias`.

## 8. Entrega Al Equipo Cloud

Antes de integrar, el equipo cloud debe pasar la suite de conformidad
(`tools/conformidad/`) contra su implementacion y entregar el reporte PASS
(ver `checklist_entrega_cloud.md`).