/**
 * @file    eeprom_emul.c
 * @brief   Power-loss-safe EEPROM emulation on NOR flash.
 *
 * Page layout (offsets inside a page):
 *   0x00  RECEIVE unit : magic "1PEE", generation      - written when a page starts to be filled
 *   0x08  VALID unit   : magic "VALD", ~generation     - written when the page is complete
 *   0x10  records ...  : appended, each one aligned to EE_UNIT_SIZE
 *
 * Record:
 *   unit 0  id (u16) | len (u16) | data_crc (u16) | hdr_crc (u16)   - little endian
 *   unit 1+ data, last unit padded with 0xFF; len 0 = deletion (tombstone)
 *   last    commit unit: magic "CMIT" | data_crc | hdr_crc           - written last
 *   data_crc covers id, len and data; hdr_crc covers the first 6 header bytes.
 *
 * Power-loss rules:
 *   - A page is used only after its VALID unit is written; of two valid pages the higher
 *     generation wins, so a swap takes effect in one unit write.
 *   - A record counts only when its commit unit matches exactly. An interrupted program
 *     leaves some bits uncleared, so a half-written commit unit never matches - unlike a
 *     CRC, which a half-written data unit can pass by chance (1 in 65536).
 *   - The header is written first so the log can always be walked: an interrupted header
 *     write ends the log and forces a page swap; an uncommitted record is skipped.
 */
#include "ee/eeprom_emul.h"

#include <string.h>

#include "ee_crc.h"

#define EE_MAGIC_RECEIVE    0x45455031u /* "1PEE" */
#define EE_MAGIC_VALID      0x56414C44u /* "DLAV" */
#define EE_MAGIC_COMMIT     0x54494D43u /* "CMIT" */
#define EE_OFF_RECEIVE      0u
#define EE_OFF_VALID        EE_UNIT_SIZE
#define EE_OFF_FIRST_RECORD (2u * EE_UNIT_SIZE)
#define EE_ERASED           0xFFu
#define EE_INDEX_NONE       0xFFFFFFFFu
#define EE_CHUNK            32u

static uint32_t round_up_unit(uint32_t n)
{
    return (n + EE_UNIT_SIZE - 1u) & ~(EE_UNIT_SIZE - 1u);
}

static uint32_t record_size(uint16_t len)
{
    return EE_UNIT_SIZE + round_up_unit(len) + EE_UNIT_SIZE;
}

static uint32_t commit_offset(uint32_t off, uint16_t len)
{
    return off + EE_UNIT_SIZE + round_up_unit(len);
}

static void put_u16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)(v & 0xFFu);
    p[1] = (uint8_t)(v >> 8u);
}

static void put_u32(uint8_t *p, uint32_t v)
{
    put_u16(p, (uint16_t)(v & 0xFFFFu));
    put_u16(&p[2], (uint16_t)(v >> 16u));
}

static uint16_t get_u16(const uint8_t *p)
{
    return (uint16_t)((uint16_t)p[0] | (uint16_t)((uint16_t)p[1] << 8u));
}

static uint32_t get_u32(const uint8_t *p)
{
    return (uint32_t)get_u16(p) | ((uint32_t)get_u16(&p[2]) << 16u);
}

static bool is_erased(const uint8_t *p, uint32_t len)
{
    for (uint32_t i = 0u; i < len; i++) {
        if (p[i] != EE_ERASED) {
            return false;
        }
    }
    return true;
}

/* ------------------------------------------------------------------ flash access */

static ee_status_t fl_read(const ee_t *ee, uint8_t page, uint32_t off, void *dst, uint32_t len)
{
    const ee_flash_ops_t *f = &ee->cfg.flash;
    return (f->read(f->ctx, ee->cfg.page_addr[page] + off, dst, len) == EE_OK) ? EE_OK : EE_ERR_FLASH;
}

static ee_status_t fl_program(const ee_t *ee, uint8_t page, uint32_t off, const void *src, uint32_t len)
{
    const ee_flash_ops_t *f = &ee->cfg.flash;
    return (f->program(f->ctx, ee->cfg.page_addr[page] + off, src, len) == EE_OK) ? EE_OK : EE_ERR_FLASH;
}

