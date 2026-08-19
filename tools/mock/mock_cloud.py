#!/usr/bin/env python3
"""Mock de referencia del cloud (contrato V3) — Fases 3/4.

Simula el backend para probar el firmware sin dependencias externas:
  REST : http://0.0.0.0:8080
         POST /eventos  -> recibe el lote JSON (array), lo guarda y responde 201
         GET  /health   -> {"ok": true}
  WS   : ws://0.0.0.0:8765
         hello -> hello_ok (con protocol_version)
         ping  -> pong (texto)
         Envia un comando de demostracion unos segundos despues de conectar:
           {"type":"command","command_id":1,"zone":"S0","action":"on","duration_s":10}
           y luego el apagado del mismo comando.

Uso:
    python tools/mock/mock_cloud.py            # puertos 8080 y 8765
    python tools/mock/mock_cloud.py -p 9000 -w 9001

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


def log(tag, msg):
    print(f"[{datetime.now().strftime('%H:%M:%S')}] {tag}: {msg}", flush=True)


class EventosHandler(BaseHTTPRequestHandler):
    def _reply(self, code, body=b""):
        self.send_response(code)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        if body:
            self.wfile.write(body)

    def do_GET(self):
        if self.path == "/health":
            self._reply(200, b'{"ok":true,"mock":"riego-control-v3"}')
        else:
            self._reply(404, b'{"error":"not_found"}')

    def do_POST(self):
        if self.path != "/eventos":
            self._reply(404, b'{"error":"not_found"}')
            return
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
        with open(EVENTS_FILE, "a", encoding="utf-8") as f:
            for ev in events:
                f.write(json.dumps(ev, ensure_ascii=False) + "\n")
        log("REST", f"POST /eventos: {len(events)} eventos aceptados")
        self._reply(201, b'{"inserted":' + str(len(events)).encode() + b"}")


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


async def ws_handler(ws):
    log("WS", f"conexion desde {ws.remote_address}")
    demo = asyncio.create_task(ws_demo_commands(ws))
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
            else:
                log("WS", f"mensaje tipo {t}: {message[:120]}")
    finally:
        demo.cancel()
        log("WS", "conexion cerrada")


async def ws_server(port):
    async with websockets.serve(ws_handler, "0.0.0.0", port, ping_interval=20):
        log("WS", f"escuchando en ws://0.0.0.0:{port}")
        await asyncio.Future()


def main():
    ap = argparse.ArgumentParser(description="Mock del cloud (contrato V3)")
    ap.add_argument("-p", "--port", type=int, default=8080, help="puerto REST")
    ap.add_argument("-w", "--ws-port", type=int, default=8765, help="puerto WS")
    args = ap.parse_args()

    httpd = ThreadingHTTPServer(("0.0.0.0", args.port), EventosHandler)
    log("REST", f"escuchando en http://0.0.0.0:{args.port} "
                f"(eventos en {EVENTS_FILE})")

    stop = threading.Event()

    def serve_http():
        while not stop.is_set():
            httpd.handle_request()

    t = threading.Thread(target=serve_http, daemon=True)
    t.start()

    try:
        asyncio.run(ws_server(args.ws_port))
    except KeyboardInterrupt:
        pass
    finally:
        stop.set()
        httpd.shutdown()


if __name__ == "__main__":
    main()