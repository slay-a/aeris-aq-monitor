#include "aeris/crc8.h"

#define CRC8_POLY 0x31u
#define CRC8_INIT 0xFFu

uint8_t aeris_crc8(const uint8_t *data, size_t len)
{
    uint8_t crc = CRC8_INIT;
    for (size_t i = 0; i < len; i++) {
        crc ^= data[i];
        for (int bit = 0; bit < 8; bit++) {
            crc = (crc & 0x80u) ? (uint8_t)((crc << 1) ^ CRC8_POLY)
                                : (uint8_t)(crc << 1);
        }
    }
    return crc;
}

size_t aeris_put_word_crc(uint8_t *buf, uint16_t word)
{
    buf[0] = (uint8_t)(word >> 8);
    buf[1] = (uint8_t)(word & 0xFFu);
    buf[2] = aeris_crc8(buf, 2);
    return 3;
}

aeris_err_t aeris_take_words_crc(const uint8_t *raw, uint16_t *out, size_t n_words)
{
    for (size_t i = 0; i < n_words; i++) {
        const uint8_t *t = &raw[i * 3];
        if (aeris_crc8(t, 2) != t[2]) {
            return AERIS_ERR_CRC;
        }
        out[i] = (uint16_t)((uint16_t)t[0] << 8 | t[1]);
    }
    return AERIS_OK;
}