static ee_status_t page_is_blank(const ee_t *ee, uint8_t page, bool *blank)
{
    uint8_t buf[EE_CHUNK];

    *blank = true;
    for (uint32_t off = 0u; (off < ee->cfg.page_size) && *blank; off += EE_CHUNK) {
        const uint32_t n = ((ee->cfg.page_size - off) < EE_CHUNK) ? (ee->cfg.page_size - off) : EE_CHUNK;
        if (fl_read(ee, page, off, buf, n) != EE_OK) {
            return EE_ERR_FLASH;
        }
        *blank = is_erased(buf, n);
    }
    return EE_OK;
}

/* Erase only when needed: every erase costs one endurance cycle. */
static ee_status_t page_erase(ee_t *ee, uint8_t page)
{
    bool blank = false;
    ee_status_t st = page_is_blank(ee, page, &blank);

    if ((st == EE_OK) && !blank) {
        const ee_flash_ops_t *f = &ee->cfg.flash;
        st = (f->erase_page(f->ctx, ee->cfg.page_addr[page]) == EE_OK) ? EE_OK : EE_ERR_FLASH;
        if (st == EE_OK) {
            ee->stats.page_erases++;
            st = page_is_blank(ee, page, &blank);
            if ((st == EE_OK) && !blank) {
                st = EE_ERR_FLASH;
            }
        }
    }
    return st;
}

/* ------------------------------------------------------------------ page header */

static bool page_is_valid(const ee_t *ee, uint8_t page, uint32_t *generation)
{
    uint8_t hdr[2u * EE_UNIT_SIZE];

    if (fl_read(ee, page, EE_OFF_RECEIVE, hdr, sizeof hdr) != EE_OK) {
        return false;
    }
    *generation = get_u32(&hdr[4]);
    return (get_u32(&hdr[0]) == EE_MAGIC_RECEIVE) && (get_u32(&hdr[8]) == EE_MAGIC_VALID) &&
           (get_u32(&hdr[12]) == ~*generation);
}

static ee_status_t page_mark(const ee_t *ee, uint8_t page, uint32_t off, uint32_t magic, uint32_t value)
{
    uint8_t unit[EE_UNIT_SIZE];

    put_u32(&unit[0], magic);
    put_u32(&unit[4], value);
    return fl_program(ee, page, off, unit, EE_UNIT_SIZE);
}

/* ------------------------------------------------------------------ records */

static uint16_t record_crc(uint16_t id, uint16_t len, const uint8_t *data)
{
    uint8_t key[4];

    put_u16(&key[0], id);
    put_u16(&key[2], len);
    return ee_crc16(ee_crc16(EE_CRC16_INIT, key, sizeof key), data, len);
}

static bool decode_header(const uint8_t unit[EE_UNIT_SIZE], uint16_t *id, uint16_t *len, uint16_t *data_crc)
{
    *id = get_u16(&unit[0]);
    *len = get_u16(&unit[2]);
    *data_crc = get_u16(&unit[4]);
    return (get_u16(&unit[6]) == ee_crc16(EE_CRC16_INIT, unit, 6u)) && (*id != EE_ID_INVALID) && (*len <= EE_MAX_DATA);
}

static void encode_commit(uint8_t unit[EE_UNIT_SIZE], const uint8_t header[EE_UNIT_SIZE])
{
    put_u32(&unit[0], EE_MAGIC_COMMIT);
    (void)memcpy(&unit[4], &header[4], 4u); /* data_crc and hdr_crc bind the commit to its record */
}

/* true when the commit unit of the record whose header is `header` is fully written */
static ee_status_t record_committed(const ee_t *ee, uint8_t page, uint32_t off, const uint8_t header[EE_UNIT_SIZE],
                                    uint16_t len, bool *committed)
{
    uint8_t expected[EE_UNIT_SIZE];
    uint8_t stored[EE_UNIT_SIZE];
    const ee_status_t st = fl_read(ee, page, commit_offset(off, len), stored, EE_UNIT_SIZE);

    encode_commit(expected, header);
    *committed = (st == EE_OK) && (memcmp(stored, expected, EE_UNIT_SIZE) == 0);
    return st;
}

