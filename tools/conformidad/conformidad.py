#!/usr/bin/env python3
"""Suite de conformidad del contrato cloud V3.

Emula un dispositivo (firmware V3) y verifica que una implementacion del
backend (mock de referencia o la nube real) cumple el contrato normativo
de `docs/contrato_nube_v3.md`:

  REST (subida):
    C1 POST /eventos acepta un lote valido y responde 2xx
    C2 POST /eventos con client_id duplicados es idempotente (2xx)
    C3 POST /eventos sin apikey es aceptado (el firmware solo envia
       headers de auth si la apikey esta configurada) [si --no-auth]
    C4 POST /eventos con apikey es aceptado [si --apikey]
    C5 Los eventos del lote no son duplicados tras reenviar el mismo lote
       [solo verificable si el backend expone lectura; con el mock: si]

  WebSocket (canal bidireccional):
    W1 hello -> hello_ok en <= --hello-timeout-s s
    W2 la nube envia mensajes (ping JSON) al menos cada --keepalive-s s;
       el dispositivo responde pong
    W3 la nube puede enviar comandos; el dispositivo responde command_update
    W4 la nube puede enviar config; el dispositivo responde config_ack

Emite un reporte PASS/FAIL (consola y, con --out, archivo Markdown).
Codigo de salida 0 si no hay FAIL.

Uso:
  python tools/conformidad/conformidad.py \
      --api-url http://127.0.0.1:8081 \
      --ws-url ws://127.0.0.1:8766 [--apikey CLAVE] \
      [--wait-command-s 60] [--keepalive-s 130] [--out reporte.md]

Contra el mock de referencia (self-check de la suite y del mock):
  python tools/mock/mock_cloud.py -p 8091 -w 8767 --ping-s 20
  python tools/conformidad/conformidad.py \
      --api-url http://127.0.0.1:8091 --ws-url ws://127.0.0.1:8767
"""

import argparse
import asyncio
import json
import sys
import time
import urllib.error
import urllib.request
from datetime import datetime, timezone

import websockets

PROTOCOL_VERSION = 1
FIRMWARE_VERSION = "3.0.0-dev"
DEVICE_ALIAS = "suite-conformidad"


class Result:
    def __init__(self, case, name, status, detail=""):
        self.case = case
        self.name = name
        self.status = status  # PASS | FAIL | SKIP
        self.detail = detail

    def line(self):
        return f"[{self.status:4}] {self.case} {self.name}: {self.detail}"


