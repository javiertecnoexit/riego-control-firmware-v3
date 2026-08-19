#include <stdio.h>
#include <string.h>

#include <unity.h>

#include "config.h"
#include "store.h"

// ============================================================================
// Pruebas nativas de persistencia (Fase 2). Backend de archivos
// (STORE_BACKEND_FILE); un nuevo storeInit() simula un reinicio.
// ============================================================================

static const char* TMP_DIR = "store_test_tmp";
static const char* CUR_PATH = "store_test_tmp/cur";
static const char* PREV_PATH = "store_test_tmp/prev";

static DeviceConfig sampleConfig(uint32_t version) {
    DeviceConfig cfg = storeConfigDefaults();
    cfg.version = version;
    strncpy(cfg.ssid, "wifi-casa", sizeof(cfg.ssid) - 1);
    strncpy(cfg.wifiPass, "clave123", sizeof(cfg.wifiPass) - 1);
    strncpy(cfg.deviceAlias, "invernadero-1", sizeof(cfg.deviceAlias) - 1);
    strncpy(cfg.apiUrl, "https://proyecto.supabase.co/rest/v1",
            sizeof(cfg.apiUrl) - 1);
    strncpy(cfg.wsUrl, "wss://ws.proyecto.dev/v1/ws", sizeof(cfg.wsUrl) - 1);
    strncpy(cfg.apiKey, "supabase-key-abc-123", sizeof(cfg.apiKey) - 1);
    cfg.substrateZones = 3;
    cfg.sprinklerZones = 1;
    cfg.readIntervalS = 45;
    cfg.uploadIntervalS = 120;
    return cfg;
}

static void corruptFile(const char* path) {
    FILE* f = fopen(path, "wb");
    TEST_ASSERT_NOT_NULL(f);
    const char* garbage = "no-json-no-crc-basura-corrupta";
    fwrite(garbage, 1, strlen(garbage), f);
    fclose(f);
}

void setUp() {
    storeInit(TMP_DIR);
    storeFactoryReset();
}

void tearDown() {}

void test_defaults_are_preloaded_but_not_applicable() {
    const DeviceConfig cfg = storeConfigDefaults();
    char err[128];
    // Los defaults de fabrica NO son aplicables (sin SSID ni apikey) y se
    // pre-cargan en el portal (D12): la URL de la API llega con valor por
    // defecto para no tener que escribirla en cada placa nueva.
    TEST_ASSERT_FALSE(storeConfigValidate(cfg, err, sizeof(err)));
    TEST_ASSERT_EQUAL_STRING("https://amtyfuicltnebtcjxxld.supabase.co/rest/v1",
                             cfg.apiUrl);
    TEST_ASSERT_EQUAL_UINT8(DEFAULT_SUBSTRATE_ZONES, cfg.substrateZones);
    TEST_ASSERT_EQUAL_UINT8(DEFAULT_SPRINKLER_ZONES, cfg.sprinklerZones);
    TEST_ASSERT_EQUAL_UINT16(DEFAULT_READ_INTERVAL_S, cfg.readIntervalS);
    TEST_ASSERT_EQUAL_UINT16(DEFAULT_UPLOAD_INTERVAL_S, cfg.uploadIntervalS);

    // Sin snapshot valido, el boot cae en defaults (fail-open conservador).
    const DeviceConfig loaded = storeConfigLoad();
    TEST_ASSERT_EQUAL_UINT32(0, loaded.version);
    TEST_ASSERT_FALSE(storeHasValidConfig());
}