/* CRC of the stored data, read in small chunks so no data buffer is needed. */
static ee_status_t stored_crc(const ee_t *ee, uint8_t page, uint32_t off, uint16_t id, uint16_t len, uint16_t *crc)
{
    uint8_t key[4];
    uint8_t buf[EE_CHUNK];

    put_u16(&key[0], id);
    put_u16(&key[2], len);
    *crc = ee_crc16(EE_CRC16_INIT, key, sizeof key);
    for (uint32_t done = 0u; done < len; done += EE_CHUNK) {
        const uint32_t n = ((len - done) < EE_CHUNK) ? (len - done) : EE_CHUNK;
        if (fl_read(ee, page, off + EE_UNIT_SIZE + done, buf, n) != EE_OK) {
            return EE_ERR_FLASH;
        }
        *crc = ee_crc16(*crc, buf, n);
    }
    return EE_OK;
}

/* Program header, data and - last - the commit unit; verify header, commit and data CRC. */
static ee_status_t record_program(ee_t *ee, uint8_t page, uint32_t off, uint16_t id, const uint8_t *data, uint16_t len)
{
    const uint32_t full = (uint32_t)len & ~(EE_UNIT_SIZE - 1u);
    const uint16_t crc = record_crc(id, len, data);
    uint8_t header[EE_UNIT_SIZE];
    uint8_t unit[EE_UNIT_SIZE];
    uint16_t rid = 0u;
    uint16_t rlen = 0u;
    uint16_t rcrc = 0u;
    uint16_t check = 0u;
    bool committed = false;
    ee_status_t st;

    put_u16(&header[0], id);
    put_u16(&header[2], len);
    put_u16(&header[4], crc);
    put_u16(&header[6], ee_crc16(EE_CRC16_INIT, header, 6u));
    st = fl_program(ee, page, off, header, EE_UNIT_SIZE);
    if ((st == EE_OK) && (full > 0u)) {
        st = fl_program(ee, page, off + EE_UNIT_SIZE, data, full);
    }
    if ((st == EE_OK) && (full < len)) {
        (void)memset(unit, EE_ERASED, sizeof unit);
        (void)memcpy(unit, &data[full], len - full);
        st = fl_program(ee, page, off + EE_UNIT_SIZE + full, unit, EE_UNIT_SIZE);
    }
    if (st == EE_OK) {
        encode_commit(unit, header);
        st = fl_program(ee, page, commit_offset(off, len), unit, EE_UNIT_SIZE);
    }
    if (st == EE_OK) {
        st = fl_read(ee, page, off, unit, EE_UNIT_SIZE);
    }
    if (st == EE_OK) {
        if (!decode_header(unit, &rid, &rlen, &rcrc) || (rid != id) || (rlen != len) || (rcrc != crc)) {
            st = EE_ERR_FLASH;
        }
    }
    if (st == EE_OK) {
        st = record_committed(ee, page, off, header, len, &committed);
        if ((st == EE_OK) && !committed) {
            st = EE_ERR_FLASH;
        }
    }
    if (st == EE_OK) {
        st = stored_crc(ee, page, off, id, len, &check);
        if ((st == EE_OK) && (check != crc)) {
            st = EE_ERR_FLASH;
        }
    }
    if (st == EE_OK) {
        ee->stats.records_written++;
    }
    return st;
}

