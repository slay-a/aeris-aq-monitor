#include "aeris/crc8.h"
#include "test.h"

void suite_crc8(void)
{
    SUITE("crc8");

    /* The one vector Sensirion publishes: 0xBEEF -> 0x92. If this passes, the
     * polynomial, init value and bit order are all right. */
    const uint8_t beef[2] = { 0xBE, 0xEF };
    CHECK_EQ_I(aeris_crc8(beef, 2), 0x92);

    /* Degenerate inputs. */
    const uint8_t zeros[2] = { 0x00, 0x00 };
    CHECK_EQ_I(aeris_crc8(zeros, 2), 0x81);
    const uint8_t ones[2] = { 0xFF, 0xFF };
    CHECK_EQ_I(aeris_crc8(ones, 2), 0xAC);

    /* put/take round trip. */
    uint8_t buf[9];
    size_t n = 0;
    n += aeris_put_word_crc(&buf[n], 0xBEEF);
    n += aeris_put_word_crc(&buf[n], 0x0000);
    n += aeris_put_word_crc(&buf[n], 0x1234);
    CHECK_EQ_I(n, 9);
    CHECK_EQ_I(buf[2], 0x92);

    uint16_t w[3];
    CHECK_EQ_I(aeris_take_words_crc(buf, w, 3), AERIS_OK);
    CHECK_EQ_I(w[0], 0xBEEF);
    CHECK_EQ_I(w[1], 0x0000);
    CHECK_EQ_I(w[2], 0x1234);

    /* A single flipped bit anywhere in a word must be caught -- this is the
     * whole reason the check exists. */
    int missed = 0;
    for (int word = 0; word < 3; word++) {
        for (int byte = 0; byte < 2; byte++) {
            for (int bit = 0; bit < 8; bit++) {
                uint8_t t[9];
                memcpy(t, buf, sizeof t);
                t[word * 3 + byte] ^= (uint8_t)(1u << bit);
                uint16_t out[3];
                if (aeris_take_words_crc(t, out, 3) != AERIS_ERR_CRC) missed++;
            }
        }
    }
    CHECK_MSG(missed == 0, "CRC-8 missed %d of 48 single-bit flips", missed);

    /* A corrupted CRC byte itself must also fail. */
    uint8_t bad[9];
    memcpy(bad, buf, sizeof bad);
    bad[2] ^= 0x01;
    uint16_t out2[3];
    CHECK_EQ_I(aeris_take_words_crc(bad, out2, 3), AERIS_ERR_CRC);
}