void test_apply_and_load_roundtrip() {
    const DeviceConfig cfg = sampleConfig(1);
    TEST_ASSERT_TRUE(storeConfigApply(cfg));

    storeInit(TMP_DIR);   // reinicio simulado
    const DeviceConfig loaded = storeConfigLoad();
    TEST_ASSERT_TRUE(storeHasValidConfig());
    TEST_ASSERT_EQUAL_UINT32(1, loaded.version);
    TEST_ASSERT_EQUAL_STRING("wifi-casa", loaded.ssid);
    TEST_ASSERT_EQUAL_STRING("clave123", loaded.wifiPass);
    TEST_ASSERT_EQUAL_STRING("invernadero-1", loaded.deviceAlias);
    TEST_ASSERT_EQUAL_STRING("https://proyecto.supabase.co/rest/v1",
                             loaded.apiUrl);
    TEST_ASSERT_EQUAL_STRING("wss://ws.proyecto.dev/v1/ws", loaded.wsUrl);
    TEST_ASSERT_EQUAL_STRING("supabase-key-abc-123", loaded.apiKey);
    TEST_ASSERT_EQUAL_UINT8(3, loaded.substrateZones);
    TEST_ASSERT_EQUAL_UINT8(1, loaded.sprinklerZones);
    TEST_ASSERT_EQUAL_UINT16(45, loaded.readIntervalS);
    TEST_ASSERT_EQUAL_UINT16(120, loaded.uploadIntervalS);
}

void test_corrupt_current_falls_back_to_previous() {
    TEST_ASSERT_TRUE(storeConfigApply(sampleConfig(1)));
    TEST_ASSERT_TRUE(storeConfigApply(sampleConfig(2)));

    corruptFile(CUR_PATH);
    const DeviceConfig loaded = storeConfigLoad();
    TEST_ASSERT_EQUAL_UINT32(1, loaded.version);   // prev = v1
    TEST_ASSERT_TRUE(storeHasValidConfig());
}

void test_both_corrupt_returns_defaults() {
    TEST_ASSERT_TRUE(storeConfigApply(sampleConfig(1)));
    corruptFile(CUR_PATH);
    corruptFile(PREV_PATH);

    const DeviceConfig loaded = storeConfigLoad();
    TEST_ASSERT_EQUAL_UINT32(0, loaded.version);
    TEST_ASSERT_FALSE(storeHasValidConfig());
}

void test_invalid_config_rejected_and_current_intact() {
    TEST_ASSERT_TRUE(storeConfigApply(sampleConfig(1)));

    DeviceConfig bad = sampleConfig(2);
    strncpy(bad.apiUrl, "ftp://nada", sizeof(bad.apiUrl) - 1);
    TEST_ASSERT_FALSE(storeConfigApply(bad));

    bad = sampleConfig(2);
    bad.substrateZones = 0;
    TEST_ASSERT_FALSE(storeConfigApply(bad));

    bad = sampleConfig(2);
    bad.apiKey[0] = '\0';
    TEST_ASSERT_FALSE(storeConfigApply(bad));

    bad = sampleConfig(2);
    bad.readIntervalS = 2;   // menor al minimo
    TEST_ASSERT_FALSE(storeConfigApply(bad));

    const DeviceConfig loaded = storeConfigLoad();
    TEST_ASSERT_EQUAL_UINT32(1, loaded.version);   // current intacto
}

void test_version_non_regressive() {
    TEST_ASSERT_TRUE(storeConfigApply(sampleConfig(5)));
    TEST_ASSERT_FALSE(storeConfigApply(sampleConfig(3)));
    TEST_ASSERT_EQUAL_UINT32(5, storeConfigLoad().version);
}

void test_outbox_append_count_ack() {
    TEST_ASSERT_TRUE(storeOutboxAppend("{\"client_id\":1,\"type\":\"a\"}"));
    TEST_ASSERT_TRUE(storeOutboxAppend("{\"client_id\":2,\"type\":\"b\"}"));
    TEST_ASSERT_TRUE(storeOutboxAppend("{\"client_id\":3,\"type\":\"c\"}"));
    TEST_ASSERT_EQUAL_UINT32(3, (uint32_t)storeOutboxCount());

    TEST_ASSERT_TRUE(storeOutboxAckUpTo(2));
    TEST_ASSERT_EQUAL_UINT32(1, (uint32_t)storeOutboxCount());

    TEST_ASSERT_TRUE(storeOutboxAckUpTo(100));
    TEST_ASSERT_EQUAL_UINT32(0, (uint32_t)storeOutboxCount());
}

