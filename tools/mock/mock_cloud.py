#!/usr/bin/env python3
"""Mock de referencia del cloud (contrato V3) — Fases 3/4.

Simula el backend para probar el firmware sin dependencias externas:
  REST : http://0.0.0.0:8080
         POST /eventos  -> recibe el lote JSON (array), deduplica por
                           client_id y responde 2xx (idempotente)
         GET  /eventos  -> devuelve todos los eventos almacenados
                           (EXTENSION del mock; no es parte del contrato)
         GET  /health   -> {"ok": true}
  WS   : ws://0.0.0.0:8765
         hello -> hello_ok (con protocol_version)
         ping  -> pong (JSON)
         Envia ping JSON cada --ping-s segundos (keep-alive del contrato)
         Envia un comando de demostracion unos segundos despues de conectar
         (desactivable con --no-demo):
           {"type":"command","command_id":1,"zone":"S0","action":"on","duration_s":10}
           y luego el apagado del mismo comando.
         Con --config-push-file <archivo.json>: envia ese objeto como
         config push unos segundos despues del hello_ok (para probar
         config_ack del dispositivo).

Uso:
    python tools/mock/mock_cloud.py                    # puertos 8080 y 8765
    python tools/mock/mock_cloud.py -p 8081 -w 8766    # otros puertos
    python tools/mock/mock_cloud.py --no-demo --ping-s 30
    python tools/mock/mock_cloud.py --config-push-file push.json

Los eventos recibidos se guardan en mock_eventos.jsonl (mismo directorio).
"""

import argparse
import asyncio
import json
import signal
import sys
import threading
import time
from datetime import datetime, timezone
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

import websockets

EVENTS_FILE = "mock_eventos.jsonl"
WS_DEMO_COMMAND_DELAY_S = 6
WS_PING_DEFAULT_S = 30
WS_CONFIG_PUSH_DELAY_S = 8

_seen_client_ids = set()
_events_lock = threading.Lock()


def log(tag, msg):
    print(f"[{datetime.now().strftime('%H:%M:%S.%f')[:-3]}] {tag}: {msg}",
          flush=True)


def _load_seen_ids():
    try:
        with open(EVENTS_FILE, "r", encoding="utf-8") as f:
            for line in f:
                line = line.strip()
                if not line:
                    continue
                try:
                    ev = json.loads(line)
                    if isinstance(ev.get("client_id"), int):
                        _seen_client_ids.add(ev["client_id"])
                except json.JSONDecodeError:
                    continue
    except FileNotFoundError:
        pass


class EventosHandler(BaseHTTPRequestHandler):
    def _reply(self, code, body=b""):
        self.send_response(code)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        if body:
            self.wfile.write(body)

    def _auth_ok(self):
        if self.server.require_apikey is None:
            return True
        hdr = self.headers.get("apikey", "") or ""
        auth = self.headers.get("Authorization", "") or ""
        if auth.startswith("Bearer "):
            hdr = auth[7:]
        return hdr == self.server.require_apikey

    def do_GET(self):
        if self.path == "/health":
            self._reply(200, b'{"ok":true,"mock":"riego-control-v3"}')
        elif self.path == "/eventos":
            # Extension del mock para inspeccion; NO es parte del contrato.
            if not self._auth_ok():
                self._reply(401, b'{"error":"unauthorized"}')
                return
            with _events_lock:
                rows = []
                try:
                    with open(EVENTS_FILE, "r", encoding="utf-8") as f:
                        for line in f:
                            line = line.strip()
                            if line:
                                rows.append(json.loads(line))
                except FileNotFoundError:
                    pass
            self._reply(200, json.dumps(rows).encode("utf-8"))
        else:
            self._reply(404, b'{"error":"not_found"}')

    def do_POST(self):
        if self.path != "/eventos":
            self._reply(404, b'{"error":"not_found"}')
            return
        if not self._auth_ok():
            log("REST", "POST /eventos rechazado: apikey invalida/ausente")
            self._reply(401, b'{"error":"unauthorized"}')
            return
        if self.server.rest_delay_ms:
            # Simular servidor lento (Fase 5 HIL: timeout del cliente).
            time.sleep(self.server.rest_delay_ms / 1000.0)
        length = int(self.headers.get("Content-Length", 0))
        body = self.rfile.read(length) if length else b""
        try:
            events = json.loads(body)
            if not isinstance(events, list):
                raise ValueError("el cuerpo debe ser un array JSON")
        except (ValueError, json.JSONDecodeError) as e:
            log("REST", f"POST /eventos rechazado: {e}")
            self._reply(400, b'{"error":"invalid_json"}')
            return

        inserted = 0
        with _events_lock:
            with open(EVENTS_FILE, "a", encoding="utf-8") as f:
                for ev in events:
                    cid = ev.get("client_id")
                    if isinstance(cid, int) and cid in _seen_client_ids:
                        continue  # dedup idempotente (Prefer ignore-duplicates)
                    f.write(json.dumps(ev, ensure_ascii=False) + "\n")
                    if isinstance(cid, int):
                        _seen_client_ids.add(cid)
                    inserted += 1
        log("REST", f"POST /eventos: {len(events)} recibidos, "
                    f"{inserted} insertados (dedup)")
        self._reply(201, b'{"inserted":' + str(inserted).encode() + b"}")


