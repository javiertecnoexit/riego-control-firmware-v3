#ifndef CONFIG_H
#define CONFIG_H

#include <stdint.h>

// ============================================================================
// config.h — Pines, constantes y valores de fabrica.
// Hardware y usabilidad identicos a V2 (P7); estructura reducida (P8).
// ============================================================================

// ----------------------------------------------------------------------------
// Pines — botones
// ----------------------------------------------------------------------------
#define PIN_BTN_SELECTOR       25   // Selector (INPUT_PULLUP)
#define PIN_BTN_CONFIRM        36   // Confirmar (INPUT, pull-up externo)
#define PIN_BTN_CONFIG         39   // Configuración (INPUT, pull-up externo)

// ----------------------------------------------------------------------------
// Pines — I2C (OLED y RTC)
// ----------------------------------------------------------------------------
#define PIN_I2C_SDA            21
#define PIN_I2C_SCL            22

// ----------------------------------------------------------------------------
// Pines — reles
// ----------------------------------------------------------------------------
#define PIN_RELAY_SUBSTRATE_1  26
#define PIN_RELAY_SUBSTRATE_2  27
#define PIN_RELAY_SUBSTRATE_3  14
#define PIN_RELAY_SUBSTRATE_4  17
#define PIN_RELAY_SPRINKLER_1  18
#define PIN_RELAY_SPRINKLER_2  19
#define PIN_RELAY_ALARM        23

// ----------------------------------------------------------------------------
// Pines — sensores
// ----------------------------------------------------------------------------
#define PIN_SOIL_ADC_1         34
#define PIN_SOIL_ADC_2         35
#define PIN_SOIL_ADC_3         32
#define PIN_SOIL_ADC_4         33
#define PIN_SENSOR_POWER       13
#define PIN_ONEWIRE            16
#define PIN_DHT21              4
#define AIR_SENSOR_MODEL       1    // 1 = DHT11 temporal, 2 = DHT21 final
#define MAX_SUBSTRATE_ZONES    4
#define MAX_DS18B20            4
#define MAX_SPRINKLER_ZONES    2

#define SOIL_STABILIZE_MS      25UL
#define SOIL_ADC_SAMPLES       12
#define SOIL_DEFAULT_DRY_RAW   3000
#define SOIL_DEFAULT_WET_RAW   1200

// ----------------------------------------------------------------------------
// Modos de operación
// ----------------------------------------------------------------------------
enum class OperationMode : uint8_t {
    NORMAL = 0,
    CONFIG = 1
};

// ----------------------------------------------------------------------------
// Ciclo local (default 30 s; configurable en el portal avanzado, P1)
// ----------------------------------------------------------------------------
#define LOCAL_CYCLE_MS         30000UL

// ----------------------------------------------------------------------------
// Hora local para pantalla (UTC-3 fijo, Argentina sin DST)
// ----------------------------------------------------------------------------
#define DISPLAY_TZ_OFFSET_HOURS (-3)

// ----------------------------------------------------------------------------
// Modo Configuración — portal local
// ----------------------------------------------------------------------------
#define AP_SSID_PREFIX         "RiegoControl-"
#define AP_PASSWORD            ""
#define CONFIG_TIMEOUT_MS      900000UL  // 15 min sin actividad -> reiniciar
#define CONFIG_WARN_MS         780000UL

// ----------------------------------------------------------------------------
// Valores de fabrica
// ----------------------------------------------------------------------------
#define DEFAULT_SUBSTRATE_ZONES 2
#define DEFAULT_SPRINKLER_ZONES 1
#define DEFAULT_IRRIGATION_S   60
#define DEFAULT_ALARM_MAX_TEMP  45.0f
#define DEFAULT_ALARM_MIN_TEMP  0.0f
#define DEFAULT_MIN_HUMIDITY    20
#define DEFAULT_READ_INTERVAL_S 30
#define DEFAULT_UPLOAD_INTERVAL_S 60

// ----------------------------------------------------------------------------
// Portal avanzado (P1, D12): URLs por defecto pre-cargadas
// ----------------------------------------------------------------------------
// Subida PostgREST directa (P6): base de la API REST de Supabase.
#define DEFAULT_API_URL        "https://amtyfuicltnebtcjxxld.supabase.co/rest/v1"
// Endpoint WebSocket a definir por el equipo cloud (queda vacio hasta Fase 3).
#define DEFAULT_WS_URL         ""

// ----------------------------------------------------------------------------
// Riego, alarmas
// ----------------------------------------------------------------------------
#define MANUAL_TIMEOUT_MS       5000UL   // 5 s sin botones en seleccion manual
#define MAX_IRRIGATION_EVENTS   32
#define MAX_SCHEDULES_PER_ZONE  4
#define SOIL_HUMIDITY_PERSISTENT_CYCLES  120  // 120 ciclos = 60 min
#define ALARM_NO_CONNECTION_SEC   3600UL

// ----------------------------------------------------------------------------
// Capa de red (Fase 3)
// ----------------------------------------------------------------------------
#define CLOUD_CYCLE_DEFAULT_MS  300000UL
#define CLOUD_BACKOFF_BASE_S    30
#define CLOUD_BACKOFF_MAX_MS    1800000UL
#define CLOUD_HTTP_TIMEOUT_MS   10000UL
#define MAX_TELEMETRY_READINGS  128

// ----------------------------------------------------------------------------
// Relé — polaridad
// ----------------------------------------------------------------------------
#define RELAY_ACTIVE_LOW        true

// ----------------------------------------------------------------------------
// Alarmas (condiciones visibles en pantalla)
// ----------------------------------------------------------------------------
enum class AlarmCondition : uint8_t {
    NONE                            = 0,
    AIR_TEMP_HIGH                   = 1,
    AIR_TEMP_LOW                    = 2,
    NO_CONNECTION_60MIN             = 3,
    SOIL_HUMIDITY_PERSISTENT_LOW    = 4
};

#endif // CONFIG_H