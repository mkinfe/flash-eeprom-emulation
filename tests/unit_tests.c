/* Unit tests: API behaviour, page swap, persistence and recovery from damaged flash. */
#include <string.h>

#include "ee/eeprom_emul.h"
#include "sim_flash.h"
#include "test_util.h"

#define PAGE  2048u
#define BASE  0x08000000u

static uint8_t g_mem[2u * PAGE];
static sim_flash_t g_sim;
static ee_config_t g_cfg;
static ee_t g_ee;

static void setup(void)
{
    sim_flash_init(&g_sim, g_mem, BASE, sizeof g_mem, PAGE, 1u);
    g_cfg.flash = sim_flash_ops(&g_sim);
    g_cfg.page_addr[0] = BASE;
    g_cfg.page_addr[1] = BASE + PAGE;
    g_cfg.page_size = PAGE;
    CHECK(ee_init(&g_ee, &g_cfg) == EE_OK);
}

static void reboot(void)
{
    CHECK(ee_init(&g_ee, &g_cfg) == EE_OK);
}

static void fill(uint8_t *buf, uint16_t len, uint8_t seed)
{
    for (uint16_t i = 0u; i < len; i++) {
        buf[i] = (uint8_t)(seed + (uint8_t)(i * 7u));
    }
}

static int value_is(uint16_t id, const uint8_t *expected, uint16_t len)
{
    uint8_t buf[EE_MAX_DATA];
    uint16_t out = 0u;

    return (ee_read(&g_ee, id, buf, sizeof buf, &out) == EE_OK) && (out == len) && (memcmp(buf, expected, len) == 0);
}

static void test_blank_flash_is_formatted(void)
{
    setup();
    CHECK(g_ee.generation == 1u);
    CHECK(g_ee.key_count == 0u);
    CHECK(ee_free_space(&g_ee) == PAGE - 16u);
    CHECK(g_sim.erases[0] == 0u); /* blank pages are not erased again */
}

static void test_write_then_read(void)
{
    const uint8_t v[5] = { 1, 2, 3, 4, 5 };

    setup();
    CHECK(ee_write(&g_ee, 10u, v, sizeof v) == EE_OK);
    CHECK(value_is(10u, v, sizeof v));
}

static void test_overwrite_returns_latest(void)
{
    uint8_t a[20];
    uint8_t b[3];

    setup();
    fill(a, sizeof a, 1u);
    fill(b, sizeof b, 9u);
    CHECK(ee_write(&g_ee, 1u, a, sizeof a) == EE_OK);
    CHECK(ee_write(&g_ee, 1u, b, sizeof b) == EE_OK);
    CHECK(value_is(1u, b, sizeof b));
}

static void test_unchanged_value_not_written(void)
{
    const uint8_t v[8] = { 7, 7, 7, 7, 7, 7, 7, 7 };

    setup();
    CHECK(ee_write(&g_ee, 3u, v, sizeof v) == EE_OK);
    CHECK(ee_write(&g_ee, 3u, v, sizeof v) == EE_OK);
    CHECK(g_ee.stats.records_written == 1u);
    CHECK(g_ee.stats.writes_skipped == 1u);
}

static void test_parameter_errors(void)
{
    uint8_t buf[4] = { 0 };
    const uint8_t big[EE_MAX_DATA + 1u] = { 0 };
    ee_t fresh;

    setup();
    (void)memset(&fresh, 0, sizeof fresh);
    CHECK(ee_write(&fresh, 1u, buf, 1u) == EE_ERR_NOT_INIT);
    CHECK(ee_write(&g_ee, EE_ID_INVALID, buf, 1u) == EE_ERR_PARAM);
    CHECK(ee_write(&g_ee, 1u, NULL, 1u) == EE_ERR_PARAM);
    CHECK(ee_write(&g_ee, 1u, buf, 0u) == EE_ERR_PARAM);
    CHECK(ee_write(&g_ee, 1u, big, (uint16_t)sizeof big) == EE_ERR_PARAM);
    CHECK(ee_read(&g_ee, 1u, buf, sizeof buf, NULL) == EE_ERR_NOT_FOUND);
    CHECK(ee_delete(&g_ee, 1u) == EE_ERR_NOT_FOUND);
}

