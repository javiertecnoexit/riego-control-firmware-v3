# Riego Control - Firmware V3

Firmware del sistema de riego controlado para ESP32 DevKit V1 (WROOM).

Este repositorio entrega: **firmware**, **contrato normativo de integracion
cloud**, **mock de referencia** y **suite de conformidad** para que el equipo
de integracion pruebe su implementacion antes de conectarla al dispositivo.

## Estado del proyecto

- Plan vigente: `PLAN_V3.md` (revision 2, aprobado).
- Fase actual: Fase 0 (fundacion) - estructura, configuracion de build y
  decisiones registradas.
- No hay aun implementacion de firmware.

## Decisiones clave

| Tema | Decision |
|---|---|
| Transporte bidireccional | WebSocket (`wss://`) para comandos y configuracion en tiempo real |
| Subida de datos | POST JSON directo a PostgREST de Supabase (RLS desactivada, header `apikey`) |
| Seguridad | Minima: transporte cifrado sin CA + `apikey`. Sin MAC, sin tokens de dispositivo, sin JWT de usuario |
| Identidad | No hay registro por MAC; diferenciacion por `device_alias` configurable en el portal |
| Portal cautivo | WiFi, zonas, calibracion, avanzado (URL API por defecto, URL WS, apikey, tiempos) y restablecimiento a fabrica |
| Persistencia | Snapshots dobles de politica cloud + outbox critica en LittleFS (64 KB) |
| Control local | 100 % independiente de la nube; la red es una tarea aislada |

## Estructura

```text
src/         Firmware (hal, domain, runtime, persist, cloud, portal)
test/        Pruebas nativas (Unity)
docs/        Arquitectura, decisiones, contrato cloud, checklist, esquema DB
tools/       Mock de referencia y suite de conformidad
partitions/  Tabla de particiones (sin OTA + LittleFS)
```

## Build

```text
pio run                    Compilar firmware
pio test -e native         Pruebas nativas en PC
pio run -t upload          Cargar por USB (COM11, 921600)
```

Requisitos: PlatformIO Core con `espressif32@7.0.1` (ver `platformio.ini`).

## Documentacion

- `PLAN_V3.md`: plan maestro (contexto, alcance, arquitectura, fases, decisiones).
- `docs/README.md`: indice de la documentacion del repositorio.
- `docs/contrato_nube_v3.md`: contrato normativo para el equipo cloud (Fase 4).