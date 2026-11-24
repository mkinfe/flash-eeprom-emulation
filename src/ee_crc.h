#ifndef EE_CRC_H
#define EE_CRC_H

#include <stdint.h>

#define EE_CRC16_INIT 0xFFFFu

/** CRC-16/CCITT-FALSE (poly 0x1021, no reflection); pass the previous result to continue. */
uint16_t ee_crc16(uint16_t crc, const uint8_t *data, uint32_t len);

#endif /* EE_CRC_H */
