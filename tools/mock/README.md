# Mock de referencia del cloud (contrato V3)

Simula el backend para probar el firmware sin dependencias externas. Es la
implementacion de referencia que el equipo cloud DEBE reemplazar por la real;
la suite de conformidad (`../conformidad/`) se ejecuta contra el.

## Uso rapido

```bash
python tools/mock/mock_cloud.py                    # REST 8080, WS 8765
python tools/mock/mock_cloud.py -p 8081 -w 8766    # otros puertos
python tools/mock/mock_cloud.py --no-demo --ping-s 30
python tools/mock/mock_cloud.py --require-apikey CLAVE
python tools/mock/mock_cloud.py --config-push-file push.json
```

Opciones (detalle en el docstring de `mock_cloud.py`):

| Opcion | Efecto |
|---|---|
| `-p/--port` | Puerto REST (POST /eventos) |
| `-w/--ws-port` | Puerto WebSocket |
| `--ping-s N` | Intervalo del ping JSON de keep-alive (default 30 s) |
| `--no-demo` | No enviar los comandos de demostracion tras conectar |
| `--config-push-file F` | Enviar ese JSON como config push tras el `hello_ok` |
| `--require-apikey C` | Exigir `apikey` (o `Authorization: Bearer`) == C; si no, 401 |
| `--rest-delay-ms N` | Retardo de respuesta del POST (simular servidor lento) |

## Supervisor (mantenerlo vivo)

En entornos donde el proceso puede ser terminado externamente (HIL en
Windows), ejecutarlo bajo el supervisor para reinicio automatico:

```bash
python tools/mock/supervisor.py -- -p 8081 -w 8766 --ping-s 20
```

El supervisor relanza `mock_cloud.py` si sale y registra en stdout la hora y el
codigo de salida de cada arranque/salida (0 = muerte externa; != 0 = crash).
Reintenta con backoff exponencial (2..60 s) si el proceso muere en menos de 3 s
(por ejemplo, puerto ocupado).

## Notas

- Los eventos recibidos se guardan en `mock_eventos.jsonl` (directorio de
  trabajo), con dedup idempotente por `client_id` (refuerza D16).
- El keep-alive del contrato es el **ping JSON**; el mock NO envia pings de
  protocolo WebSocket (`ping_interval=None`) porque la libreria del firmware
  no responde a pings de protocolo del servidor.
- La tabla de referencia de puertos en este repo (HIL): placa en 8081/8766,
  suite de conformidad en 8091/8767.