static void test_buffer_too_small_leaves_buffer_untouched(void)
{
    const uint8_t v[10] = { 1, 2, 3, 4, 5, 6, 7, 8, 9, 10 };
    uint8_t buf[4] = { 0xAA, 0xAA, 0xAA, 0xAA };
    uint16_t len = 0u;

    setup();
    CHECK(ee_write(&g_ee, 2u, v, sizeof v) == EE_OK);
    CHECK(ee_read(&g_ee, 2u, buf, sizeof buf, &len) == EE_ERR_BUFFER_SMALL);
    CHECK(len == sizeof v);
    CHECK(buf[0] == 0xAAu);
}

static void test_delete(void)
{
    const uint8_t v[4] = { 4, 3, 2, 1 };
    uint8_t buf[4];

    setup();
    CHECK(ee_write(&g_ee, 5u, v, sizeof v) == EE_OK);
    CHECK(ee_delete(&g_ee, 5u) == EE_OK);
    CHECK(ee_read(&g_ee, 5u, buf, sizeof buf, NULL) == EE_ERR_NOT_FOUND);
    reboot();
    CHECK(ee_read(&g_ee, 5u, buf, sizeof buf, NULL) == EE_ERR_NOT_FOUND);
}

static void test_values_survive_reboot(void)
{
    uint8_t v[3][40];

    setup();
    for (uint8_t i = 0u; i < 3u; i++) {
        fill(v[i], sizeof v[i], (uint8_t)(i * 50u));
        CHECK(ee_write(&g_ee, (uint16_t)(100u + i), v[i], sizeof v[i]) == EE_OK);
    }
    reboot();
    for (uint8_t i = 0u; i < 3u; i++) {
        CHECK(value_is((uint16_t)(100u + i), v[i], sizeof v[i]));
    }
}

static void test_page_swap_keeps_latest_values(void)
{
    uint8_t keep[30];
    uint8_t v[64];

    setup();
    fill(keep, sizeof keep, 3u);
    CHECK(ee_write(&g_ee, 1u, keep, sizeof keep) == EE_OK);
    for (uint16_t n = 0u; n < 200u; n++) {
        fill(v, sizeof v, (uint8_t)n);
        CHECK(ee_write(&g_ee, 2u, v, sizeof v) == EE_OK);
    }
    CHECK(g_ee.stats.gc_runs >= 5u);
    CHECK(value_is(1u, keep, sizeof keep));
    CHECK(value_is(2u, v, sizeof v));
    reboot(); /* both pages are valid now: the newer generation must win */
    CHECK(value_is(1u, keep, sizeof keep));
    CHECK(value_is(2u, v, sizeof v));
    CHECK(g_sim.write_once_violations == 0u);
}

static void test_keys_full(void)
{
    const uint8_t v[1] = { 1 };

    setup();
    for (uint16_t id = 0u; id < EE_MAX_KEYS; id++) {
        CHECK(ee_write(&g_ee, id, v, sizeof v) == EE_OK);
    }
    CHECK(ee_write(&g_ee, (uint16_t)EE_MAX_KEYS, v, sizeof v) == EE_ERR_KEYS_FULL);
    CHECK(ee_delete(&g_ee, 0u) == EE_OK);
    CHECK(ee_write(&g_ee, (uint16_t)EE_MAX_KEYS, v, sizeof v) == EE_OK);
}

static void test_page_full_keeps_old_values(void)
{
    uint8_t v[EE_MAX_DATA];
    uint16_t id = 0u;
    ee_status_t st = EE_OK;

    setup();
    fill(v, sizeof v, 0x55u);
    while ((st == EE_OK) && (id < 20u)) {
        st = ee_write(&g_ee, id, v, sizeof v);
        id++;
    }
    CHECK(st == EE_ERR_FULL);
    for (uint16_t i = 0u; i + 1u < id; i++) {
        CHECK(value_is(i, v, sizeof v));
    }
    reboot();
    CHECK(value_is(0u, v, sizeof v));
}