async def ws_demo_commands(ws):
    await asyncio.sleep(WS_DEMO_COMMAND_DELAY_S)
    await ws.send(json.dumps({
        "type": "command", "command_id": 1, "zone": "S0",
        "action": "on", "duration_s": 10,
    }))
    log("WS", "comando de demostracion enviado: S0 ON 10s (command_id=1)")
    await asyncio.sleep(15)
    await ws.send(json.dumps({
        "type": "command", "command_id": 2, "zone": "S0",
        "action": "off",
    }))
    log("WS", "comando de demostracion enviado: S0 OFF (command_id=2)")


async def ws_keepalive_pings(ws, interval_s):
    while True:
        await asyncio.sleep(interval_s)
        await ws.send(json.dumps({"type": "ping"}))
        log("WS", "ping JSON enviado (keep-alive)")


async def ws_config_push(ws, push_path):
    try:
        with open(push_path, "r", encoding="utf-8") as f:
            cfg = json.load(f)
    except (OSError, json.JSONDecodeError) as e:
        log("WS", f"config push no leido: {e}")
        return
    payload = {"type": "config"}
    payload.update(cfg)
    await asyncio.sleep(WS_CONFIG_PUSH_DELAY_S)
    await ws.send(json.dumps(payload))
    log("WS", f"config push enviado (version {cfg.get('version')})")


async def ws_handler(ws, args):
    log("WS", f"conexion desde {ws.remote_address}")
    tasks = [asyncio.create_task(ws_keepalive_pings(ws, args.ping_s))]
    if not args.no_demo:
        tasks.append(asyncio.create_task(ws_demo_commands(ws)))
    if args.config_push_file:
        tasks.append(asyncio.create_task(ws_config_push(ws, args.config_push_file)))
    try:
        async for message in ws:
            try:
                data = json.loads(message)
            except json.JSONDecodeError:
                log("WS", f"mensaje no JSON: {message[:120]}")
                continue
            t = data.get("type")
            if t == "hello":
                log("WS", f"hello: protocol={data.get('protocol_version')} "
                          f"fw={data.get('firmware_version')} "
                          f"alias={data.get('device_alias')}")
                await ws.send(json.dumps({
                    "type": "hello_ok", "protocol_version": 1,
                }))
            elif t == "pong":
                log("WS", "pong recibido")
            elif t == "command_update":
                log("WS", f"command_update: id={data.get('command_id')} "
                          f"status={data.get('status')}")
            elif t == "config_ack":
                log("WS", f"config_ack: version={data.get('version')} "
                          f"status={data.get('status')} "
                          f"reason={data.get('reason', '')}")
            else:
                log("WS", f"mensaje tipo {t}: {message[:120]}")
    finally:
        for task in tasks:
            task.cancel()
        log("WS", "conexion cerrada")


async def ws_server(port, args):
    # El keep-alive del contrato es el ping JSON (no pings de protocolo):
    # la libreria del firmware no responde pings de protocolo del servidor.
    async with websockets.serve(
        lambda ws: ws_handler(ws, args), "0.0.0.0", port, ping_interval=None
    ):
        log("WS", f"escuchando en ws://0.0.0.0:{port}")
        await asyncio.Future()


def main():
    ap = argparse.ArgumentParser(description="Mock del cloud (contrato V3)")
    ap.add_argument("-p", "--port", type=int, default=8080, help="puerto REST")
    ap.add_argument("-w", "--ws-port", type=int, default=8765, help="puerto WS")
    ap.add_argument("--no-demo", action="store_true",
                    help="no enviar los comandos de demostracion")
    ap.add_argument("--ping-s", type=int, default=WS_PING_DEFAULT_S,
                    help="intervalo del ping JSON (keep-alive)")
    ap.add_argument("--config-push-file", default=None, metavar="ARCHIVO",
                    help="enviar este JSON como config push tras el hello_ok")
    ap.add_argument("--require-apikey", default=None, metavar="CLAVE",
                    help="exigir este valor en el header apikey (o Bearer); "
                         "si no coincide: 401 (Fase 5 HIL)")
    ap.add_argument("--rest-delay-ms", type=int, default=0, metavar="N",
                    help="retardo de respuesta del POST /eventos (simular "
                         "servidor lento; Fase 5 HIL)")
    args = ap.parse_args()

    _load_seen_ids()
    httpd = ThreadingHTTPServer(("0.0.0.0", args.port), EventosHandler)
    httpd.require_apikey = args.require_apikey
    httpd.rest_delay_ms = args.rest_delay_ms
    log("REST", f"escuchando en http://0.0.0.0:{args.port} "
                f"(eventos en {EVENTS_FILE})")

    stop = threading.Event()

    def serve_http():
        while not stop.is_set():
            httpd.handle_request()

    t = threading.Thread(target=serve_http, daemon=True)
    t.start()

    try:
        asyncio.run(ws_server(args.ws_port, args))
    except KeyboardInterrupt:
        pass
    finally:
        stop.set()
        httpd.shutdown()


if __name__ == "__main__":
    main()