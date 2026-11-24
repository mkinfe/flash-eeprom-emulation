/**
 * @file    sim_flash.h
 * @brief   Host model of NOR flash with power-cut injection, for tests.
 *
 * Behaviour modelled on STM32L4/G4-type flash:
 *   - erase sets a whole page to 0xFF;
 *   - programming works on EE_UNIT_SIZE units, can only clear bits, and a unit may be
 *     programmed only once per erase (ECC) - a second program is counted as a violation;
 *   - a power cut during program leaves a random subset of the bits cleared, during erase a
 *     random subset of the bits set; after the cut every operation fails until power returns.
 */
#ifndef SIM_FLASH_H
#define SIM_FLASH_H

#include <stdbool.h>
#include <stdint.h>

#include "ee/eeprom_emul.h"

#define SIM_MAX_PAGES 8u

typedef struct {
    uint8_t *mem;
    uint32_t base;
    uint32_t size;
    uint32_t page_size;
    int32_t ops_until_cut; /**< < 0: no cut armed; otherwise operations that still complete. */
    bool power_lost;
    bool last_cut_in_erase; /**< The last cut interrupted an erase (otherwise a program). */
    uint32_t last_cut_addr; /**< Address of the unit / page whose operation was interrupted. */
    uint32_t rng;
    uint32_t erases[SIM_MAX_PAGES];
    uint32_t unit_programs;
    uint32_t write_once_violations;
} sim_flash_t;

void sim_flash_init(sim_flash_t *f, uint8_t *mem, uint32_t base, uint32_t size, uint32_t page_size, uint32_t seed);

/** Cut the power after `ops` more unit programs / page erases (the next one is interrupted). */
void sim_flash_arm_cut(sim_flash_t *f, int32_t ops);

/** Power is back: operations work again, no cut armed. */
void sim_flash_power_restore(sim_flash_t *f);

/** Driver table for ee_config_t. */
ee_flash_ops_t sim_flash_ops(sim_flash_t *f);

/** Small deterministic PRNG (xorshift32), shared with the tests. */
uint32_t sim_rand(uint32_t *state);

#endif /* SIM_FLASH_H */
