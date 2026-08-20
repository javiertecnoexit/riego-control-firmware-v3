#!/usr/bin/env python3
"""Supervisor del mock de referencia (HIL / Fase 4).

Mantiene mock_cloud.py vivo: si el proceso sale por cualquier motivo, lo
relanza. Registra en stdout la hora de cada arranque/salida y el codigo de
salida, para distinguir una muerte externa (0) de un crash (!= 0).

Uso:
    python tools/mock/supervisor.py -- [args de mock_cloud.py]

Ejemplo:
    python tools/mock/supervisor.py -- -p 8081 -w 8766 --ping-s 20
    python tools/mock/supervisor.py -- -p 8091 -w 8767 --ping-s 20 --no-demo

Para salir limpio: Ctrl+C.
"""

import subprocess
import sys
import time
from datetime import datetime

MIN_RESTART_S = 2.0
MAX_RESTART_S = 60.0
FAST_EXIT_S = 3.0


def now():
    return datetime.now().strftime("%H:%M:%S")


def main():
    if "--" not in sys.argv or len(sys.argv) == 1:
        print("Uso: supervisor.py -- [args de mock_cloud.py]", flush=True)
        return 1
    args = sys.argv[sys.argv.index("--") + 1 :]

    fast_exits = 0
    while True:
        print(f"[{now()}] SUPERVISOR: arrancando mock_cloud.py {' '.join(args)}",
              flush=True)
        start = time.monotonic()
        try:
            rc = subprocess.call([sys.executable, "mock_cloud.py"] + args,
                                 cwd=None)
        except KeyboardInterrupt:
            print(f"[{now()}] SUPERVISOR: Ctrl+C, saliendo", flush=True)
            return 0
        elapsed = time.monotonic() - start
        print(f"[{now()}] SUPERVISOR: mock salio codigo={rc} "
              f"tras {elapsed:.1f}s", flush=True)
        if elapsed < FAST_EXIT_S:
            fast_exits += 1
        else:
            fast_exits = 0
        wait = min(MIN_RESTART_S * (2 ** min(fast_exits, 5)), MAX_RESTART_S)
        print(f"[{now()}] SUPERVISOR: reintento en {wait:.0f}s", flush=True)
        time.sleep(wait)


if __name__ == "__main__":
    sys.exit(main())