/* Load the value at index position pos into ee->scratch and check its CRC. */
static ee_status_t value_load(ee_t *ee, uint32_t pos)
{
    const ee_index_entry_t *e = &ee->index[pos];
    uint8_t unit[EE_UNIT_SIZE];
    uint16_t id = 0u;
    uint16_t len = 0u;
    uint16_t crc = 0u;
    ee_status_t st = fl_read(ee, ee->active, e->offset, unit, EE_UNIT_SIZE);

    if (st == EE_OK) {
        if (!decode_header(unit, &id, &len, &crc) || (id != e->id) || (len != e->len)) {
            st = EE_ERR_CRC;
        }
    }
    if (st == EE_OK) {
        st = fl_read(ee, ee->active, e->offset + EE_UNIT_SIZE, ee->scratch, len);
    }
    if ((st == EE_OK) && (record_crc(id, len, ee->scratch) != crc)) {
        st = EE_ERR_CRC;
    }
    if (st == EE_ERR_CRC) {
        ee->stats.crc_errors++;
    }
    return st;
}

/* ------------------------------------------------------------------ RAM index */

static uint32_t index_find(const ee_t *ee, uint16_t id)
{
    for (uint32_t i = 0u; i < ee->key_count; i++) {
        if (ee->index[i].id == id) {
            return i;
        }
    }
    return EE_INDEX_NONE;
}

/* len 0 removes the id (deletion record). */
static ee_status_t index_apply(ee_t *ee, uint16_t id, uint16_t len, uint32_t offset)
{
    const uint32_t pos = index_find(ee, id);

    if (len == 0u) {
        if (pos != EE_INDEX_NONE) {
            ee->key_count--;
            ee->index[pos] = ee->index[ee->key_count];
        }
        return EE_OK;
    }
    if (pos != EE_INDEX_NONE) {
        ee->index[pos].len = len;
        ee->index[pos].offset = offset;
        return EE_OK;
    }
    if (ee->key_count >= EE_MAX_KEYS) {
        return EE_ERR_KEYS_FULL;
    }
    ee->index[ee->key_count].id = id;
    ee->index[ee->key_count].len = len;
    ee->index[ee->key_count].offset = offset;
    ee->key_count++;
    return EE_OK;
}

/* Rebuild the index from the active page. Uncommitted records (interrupted writes) are
 * skipped; a bad header ends the log and the next write swaps pages. */
static ee_status_t page_scan(ee_t *ee)
{
    uint32_t off = EE_OFF_FIRST_RECORD;
    ee_status_t st = EE_OK;

    ee->key_count = 0u;
    ee->needs_gc = false;
    while ((st == EE_OK) && ((off + EE_UNIT_SIZE) <= ee->cfg.page_size)) {
        uint8_t unit[EE_UNIT_SIZE];
        uint16_t id = 0u;
        uint16_t len = 0u;
        uint16_t crc = 0u;
        uint16_t check = 0u;
        bool committed = false;

        st = fl_read(ee, ee->active, off, unit, EE_UNIT_SIZE);
        if ((st != EE_OK) || is_erased(unit, EE_UNIT_SIZE)) {
            break;
        }
        if (!decode_header(unit, &id, &len, &crc) || ((off + record_size(len)) > ee->cfg.page_size)) {
            ee->needs_gc = true;
            off = ee->cfg.page_size;
            break;
        }
        st = record_committed(ee, ee->active, off, unit, len, &committed);
        if ((st == EE_OK) && committed) {
            st = stored_crc(ee, ee->active, off, id, len, &check);
            if ((st == EE_OK) && (check == crc)) {
                st = index_apply(ee, id, len, off);
            }
            else if (st == EE_OK) {
                ee->stats.crc_errors++; /* committed but changed since: keep the previous value */
            }
            else {
                /* flash read error: returned below */
            }
        }
        off += record_size(len);
    }
    ee->write_offset = (off > ee->cfg.page_size) ? ee->cfg.page_size : off;
    return st;
}

/* ------------------------------------------------------------------ garbage collection */

/* Copy the latest value of every id except new_id into the other page, add new_id's value
 * (len 0 = deleted), then commit the page with its VALID unit. The old page stays valid
 * (lower generation) until the next swap erases it. */
