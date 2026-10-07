/* crc8.h - Sensirion CRC-8 (poly 0x31, init 0xFF, no reflection, no xor-out).
 *
 * Shared by the SCD40, SGP41 and SHT31. Every 16-bit word on the wire carries
 * one of these; a sensor read that skips the check is a sensor read you cannot
 * trust, because a single flipped bit on a long I2C run decodes as a perfectly
 * plausible CO2 reading.
 */
#ifndef AERIS_CRC8_H
#define AERIS_CRC8_H

#include <stddef.h>
#include <stdint.h>
#include "aeris/aeris_err.h"

uint8_t aeris_crc8(const uint8_t *data, size_t len);

/* Append a big-endian word plus its CRC to buf; returns bytes written (3). */
size_t aeris_put_word_crc(uint8_t *buf, uint16_t word);

/* Decode n_words of [msb, lsb, crc] triplets from raw into out.
 * Returns AERIS_ERR_CRC on the first bad triplet. */
aeris_err_t aeris_take_words_crc(const uint8_t *raw, uint16_t *out, size_t n_words);

#endif /* AERIS_CRC8_H */
