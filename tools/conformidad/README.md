# Suite de conformidad (Fase 4)

Emula al **dispositivo** y valida la implementacion cloud contra el contrato
`docs/contrato_nube_v3.md`. Reporta PASS/FAIL/SKIP por caso y termina con
codigo de salida 0 si no hay FAIL.

## Requisitos

- Python 3.10+ con el modulo `websockets`.
  - Windows: `py -m pip install websockets` o `python -m pip install websockets`.
- Una implementacion cloud ejecutandose (el mock `tools/mock/mock_cloud.py`
  cumple el contrato y sirve de referencia).
- Linux/WSL y Windows estan soportados.

## Uso

```text
python tools/conformidad/conformidad.py --api-url http://IP:8081 --ws-url ws://IP:8766
```

Casos (C = REST, W = WebSocket):

| Caso | Que valida |
|---|---|
| C1 | POST lote valido -> 2xx |
| C2 | POST duplicados idempotente (misma `client_id`) |
| C3 | POST con apikey invalida -> 4xx (requiere `--apikey`) |
| C4 | POST sin apikey aceptado (apikey opcional) |
| C5 | Dedup confirmado por lectura (`--check-storage`, solo mock) |
| W1 | WS hello -> `hello_ok` en <= 5 s |
| W2 | Ping JSON de la nube -> pong en la ventana `--keepalive-s` (130 s) |
| W3 | Comando -> `command_update` con `accepted`/`rejected` |
| W4 | Config push -> `config_ack` con `applied`/`rejected` |

## Opciones

| Flag | Por defecto | Descripcion |
|---|---|---|
| `--api-url` | `http://127.0.0.1:8081` | Base URL REST del backend |
| `--ws-url` | `ws://127.0.0.1:8766` | URL WebSocket del backend |
| `--apikey` | *(ninguna)* | Apikey a probar (habilita C3) |
| `--hello-timeout-s` | `5` | Timeout del handshake hello |
| `--keepalive-s` | `130` | Ventana de espera del ping JSON |
| `--check-storage` | *(off)* | Lee `GET /eventos` para confirmar dedup (solo mock) |
| `--out` | *(stdout)* | Archivo de reporte opcional |

## Ejemplo con el mock

```bash
# Terminal 1: mock dedicado a la suite
python tools/mock/mock_cloud.py -p 8091 -w 8767 --no-demo --ping-s 20

# Terminal 2: suite
python tools/conformidad/conformidad.py --api-url http://127.0.0.1:8091 \
  --ws-url ws://127.0.0.1:8767 --check-storage --out reporte.md

echo $?   # 0 = sin FAIL
```

## Reglas de evaluacion

- Un caso con fallo -> FAIL y la suite termina con codigo 1.
- Casos no aplicables (p. ej. C3 sin `--apikey`) -> SKIP y no afectan la salida.
- El reporte es texto plano simple; el equipo cloud puede anexarlo a su PR de
  integracion como evidencia de conformidad.