void test_outbox_keeps_lines_without_client_id() {
    // Una linea sin client_id se trata como id 0: cualquier acuse >= 0 la
    // elimina (no puede confirmarse). Las lineas con id mayor al watermark
    // se conservan.
    TEST_ASSERT_TRUE(storeOutboxAppend("{\"client_id\":5,\"type\":\"x\"}"));
    TEST_ASSERT_TRUE(storeOutboxAppend("{\"not_an_event\":true}"));
    TEST_ASSERT_TRUE(storeOutboxAckUpTo(4));
    TEST_ASSERT_EQUAL_UINT32(1, (uint32_t)storeOutboxCount());
    TEST_ASSERT_TRUE(storeOutboxAckUpTo(5));
    TEST_ASSERT_EQUAL_UINT32(0, (uint32_t)storeOutboxCount());
}

void test_outbox_persists_across_reinit() {
    TEST_ASSERT_TRUE(storeOutboxAppend("{\"client_id\":7,\"type\":\"e\"}"));
    storeInit(TMP_DIR);   // reinicio simulado
    TEST_ASSERT_EQUAL_UINT32(1, (uint32_t)storeOutboxCount());
    TEST_ASSERT_TRUE(storeOutboxAckUpTo(7));
    TEST_ASSERT_EQUAL_UINT32(0, (uint32_t)storeOutboxCount());
}

void test_outbox_cap_trims_oldest() {
    char line[48];
    for (uint16_t i = 0; i < 600; ++i) {
        snprintf(line, sizeof(line), "{\"client_id\":%u,\"type\":\"e\"}", i);
        TEST_ASSERT_TRUE(storeOutboxAppend(line));
    }
    TEST_ASSERT_TRUE(storeOutboxCount() <= STORE_OUTBOX_MAX_LINES);
    TEST_ASSERT_TRUE(storeOutboxUsedBytes() <= STORE_OUTBOX_MAX_BYTES);

    // Las ultimas lineas sobreviven al recorte.
    TEST_ASSERT_TRUE(storeOutboxAckUpTo(599));
    TEST_ASSERT_EQUAL_UINT32(0, (uint32_t)storeOutboxCount());
}

void test_factory_reset_clears_everything() {
    TEST_ASSERT_TRUE(storeConfigApply(sampleConfig(9)));
    TEST_ASSERT_TRUE(storeOutboxAppend("{\"client_id\":1,\"type\":\"e\"}"));
    TEST_ASSERT_TRUE(storeHasValidConfig());

    storeFactoryReset();

    TEST_ASSERT_FALSE(storeHasValidConfig());
    TEST_ASSERT_EQUAL_UINT32(0, (uint32_t)storeConfigLoad().version);
    TEST_ASSERT_EQUAL_UINT32(0, (uint32_t)storeOutboxCount());
}

int main(int, char**) {
    UNITY_BEGIN();
    RUN_TEST(test_defaults_are_preloaded_but_not_applicable);
    RUN_TEST(test_apply_and_load_roundtrip);
    RUN_TEST(test_corrupt_current_falls_back_to_previous);
    RUN_TEST(test_both_corrupt_returns_defaults);
    RUN_TEST(test_invalid_config_rejected_and_current_intact);
    RUN_TEST(test_version_non_regressive);
    RUN_TEST(test_outbox_append_count_ack);
    RUN_TEST(test_outbox_keeps_lines_without_client_id);
    RUN_TEST(test_outbox_persists_across_reinit);
    RUN_TEST(test_outbox_cap_trims_oldest);
    RUN_TEST(test_factory_reset_clears_everything);
    return UNITY_END();
}