static ee_status_t page_swap(ee_t *ee, uint16_t new_id, const uint8_t *new_data, uint16_t new_len)
{
    const uint8_t dst = (uint8_t)(ee->active ^ 1u);
    const uint32_t generation = ee->generation + 1u;
    ee_index_entry_t next[EE_MAX_KEYS];
    uint32_t count = 0u;
    uint32_t off = EE_OFF_FIRST_RECORD;
    ee_status_t st = page_erase(ee, dst);

    if (st == EE_OK) {
        st = page_mark(ee, dst, EE_OFF_RECEIVE, EE_MAGIC_RECEIVE, generation);
    }
    for (uint32_t i = 0u; (st == EE_OK) && (i < ee->key_count); i++) {
        const ee_index_entry_t e = ee->index[i];
        if (e.id == new_id) {
            continue;
        }
        st = value_load(ee, i);
        if (st == EE_ERR_CRC) {
            st = EE_OK; /* corrupted value: dropped, counted in stats */
            continue;
        }
        if ((st == EE_OK) && ((off + record_size(e.len)) > ee->cfg.page_size)) {
            st = EE_ERR_FULL;
        }
        if (st == EE_OK) {
            st = record_program(ee, dst, off, e.id, ee->scratch, e.len);
        }
        if (st == EE_OK) {
            next[count].id = e.id;
            next[count].len = e.len;
            next[count].offset = off;
            count++;
            off += record_size(e.len);
        }
    }
    if ((st == EE_OK) && (new_len > 0u)) {
        if ((off + record_size(new_len)) > ee->cfg.page_size) {
            st = EE_ERR_FULL;
        }
        else {
            st = record_program(ee, dst, off, new_id, new_data, new_len);
        }
        if (st == EE_OK) {
            next[count].id = new_id;
            next[count].len = new_len;
            next[count].offset = off;
            count++;
            off += record_size(new_len);
        }
    }
    if (st == EE_OK) {
        st = page_mark(ee, dst, EE_OFF_VALID, EE_MAGIC_VALID, ~generation);
    }
    if (st == EE_OK) {
        ee->active = dst;
        ee->generation = generation;
        ee->write_offset = off;
        ee->needs_gc = false;
        ee->key_count = count;
        (void)memcpy(ee->index, next, count * sizeof next[0]);
        ee->stats.gc_runs++;
    }
    return st;
}

/* Append a record (len 0 = deletion); swap pages when the active page has no room. */
static ee_status_t record_store(ee_t *ee, uint16_t id, const uint8_t *data, uint16_t len)
{
    const uint32_t size = record_size(len);

    if (!ee->needs_gc && ((ee->write_offset + size) <= ee->cfg.page_size)) {
        const uint32_t off = ee->write_offset;
        if (record_program(ee, ee->active, off, id, data, len) == EE_OK) {
            ee->write_offset += size;
            return index_apply(ee, id, len, off);
        }
        /* The units are used now; continue on a fresh page. */
        ee->needs_gc = true;
        ee->write_offset = ee->cfg.page_size;
    }
    return page_swap(ee, id, data, len);
}

static ee_status_t format(ee_t *ee)
{
    ee_status_t st = page_erase(ee, 0u);

    if (st == EE_OK) {
        st = page_erase(ee, 1u);
    }
    if (st == EE_OK) {
        st = page_mark(ee, 0u, EE_OFF_RECEIVE, EE_MAGIC_RECEIVE, 1u);
    }
    if (st == EE_OK) {
        st = page_mark(ee, 0u, EE_OFF_VALID, EE_MAGIC_VALID, ~1u);
    }
    ee->active = 0u;
    ee->generation = 1u;
    ee->write_offset = EE_OFF_FIRST_RECORD;
    ee->needs_gc = false;
    ee->key_count = 0u;
    return st;
}

static bool config_valid(const ee_config_t *cfg)
{
    const uint32_t a = cfg->page_addr[0];
    const uint32_t b = cfg->page_addr[1];
    const uint32_t gap = (a > b) ? (a - b) : (b - a);

    return (cfg->flash.read != NULL) && (cfg->flash.program != NULL) && (cfg->flash.erase_page != NULL) &&
           ((cfg->page_size % EE_UNIT_SIZE) == 0u) &&
           (cfg->page_size >= (EE_OFF_FIRST_RECORD + record_size((uint16_t)EE_MAX_DATA))) && (gap >= cfg->page_size) &&
           ((a % EE_UNIT_SIZE) == 0u) && ((b % EE_UNIT_SIZE) == 0u);
}

