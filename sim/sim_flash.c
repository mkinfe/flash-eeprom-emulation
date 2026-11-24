#include "sim_flash.h"

#include <string.h>

uint32_t sim_rand(uint32_t *state)
{
    uint32_t x = (*state != 0u) ? *state : 0x9E3779B9u;

    x ^= x << 13u;
    x ^= x >> 17u;
    x ^= x << 5u;
    *state = x;
    return x;
}

static bool in_range(const sim_flash_t *f, uint32_t addr, uint32_t len)
{
    return (addr >= f->base) && (len <= f->size) && ((addr - f->base) <= (f->size - len));
}

/* true when the power fails during this operation */
static bool cut_now(sim_flash_t *f)
{
    if (f->ops_until_cut < 0) {
        return false;
    }
    if (f->ops_until_cut == 0) {
        f->power_lost = true;
        return true;
    }
    f->ops_until_cut--;
    return false;
}

static ee_status_t sim_read(void *ctx, uint32_t addr, void *dst, uint32_t len)
{
    const sim_flash_t *f = ctx;

    if (!in_range(f, addr, len)) {
        return EE_ERR_FLASH;
    }
    (void)memcpy(dst, &f->mem[addr - f->base], len);
    return EE_OK;
}

static ee_status_t sim_program(void *ctx, uint32_t addr, const void *src, uint32_t len)
{
    sim_flash_t *f = ctx;
    const uint8_t *s = src;

    if (f->power_lost || !in_range(f, addr, len) || ((addr % EE_UNIT_SIZE) != 0u) || ((len % EE_UNIT_SIZE) != 0u)) {
        return EE_ERR_FLASH;
    }
    for (uint32_t u = 0u; u < len; u += EE_UNIT_SIZE) {
        uint8_t *m = &f->mem[addr - f->base + u];
        bool erased = true;

        for (uint32_t i = 0u; i < EE_UNIT_SIZE; i++) {
            erased = erased && (m[i] == 0xFFu);
        }
        if (cut_now(f)) {
            /* Interrupted: only some of the bits that should be cleared are cleared. */
            f->last_cut_in_erase = false;
            f->last_cut_addr = addr + u;
            for (uint32_t i = 0u; i < EE_UNIT_SIZE; i++) {
                const uint8_t clear = (uint8_t)(m[i] & (uint8_t)~s[u + i]);
                m[i] = (uint8_t)(m[i] & (uint8_t)~(clear & (uint8_t)sim_rand(&f->rng)));
            }
            return EE_ERR_FLASH;
        }
        if (!erased) {
            f->write_once_violations++;
            return EE_ERR_FLASH;
        }
        for (uint32_t i = 0u; i < EE_UNIT_SIZE; i++) {
            m[i] = (uint8_t)(m[i] & s[u + i]);
        }
        f->unit_programs++;
    }
    return EE_OK;
}

static ee_status_t sim_erase(void *ctx, uint32_t addr)
{
    sim_flash_t *f = ctx;
    uint8_t *m;

    if (f->power_lost || !in_range(f, addr, f->page_size) || (((addr - f->base) % f->page_size) != 0u)) {
        return EE_ERR_FLASH;
    }
    m = &f->mem[addr - f->base];
    if (cut_now(f)) {
        /* Interrupted: a random subset of the bits is already back at 1. */
        f->last_cut_in_erase = true;
        f->last_cut_addr = addr;
        for (uint32_t i = 0u; i < f->page_size; i++) {
            m[i] = (uint8_t)(m[i] | (uint8_t)sim_rand(&f->rng));
        }
        return EE_ERR_FLASH;
    }
    (void)memset(m, 0xFF, f->page_size);
    f->erases[((addr - f->base) / f->page_size) % SIM_MAX_PAGES]++;
    return EE_OK;
}

void sim_flash_init(sim_flash_t *f, uint8_t *mem, uint32_t base, uint32_t size, uint32_t page_size, uint32_t seed)
{
    (void)memset(f, 0, sizeof *f);
    f->mem = mem;
    f->base = base;
    f->size = size;
    f->page_size = page_size;
    f->ops_until_cut = -1;
    f->rng = seed;
    (void)memset(mem, 0xFF, size);
}

void sim_flash_arm_cut(sim_flash_t *f, int32_t ops)
{
    f->ops_until_cut = ops;
}

void sim_flash_power_restore(sim_flash_t *f)
{
    f->power_lost = false;
    f->ops_until_cut = -1;
}

ee_flash_ops_t sim_flash_ops(sim_flash_t *f)
{
    ee_flash_ops_t ops = { sim_read, sim_program, sim_erase, f };
    return ops;
}