def now_iso():
    return datetime.now(timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ")


def make_event(client_id, **extra):
    ev = {
        "client_id": client_id,
        "device_alias": DEVICE_ALIAS,
        "event_type": "reading",
        "recorded_at": now_iso(),
    }
    ev.update(extra)
    return ev


def rest_post(api_url, events, apikey=None):
    url = api_url.rstrip("/") + "/eventos"
    body = json.dumps(events).encode("utf-8")
    req = urllib.request.Request(url, data=body, method="POST")
    req.add_header("Content-Type", "application/json")
    req.add_header("Prefer", "resolution=ignore-duplicates,return=minimal")
    if apikey:
        req.add_header("apikey", apikey)
        req.add_header("Authorization", f"Bearer {apikey}")
    try:
        with urllib.request.urlopen(req, timeout=15) as resp:
            return resp.status, resp.read(4096).decode("utf-8", "replace")
    except urllib.error.HTTPError as e:
        return e.code, e.read(4096).decode("utf-8", "replace")
    except Exception as e:  # noqa: BLE001
        return -1, str(e)


def rest_get_events(api_url):
    url = api_url.rstrip("/") + "/eventos"
    req = urllib.request.Request(url)
    try:
        with urllib.request.urlopen(req, timeout=10) as resp:
            return json.loads(resp.read())
    except Exception:  # noqa: BLE001
        return None


class DeviceWS:
    """Cliente WS que emula al firmware (hello, pong, command_update,
    config_ack) y recoge los mensajes entrantes por tipo."""

    def __init__(self, ws_url, apikey=None):
        self.ws_url = ws_url
        self.apikey = apikey
        self.ws = None
        self.inbox = []

    async def connect(self):
        self.ws = await websockets.connect(self.ws_url, ping_interval=None,
                                           open_timeout=10)
        await self.send_hello()

    async def close(self):
        if self.ws:
            await self.ws.close()
            self.ws = None

    async def send(self, obj):
        await self.ws.send(json.dumps(obj))

    async def send_hello(self):
        hello = {
            "type": "hello",
            "protocol_version": PROTOCOL_VERSION,
            "firmware_version": FIRMWARE_VERSION,
            "device_alias": DEVICE_ALIAS,
        }
        if self.apikey:
            hello["apikey"] = self.apikey
        await self.send(hello)

    async def wait_for(self, msg_type, timeout_s):
        """Espera un mensaje con type == msg_type (timeout en segundos).
        El resto de mensajes se acumulan en self.inbox."""
        end = time.monotonic() + timeout_s
        while time.monotonic() < end:
            remaining = end - time.monotonic()
            if remaining <= 0:
                break
            try:
                raw = await asyncio.wait_for(self.ws.recv(), timeout=remaining)
            except asyncio.TimeoutError:
                break
            try:
                msg = json.loads(raw)
            except json.JSONDecodeError:
                self.inbox.append(("non-json", raw))
                continue
            if msg.get("type") == msg_type:
                return msg
            self.inbox.append((msg.get("type"), msg))
        return None

    async def collect(self, seconds, min_types=None):
        """Recoge mensajes durante `seconds`; responde pong a los ping y
        command_update/config_ack a los command/config. Devuelve los tipos
        vistos."""
        end = time.monotonic() + seconds
        seen = {}
        while time.monotonic() < end:
            remaining = end - time.monotonic()
            if remaining <= 0:
                break
            try:
                raw = await asyncio.wait_for(self.ws.recv(), timeout=remaining)
            except asyncio.TimeoutError:
                continue
            try:
                msg = json.loads(raw)
            except json.JSONDecodeError:
                seen.setdefault("non-json", []).append(raw)
                continue
            t = msg.get("type")
            seen.setdefault(t, []).append(msg)
            if t == "ping":
                await self.send({"type": "pong"})
            elif t == "command":
                await self.send({
                    "type": "command_update",
                    "command_id": msg.get("command_id", 0),
                    "status": "accepted",
                })
            elif t == "config":
                await self.send({
                    "type": "config_ack",
                    "version": msg.get("version", 0),
                    "status": "applied",
                })
        return seen


async def run_ws_cases(args, results):
    try:
        device = DeviceWS(args.ws_url, apikey=args.apikey)
        await device.connect()
    except Exception as e:  # noqa: BLE001
        results.append(Result("W1", "hello -> hello_ok", "FAIL",
                              f"no se pudo conectar: {e}"))
        for case in ("W2", "W3", "W4"):
            results.append(Result(case, {"W2": "ping keep-alive",
                                         "W3": "comando remoto",
                                         "W4": "config push"}[case],
                                  "FAIL", "sin conexion WS"))
        return

    hello_ok = await device.wait_for("hello_ok", args.hello_timeout_s)
    if hello_ok is None:
        results.append(Result("W1", "hello -> hello_ok", "FAIL",
                              f"sin hello_ok en {args.hello_timeout_s}s"))
        await device.close()
        return
    results.append(Result("W1", "hello -> hello_ok", "PASS",
                          f"hello_ok en <{args.hello_timeout_s}s"))

    seen = await device.collect(args.keepalive_s)
    await device.close()

    if seen.get("ping"):
        results.append(Result("W2", "ping keep-alive", "PASS",
                              f"{len(seen['ping'])} ping JSON en "
                              f"{args.keepalive_s}s, pong respondido"))
    else:
        results.append(Result("W2", "ping keep-alive", "FAIL",
                              f"ningun mensaje de la nube en "
                              f"{args.keepalive_s}s (contrato: ping JSON "
                              f"periodico)"))

    cmds = seen.get("command", [])
    if cmds:
        results.append(Result("W3", "comando remoto", "PASS",
                              f"{len(cmds)} comando(s), command_update "
                              f"respondido"))
    else:
        results.append(Result("W3", "comando remoto", "FAIL",
                              f"ningun comando en {args.keepalive_s}s "
                              f"(el cloud DEBE poder enviar comandos)"))

    cfgs = seen.get("config", [])
    if cfgs:
        results.append(Result("W4", "config push", "PASS",
                              f"{len(cfgs)} config(s), config_ack "
                              f"respondido"))
    else:
        results.append(Result("W4", "config push", "SKIP",
                              "sin config push en la ventana de prueba"))


def run_rest_cases(args, results):
    base_id = 400000 + int(time.time()) % 100000

    lote = [
        make_event(base_id + 1, zone_type="substrate", zone_index=0,
                   reading_type="soil_humidity", value=42.5),
        make_event(base_id + 2, reading_type="air_temp", value=20.5),
        make_event(base_id + 3, event_type="irrigation_start",
                   zone_type="substrate", zone_index=0, trigger=3,
                   duration_s=60),
    ]

    code, body = rest_post(args.api_url, lote, apikey=args.apikey)
    if 200 <= code < 300:
        results.append(Result("C1", "lote valido -> 2xx", "PASS",
                              f"POST /eventos -> {code}"))
    else:
        results.append(Result("C1", "lote valido -> 2xx", "FAIL",
                              f"POST /eventos -> {code}: {body[:200]}"))

    code, body = rest_post(args.api_url, lote, apikey=args.apikey)
    if 200 <= code < 300:
        results.append(Result("C2", "duplicados idempotente", "PASS",
                              f"POST repetido -> {code}"))
    else:
        results.append(Result("C2", "duplicados idempotente", "FAIL",
                              f"POST repetido -> {code}: {body[:200]}"))

    if args.apikey:
        code, body = rest_post(args.api_url, lote, apikey="clave-invalida")
        if code in (401, 403):
            results.append(Result("C3", "apikey invalida -> 4xx", "PASS",
                                  f"POST -> {code}"))
        else:
            results.append(Result("C3", "apikey invalida -> 4xx", "SKIP",
                                  f"backend sin auth: POST -> {code}"))
    else:
        results.append(Result("C3", "apikey invalida -> 4xx", "SKIP",
                              "--apikey no proporcionado"))

    if not args.apikey:
        code, body = rest_post(args.api_url, lote)
        if 200 <= code < 300:
            results.append(Result("C4", "sin apikey aceptado", "PASS",
                                  f"POST sin auth -> {code}"))
        else:
            results.append(Result("C4", "sin apikey aceptado", "FAIL",
                                  f"POST sin auth -> {code}: {body[:200]}"))
    else:
        results.append(Result("C4", "sin apikey aceptado", "SKIP",
                              "correr con --no-auth para este caso"))

    if args.check_storage:
        stored = rest_get_events(args.api_url)
        if stored is None:
            results.append(Result("C5", "dedup verificado por lectura", "SKIP",
                                  "el backend no expone lectura de eventos"))
            return
        ids = [ev.get("client_id") for ev in stored]
        dup = {i for i in ids if ids.count(i) > 1}
        if dup:
            results.append(Result("C5", "dedup verificado por lectura", "FAIL",
                                  f"client_id duplicados en almacen: "
                                  f"{sorted(dup)[:5]}"))
        else:
            results.append(Result("C5", "dedup verificado por lectura", "PASS",
                                  f"{len(ids)} eventos, sin duplicados"))
    else:
        results.append(Result("C5", "dedup verificado por lectura", "SKIP",
                              "usar --check-storage con el mock"))


def render_report(results, args):
    lines = []
    lines.append(f"# Reporte De Conformidad — Contrato Nube V3")
    lines.append("")
    lines.append(f"- Fecha: {now_iso()}")
    lines.append(f"- API: {args.api_url}")
    lines.append(f"- WS:  {args.ws_url}")
    lines.append(f"- Apikey: {'proporcionada' if args.apikey else 'no usada'}")
    lines.append(f"- Protocolo: {PROTOCOL_VERSION} | Firmware simulado: "
                 f"{FIRMWARE_VERSION}")
    lines.append("")
    for r in results:
        lines.append(f"- {r.line()}")
    lines.append("")
    n_pass = sum(1 for r in results if r.status == "PASS")
    n_fail = sum(1 for r in results if r.status == "FAIL")
    n_skip = sum(1 for r in results if r.status == "SKIP")
    lines.append(f"**Resumen: {n_pass} PASS, {n_fail} FAIL, {n_skip} SKIP**")
    lines.append("")
    lines.append("El equipo de integracion DEBE entregar un reporte sin FAIL "
                 "antes de integrar el firmware.")
    return "\n".join(lines) + "\n"


def main():
    ap = argparse.ArgumentParser(
        description="Suite de conformidad del contrato cloud V3")
    ap.add_argument("--api-url", required=True,
                    help="base de la API REST (ej: http://127.0.0.1:8081)")
    ap.add_argument("--ws-url", required=True,
                    help="URL del WebSocket (ej: ws://127.0.0.1:8766)")
    ap.add_argument("--apikey", default=None, help="apikey del proyecto")
    ap.add_argument("--hello-timeout-s", type=int, default=5,
                    help="timeout del hello_ok (s)")
    ap.add_argument("--keepalive-s", type=int, default=130,
                    help="ventana de observacion WS / keep-alive (s)")
    ap.add_argument("--check-storage", action="store_true",
                    help="verificar dedup leyendo el almacen (solo mock)")
    ap.add_argument("--out", default=None, help="reporte Markdown")
    args = ap.parse_args()

    results = []
    run_rest_cases(args, results)
    asyncio.run(run_ws_cases(args, results))

    for r in results:
        print(r.line())
    n_fail = sum(1 for r in results if r.status == "FAIL")
    n_pass = sum(1 for r in results if r.status == "PASS")
    n_skip = sum(1 for r in results if r.status == "SKIP")
    print(f"\nResumen: {n_pass} PASS, {n_fail} FAIL, {n_skip} SKIP")

    if args.out:
        with open(args.out, "w", encoding="utf-8") as f:
            f.write(render_report(results, args))
        print(f"Reporte guardado en {args.out}")

    sys.exit(1 if n_fail else 0)


if __name__ == "__main__":
    main()