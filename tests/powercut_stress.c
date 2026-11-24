/* Power-cut stress test.
 *
 * Runs random writes and deletes against the simulated flash and cuts the power at random
 * points (inside record writes, page swaps and erases). After every cut the device "reboots"
 * (ee_init on the same flash) and every id is compared with a RAM model:
 *   - the id being written must hold its old value or its new value, never anything else;
 *   - every other id must be unchanged.
 *
 * Usage: powercut_stress [operations] [seed]
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ee/eeprom_emul.h"
#include "sim_flash.h"

#define PAGE     2048u
#define BASE     0x08000000u
#define IDS      24u
#define MAX_LEN  64u

typedef struct {
    int present;
    uint16_t len;
    uint8_t data[MAX_LEN];
} model_t;

static uint8_t g_mem[2u * PAGE];
static sim_flash_t g_sim;
static ee_config_t g_cfg;
static ee_t g_ee;
static model_t g_model[IDS];
static uint32_t g_rng;

static int matches(uint16_t id, const model_t *m)
{
    uint8_t buf[EE_MAX_DATA];
    uint16_t len = 0u;
    const ee_status_t st = ee_read(&g_ee, id, buf, sizeof buf, &len);

    if (!m->present) {
        return st == EE_ERR_NOT_FOUND;
    }
    return (st == EE_OK) && (len == m->len) && (memcmp(buf, m->data, len) == 0);
}

static int verify_all(uint16_t skip)
{
    for (uint16_t id = 0u; id < IDS; id++) {
        if ((id != skip) && !matches(id, &g_model[id])) {
            (void)printf("mismatch on id %u\n", id);
            return 0;
        }
    }
    return 1;
}

int main(int argc, char **argv)
{
    const unsigned long ops = (argc > 1) ? strtoul(argv[1], NULL, 10) : 100000ul;
    const uint32_t seed = (argc > 2) ? (uint32_t)strtoul(argv[2], NULL, 10) : 1u;
    unsigned long cuts = 0ul;
    unsigned long cuts_in_swap = 0ul;
    unsigned long cuts_in_erase = 0ul;
    unsigned long writes = 0ul;
    unsigned long deletes = 0ul;
    unsigned long new_after_cut = 0ul;
    unsigned long gc_total = 0ul;
    unsigned long erase_total = 0ul;

    g_rng = seed;
    sim_flash_init(&g_sim, g_mem, BASE, sizeof g_mem, PAGE, seed);
    g_cfg.flash = sim_flash_ops(&g_sim);
    g_cfg.page_addr[0] = BASE;
    g_cfg.page_addr[1] = BASE + PAGE;
    g_cfg.page_size = PAGE;
    if (ee_init(&g_ee, &g_cfg) != EE_OK) {
        (void)printf("init failed\n");
        return 1;
    }

    for (unsigned long n = 0ul; n < ops; n++) {
        const uint16_t id = (uint16_t)(sim_rand(&g_rng) % IDS);
        const int do_delete = g_model[id].present && ((sim_rand(&g_rng) % 8u) == 0u);
        const uint32_t active_base = g_cfg.page_addr[g_ee.active];
        model_t next;
        ee_status_t st;

        (void)memset(&next, 0, sizeof next);
        if (!do_delete) {
            next.present = 1;
            next.len = (uint16_t)(1u + (sim_rand(&g_rng) % MAX_LEN));
            for (uint16_t i = 0u; i < next.len; i++) {
                next.data[i] = (uint8_t)sim_rand(&g_rng);
            }
        }
        /* Every 4th operation gets a power cut: half of them early (inside a record write),
         * half of them late (inside a page swap: erase, copies, commit). */
        if ((sim_rand(&g_rng) % 4u) == 0u) {
            const uint32_t window = ((sim_rand(&g_rng) % 2u) == 0u) ? 12u : 400u;
            sim_flash_arm_cut(&g_sim, (int32_t)(sim_rand(&g_rng) % window));
        }

        st = do_delete ? ee_delete(&g_ee, id) : ee_write(&g_ee, id, next.data, next.len);

        if (g_sim.power_lost) {
            cuts++;
            /* A page swap writes only the other page; an append writes the active page. */
            cuts_in_swap += ((g_sim.last_cut_addr - active_base) >= PAGE) ? 1ul : 0ul;
            cuts_in_erase += g_sim.last_cut_in_erase ? 1ul : 0ul;
            gc_total += g_ee.stats.gc_runs;
            erase_total += g_ee.stats.page_erases;
            sim_flash_power_restore(&g_sim);
            if (ee_init(&g_ee, &g_cfg) != EE_OK) {
                (void)printf("FAIL: init after power cut %lu (op %lu)\n", cuts, n);
                return 1;
            }
            if (matches(id, &next)) {
                g_model[id] = next; /* the interrupted operation completed */
                new_after_cut++;
            }
            else if (!matches(id, &g_model[id])) {
                uint8_t buf[EE_MAX_DATA];
                uint16_t len = 0u;
                const ee_status_t rs = ee_read(&g_ee, id, buf, sizeof buf, &len);
                (void)printf("FAIL: id %u is neither old nor new after cut %lu (op %lu)\n", id, cuts, n);
                (void)printf("  op=%s st=%d read=%d len=%u | old present=%d len=%u | new present=%d len=%u\n",
                             do_delete ? "delete" : "write", (int)st, (int)rs, len, g_model[id].present,
                             g_model[id].len, next.present, next.len);
                (void)printf("  cut addr=0x%08X erase=%d active_before=0x%08X active_now=%u gen=%u needs_gc=%d wo=%u\n",
                             g_sim.last_cut_addr, g_sim.last_cut_in_erase, active_base, g_ee.active,
                             g_ee.generation, g_ee.needs_gc, g_ee.write_offset);
                if (rs == EE_OK) {
                    (void)printf("  same as old: %d, same as new: %d\n",
                                 (len == g_model[id].len) && (memcmp(buf, g_model[id].data, len) == 0),
                                 (len == next.len) && (memcmp(buf, next.data, len) == 0));
                }
                return 1;
            }
            if (!verify_all(id)) {
                (void)printf("FAIL: other ids changed after cut %lu (op %lu)\n", cuts, n);
                return 1;
            }
            continue;
        }
        sim_flash_power_restore(&g_sim); /* disarm a cut that did not trigger */
        if (st != EE_OK) {
            (void)printf("FAIL: op %lu returned %d without a power cut\n", n, (int)st);
            return 1;
        }
        g_model[id] = next;
        if (do_delete) {
            deletes++;
        }
        else {
            writes++;
        }
        if ((n % 1000ul) == 0ul) {
            gc_total += g_ee.stats.gc_runs;
            erase_total += g_ee.stats.page_erases;
            if ((ee_init(&g_ee, &g_cfg) != EE_OK) || !verify_all(EE_ID_INVALID)) {
                (void)printf("FAIL: clean reboot check at op %lu\n", n);
                return 1;
            }
        }
    }
    gc_total += g_ee.stats.gc_runs;
    erase_total += g_ee.stats.page_erases;

    if (g_sim.write_once_violations != 0u) {
        (void)printf("FAIL: %u program(s) on a non-erased unit\n", g_sim.write_once_violations);
        return 1;
    }
    (void)printf("operations        : %lu (writes %lu, deletes %lu, seed %u)\n", ops, writes, deletes, seed);
    (void)printf("power cuts        : %lu (in page swap %lu, of them in erase %lu; interrupted op completed %lu)\n",
                 cuts, cuts_in_swap, cuts_in_erase, new_after_cut);
    (void)printf("page swaps        : %lu\n", gc_total);
    (void)printf("page erases       : %lu counted by the library, %u done by the flash (page 0: %u, page 1: %u)\n",
                 erase_total, g_sim.erases[0] + g_sim.erases[1], g_sim.erases[0], g_sim.erases[1]);
    (void)printf("writes per erase  : %.1f\n", (double)(writes + deletes) / (double)(g_sim.erases[0] + g_sim.erases[1] + 1u));
    (void)printf("result            : PASS - no lost or mixed values, no write-once violation\n");
    return 0;
}