static void test_interrupted_commit_keeps_old_value(void)
{
    const uint8_t v_old[8] = { 1, 1, 1, 1, 1, 1, 1, 1 };
    const uint8_t v_new[8] = { 2, 2, 2, 2, 2, 2, 2, 2 };

    setup();
    CHECK(ee_write(&g_ee, 1u, v_old, sizeof v_old) == EE_OK);
    sim_flash_arm_cut(&g_sim, 2); /* header and data unit complete, commit unit interrupted */
    CHECK(ee_write(&g_ee, 1u, v_new, sizeof v_new) != EE_OK);
    CHECK(g_sim.power_lost);
    sim_flash_power_restore(&g_sim);
    reboot();
    CHECK(value_is(1u, v_old, sizeof v_old));
    CHECK(!g_ee.needs_gc); /* the uncommitted record is skipped, the log continues after it */
    CHECK(ee_write(&g_ee, 1u, v_new, sizeof v_new) == EE_OK);
    CHECK(value_is(1u, v_new, sizeof v_new));
    reboot();
    CHECK(value_is(1u, v_new, sizeof v_new));
    CHECK(g_sim.write_once_violations == 0u);
}

static void test_damaged_tail_forces_page_swap(void)
{
    const uint8_t v[16] = { 9 };
    const uint8_t w[16] = { 8 };
    uint32_t off;

    setup();
    CHECK(ee_write(&g_ee, 1u, v, sizeof v) == EE_OK);
    off = g_ee.write_offset;
    /* An interrupted header write: some bits cleared, header CRC wrong. */
    g_mem[(g_ee.cfg.page_addr[g_ee.active] - BASE) + off] = 0x12u;
    g_mem[(g_ee.cfg.page_addr[g_ee.active] - BASE) + off + 3u] = 0x00u;
    reboot();
    CHECK(g_ee.needs_gc);
    CHECK(value_is(1u, v, sizeof v));
    CHECK(ee_write(&g_ee, 2u, w, sizeof w) == EE_OK);
    CHECK(g_ee.stats.gc_runs == 1u);
    CHECK(value_is(1u, v, sizeof v) && value_is(2u, w, sizeof w));
    CHECK(g_sim.write_once_violations == 0u);
}

static void test_corrupted_value_reports_crc_error(void)
{
    const uint8_t v[8] = { 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF };
    uint8_t buf[8];

    setup();
    CHECK(ee_write(&g_ee, 1u, v, sizeof v) == EE_OK);
    g_mem[(g_ee.cfg.page_addr[g_ee.active] - BASE) + g_ee.index[0].offset + EE_UNIT_SIZE] = 0x7Fu; /* bit flip */
    CHECK(ee_read(&g_ee, 1u, buf, sizeof buf, NULL) == EE_ERR_CRC);
}

static void test_bad_configuration(void)
{
    ee_config_t bad;

    setup();
    bad = g_cfg;
    bad.page_size = 100u; /* not a multiple of the unit, too small */
    CHECK(ee_init(&g_ee, &bad) == EE_ERR_PARAM);
    bad = g_cfg;
    bad.page_addr[1] = BASE + 8u; /* overlapping pages */
    CHECK(ee_init(&g_ee, &bad) == EE_ERR_PARAM);
    bad = g_cfg;
    bad.flash.erase_page = NULL;
    CHECK(ee_init(&g_ee, &bad) == EE_ERR_PARAM);
}

int main(void)
{
    RUN(test_blank_flash_is_formatted);
    RUN(test_write_then_read);
    RUN(test_overwrite_returns_latest);
    RUN(test_unchanged_value_not_written);
    RUN(test_parameter_errors);
    RUN(test_buffer_too_small_leaves_buffer_untouched);
    RUN(test_delete);
    RUN(test_values_survive_reboot);
    RUN(test_page_swap_keeps_latest_values);
    RUN(test_keys_full);
    RUN(test_page_full_keeps_old_values);
    RUN(test_interrupted_commit_keeps_old_value);
    RUN(test_damaged_tail_forces_page_swap);
    RUN(test_corrupted_value_reports_crc_error);
    RUN(test_bad_configuration);
    (void)printf("\n%d checks, %d failed\n", g_checks, g_failures);
    return (g_failures == 0) ? 0 : 1;
}