/* ------------------------------------------------------------------ public API */

ee_status_t ee_init(ee_t *ee, const ee_config_t *cfg)
{
    uint32_t gen0 = 0u;
    uint32_t gen1 = 0u;
    bool valid0;
    bool valid1;
    ee_status_t st;

    if ((ee == NULL) || (cfg == NULL)) {
        return EE_ERR_PARAM;
    }
    (void)memset(ee, 0, sizeof *ee);
    if (!config_valid(cfg)) {
        return EE_ERR_PARAM;
    }
    ee->cfg = *cfg;
    valid0 = page_is_valid(ee, 0u, &gen0);
    valid1 = page_is_valid(ee, 1u, &gen1);
    if (!valid0 && !valid1) {
        st = format(ee);
    }
    else {
        ee->active = (valid1 && (!valid0 || (gen1 > gen0))) ? 1u : 0u;
        ee->generation = (ee->active == 1u) ? gen1 : gen0;
        st = page_scan(ee);
    }
    ee->initialised = (st == EE_OK);
    return st;
}

ee_status_t ee_format(ee_t *ee)
{
    if ((ee == NULL) || !ee->initialised) {
        return EE_ERR_NOT_INIT;
    }
    return format(ee);
}

ee_status_t ee_write(ee_t *ee, uint16_t id, const void *data, uint16_t len)
{
    uint32_t pos;

    if ((ee == NULL) || !ee->initialised) {
        return EE_ERR_NOT_INIT;
    }
    if ((id == EE_ID_INVALID) || (data == NULL) || (len == 0u) || (len > EE_MAX_DATA)) {
        return EE_ERR_PARAM;
    }
    pos = index_find(ee, id);
    if ((pos != EE_INDEX_NONE) && (ee->index[pos].len == len) && (value_load(ee, pos) == EE_OK) &&
        (memcmp(ee->scratch, data, len) == 0)) {
        ee->stats.writes_skipped++;
        return EE_OK;
    }
    if ((pos == EE_INDEX_NONE) && (ee->key_count >= EE_MAX_KEYS)) {
        return EE_ERR_KEYS_FULL;
    }
    return record_store(ee, id, (const uint8_t *)data, len);
}

ee_status_t ee_read(ee_t *ee, uint16_t id, void *buf, uint16_t buf_size, uint16_t *out_len)
{
    uint32_t pos;
    ee_status_t st;

    if ((ee == NULL) || !ee->initialised) {
        return EE_ERR_NOT_INIT;
    }
    if ((id == EE_ID_INVALID) || (buf == NULL)) {
        return EE_ERR_PARAM;
    }
    pos = index_find(ee, id);
    if (pos == EE_INDEX_NONE) {
        return EE_ERR_NOT_FOUND;
    }
    if (out_len != NULL) {
        *out_len = ee->index[pos].len;
    }
    if (buf_size < ee->index[pos].len) {
        return EE_ERR_BUFFER_SMALL;
    }
    st = value_load(ee, pos);
    if (st == EE_OK) {
        (void)memcpy(buf, ee->scratch, ee->index[pos].len);
    }
    return st;
}

ee_status_t ee_delete(ee_t *ee, uint16_t id)
{
    if ((ee == NULL) || !ee->initialised) {
        return EE_ERR_NOT_INIT;
    }
    if (id == EE_ID_INVALID) {
        return EE_ERR_PARAM;
    }
    if (index_find(ee, id) == EE_INDEX_NONE) {
        return EE_ERR_NOT_FOUND;
    }
    return record_store(ee, id, NULL, 0u);
}

uint32_t ee_free_space(const ee_t *ee)
{
    if ((ee == NULL) || !ee->initialised || ee->needs_gc) {
        return 0u;
    }
    return ee->cfg.page_size - ee->write_offset;
}
