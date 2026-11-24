/**
 * @file    eeprom_emul.h
 * @brief   Power-loss-safe EEPROM emulation on NOR flash (key/value records, two pages).
 *
 * Values are stored as records (id, length, CRC, data) appended to the active flash page.
 * When the page is full, the latest value of every id is copied to the other page
 * (garbage collection) and that page becomes active. A power cut at any point leaves
 * every id with either its old or its new value - never a mix.
 */
#ifndef EE_EEPROM_EMUL_H
#define EE_EEPROM_EMUL_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Maximum number of different ids stored at the same time. */
#ifndef EE_MAX_KEYS
#define EE_MAX_KEYS 32u
#endif

/** Maximum value length in bytes. */
#ifndef EE_MAX_DATA
#define EE_MAX_DATA 256u
#endif

/** Smallest programmable flash unit in bytes; each unit is programmed at most once per erase. */
#define EE_UNIT_SIZE 8u

/** Reserved id (an erased flash unit reads as 0xFFFF). */
#define EE_ID_INVALID 0xFFFFu

typedef enum {
    EE_OK = 0,
    EE_ERR_PARAM,        /**< Invalid argument or configuration. */
    EE_ERR_NOT_INIT,     /**< ee_init() not called or failed. */
    EE_ERR_NOT_FOUND,    /**< No value stored for this id. */
    EE_ERR_BUFFER_SMALL, /**< Destination buffer smaller than the stored value. */
    EE_ERR_CRC,          /**< Stored value is corrupted. */
    EE_ERR_FULL,         /**< All live values together do not fit one page. */
    EE_ERR_KEYS_FULL,    /**< EE_MAX_KEYS different ids already stored. */
    EE_ERR_FLASH         /**< Flash driver error (program, erase or verify). */
} ee_status_t;

/** Flash driver: addresses are absolute; program() gets EE_UNIT_SIZE-aligned address and length. */
typedef struct {
    ee_status_t (*read)(void *ctx, uint32_t addr, void *dst, uint32_t len);
    ee_status_t (*program)(void *ctx, uint32_t addr, const void *src, uint32_t len);
    ee_status_t (*erase_page)(void *ctx, uint32_t addr);
    void *ctx;
} ee_flash_ops_t;

typedef struct {
    ee_flash_ops_t flash;
    uint32_t page_addr[2]; /**< Start address of the two flash pages (erase units). */
    uint32_t page_size;    /**< Page size in bytes, multiple of EE_UNIT_SIZE. */
} ee_config_t;

typedef struct {
    uint32_t page_erases;     /**< Page erases done by this instance. */
    uint32_t gc_runs;         /**< Garbage collections (page swaps). */
    uint32_t records_written; /**< Records programmed (values and deletions). */
    uint32_t writes_skipped;  /**< Writes skipped because the value was unchanged. */
    uint32_t crc_errors;      /**< Corrupted records found while reading or copying. */
} ee_stats_t;

typedef struct {
    uint16_t id;
    uint16_t len;
    uint32_t offset; /**< Record offset inside the active page. */
} ee_index_entry_t;

/** Instance state; treat as opaque (fields are visible for tests and debugging). */
typedef struct {
    ee_config_t cfg;
    bool initialised;
    bool needs_gc;         /**< A damaged tail was found: the next write swaps pages. */
    uint8_t active;        /**< Index (0/1) of the active page. */
    uint32_t generation;   /**< Increases with every page swap; the newer valid page wins. */
    uint32_t write_offset; /**< Next free offset in the active page. */
    uint32_t key_count;
    ee_index_entry_t index[EE_MAX_KEYS];
    uint8_t scratch[EE_MAX_DATA];
    ee_stats_t stats;
} ee_t;

/** Mount the emulation: selects the valid page, rebuilds the RAM index, formats blank flash. */
ee_status_t ee_init(ee_t *ee, const ee_config_t *cfg);

/** Erase both pages and start empty. */
ee_status_t ee_format(ee_t *ee);

/** Store a value (1..EE_MAX_DATA bytes); an unchanged value is not written again. */
ee_status_t ee_write(ee_t *ee, uint16_t id, const void *data, uint16_t len);

/** Read a value; *out_len (optional) receives the stored length. buf is untouched on error. */
ee_status_t ee_read(ee_t *ee, uint16_t id, void *buf, uint16_t buf_size, uint16_t *out_len);

/** Delete a value. */
ee_status_t ee_delete(ee_t *ee, uint16_t id);

/** Bytes left in the active page before the next garbage collection. */
uint32_t ee_free_space(const ee_t *ee);

#ifdef __cplusplus
}
#endif

#endif /* EE_EEPROM_EMUL_H */
