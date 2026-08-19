#ifndef PORTAL_H
#define PORTAL_H

#include <stdbool.h>

// ============================================================================
// portal.h — Portal cautivo V3 (P1).
// AP local + DNS cautivo + servidor HTTP de configuracion:
//   - Formulario: WiFi, alias, zonas, avanzado (URL API default, URL WS,
//     apikey, tiempos lectura/subida, zonas simultaneas).
//   - Calibracion de humedad por zona en vivo (seco/humedo).
//   - Restablecimiento a valores de fabrica con confirmacion.
// El guardado valida y aplica la configuracion (store) y reinicia.
// ============================================================================

// Inicia el AP y los servidores DNS/HTTP. Llamar al entrar en modo CONFIG.
void portalStart();

// Atiende DNS y peticiones HTTP. Llamar en cada pasada del loop (modo CONFIG).
void portalHandle();

// true si el usuario guardo configuracion valida (main debe reiniciar).
bool portalSavedAndReadyToReboot();

#endif // PORTAL_H