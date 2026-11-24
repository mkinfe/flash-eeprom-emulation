/**
 * @file    ee_port_stm32l4.c
 * @brief   Example flash driver for STM32L4 (e.g. NUCLEO-L476RG) using the ST HAL.
 *
 * Not part of the host build. Uses the last two 2 KB pages of a 1 MB device; reserve them in
 * the linker script so the application never places code there. STM32L4 programs 64-bit
 * double words once per erase, which matches EE_UNIT_SIZE = 8.
 *
 * Note: reading a double word whose programming was interrupted by a power cut can raise an
 * ECC double-error (ECCD) NMI on STM32L4. A production port handles it in NMI_Handler: clear
 * FLASH_ECCR_ECCD and make the read return EE_ERR_FLASH, so the library treats it as damage.
 */
#include <string.h>

#include "ee/eeprom_emul.h"
#include "stm32l4xx_hal.h"

#define EE_PAGE0_ADDR 0x080FF000u /* bank 2, page 254 */
#define EE_PAGE1_ADDR 0x080FF800u /* bank 2, page 255 */

static ee_status_t l4_read(void *ctx, uint32_t addr, void *dst, uint32_t len)
{
    (void)ctx;
    (void)memcpy(dst, (const void *)(uintptr_t)addr, len);
    return EE_OK;
}

static ee_status_t l4_program(void *ctx, uint32_t addr, const void *src, uint32_t len)
{
    const uint8_t *p = src;
    ee_status_t st = EE_OK;

    (void)ctx;
    (void)HAL_FLASH_Unlock();
    __HAL_FLASH_CLEAR_FLAG(FLASH_FLAG_ALL_ERRORS);
    for (uint32_t i = 0u; (i < len) && (st == EE_OK); i += EE_UNIT_SIZE) {
        uint64_t dword;
        (void)memcpy(&dword, &p[i], sizeof dword);
        if (HAL_FLASH_Program(FLASH_TYPEPROGRAM_DOUBLEWORD, addr + i, dword) != HAL_OK) {
            st = EE_ERR_FLASH;
        }
    }
    (void)HAL_FLASH_Lock();
    return st;
}

static ee_status_t l4_erase_page(void *ctx, uint32_t addr)
{
    const uint32_t offset = addr - FLASH_BASE;
    FLASH_EraseInitTypeDef erase = { 0 };
    uint32_t page_error = 0u;
    ee_status_t st;

    (void)ctx;
    erase.TypeErase = FLASH_TYPEERASE_PAGES;
    erase.Banks = (offset < FLASH_BANK_SIZE) ? FLASH_BANK_1 : FLASH_BANK_2;
    erase.Page = (offset % FLASH_BANK_SIZE) / FLASH_PAGE_SIZE;
    erase.NbPages = 1u;
    (void)HAL_FLASH_Unlock();
    __HAL_FLASH_CLEAR_FLAG(FLASH_FLAG_ALL_ERRORS);
    st = (HAL_FLASHEx_Erase(&erase, &page_error) == HAL_OK) ? EE_OK : EE_ERR_FLASH;
    (void)HAL_FLASH_Lock();
    return st;
}

const ee_config_t ee_stm32l4_config = {
    .flash = { l4_read, l4_program, l4_erase_page, NULL },
    .page_addr = { EE_PAGE0_ADDR, EE_PAGE1_ADDR },
    .page_size = 2048u,
};

/* Usage:
 *     static ee_t ee;
 *     if (ee_init(&ee, &ee_stm32l4_config) == EE_OK) {
 *         uint32_t boot_count = 0u;
 *         (void)ee_read(&ee, 1u, &boot_count, sizeof boot_count, NULL);
 *         boot_count++;
 *         (void)ee_write(&ee, 1u, &boot_count, sizeof boot_count);
 *     }
 */
