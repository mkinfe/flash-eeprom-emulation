#include "ee_crc.h"

uint16_t ee_crc16(uint16_t crc, const uint8_t *data, uint32_t len)
{
    uint16_t value = crc;

    for (uint32_t i = 0u; i < len; i++) {
        value ^= (uint16_t)((uint16_t)data[i] << 8u);
        for (uint32_t bit = 0u; bit < 8u; bit++) {
            value = ((value & 0x8000u) != 0u) ? (uint16_t)((uint16_t)(value << 1u) ^ 0x1021u) : (uint16_t)(value << 1u);
        }
    }
    return value;
}
