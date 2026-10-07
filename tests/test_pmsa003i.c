#include <string.h>
#include "aeris/pmsa003i.h"
#include "room_model.h"
#include "test.h"
#include "virtual_i2c.h"

/* Build a well-formed frame with the given PM2.5, so the parser can be tested
 * without the sensor model in the way. */
static void make_frame(uint8_t *f, uint16_t pm2_5)
{
    memset(f, 0, PMSA003I_FRAME_LEN);
    f[0] = PMSA003I_MAGIC_0;
    f[1] = PMSA003I_MAGIC_1;
    f[2] = 0x00; f[3] = PMSA003I_FRAME_BODY;
    f[12] = (uint8_t)(pm2_5 >> 8);      /* pm2_5_env */
    f[13] = (uint8_t)(pm2_5 & 0xFF);
    uint16_t sum = 0;
    for (int i = 0; i < PMSA003I_FRAME_LEN - 2; i++) sum = (uint16_t)(sum + f[i]);
    f[30] = (uint8_t)(sum >> 8);
    f[31] = (uint8_t)(sum & 0xFF);
}

void suite_pmsa003i(void)
{
    SUITE("pmsa003i");

    uint8_t f[PMSA003I_FRAME_LEN];
    pmsa003i_sample_t s;

    make_frame(f, 23);
    CHECK_EQ_I(pmsa003i_parse(f, &s), AERIS_OK);
    CHECK_EQ_I(s.pm2_5_env, 23);

    /* Wrong magic: a short read that leaves the buffer misaligned must not be
     * accepted as data. */
    make_frame(f, 23); f[0] = 0x00;
    CHECK_EQ_I(pmsa003i_parse(f, &s), AERIS_ERR_FRAME);
    make_frame(f, 23); f[1] = 0x4C;
    CHECK_EQ_I(pmsa003i_parse(f, &s), AERIS_ERR_FRAME);

    /* Wrong length field. */
    make_frame(f, 23); f[3] = 0x20;
    CHECK_EQ_I(pmsa003i_parse(f, &s), AERIS_ERR_FRAME);

    /* Every single-bit flip in the body must be caught by the checksum. */
    int missed = 0;
    for (int byte = 4; byte < 30; byte++) {
        for (int bit = 0; bit < 8; bit++) {
            make_frame(f, 23);
            f[byte] ^= (uint8_t)(1u << bit);
            if (pmsa003i_parse(f, &s) == AERIS_OK) missed++;
        }
    }
    CHECK_MSG(missed == 0, "checksum missed %d of 208 single-bit flips", missed);

    /* Extreme but valid values decode rather than being rejected. */
    make_frame(f, 1000);
    CHECK_EQ_I(pmsa003i_parse(f, &s), AERIS_OK);
    CHECK_EQ_I(s.pm2_5_env, 1000);

    /* ---- against the sensor model ------------------------------------ */
    room_t room; room_init(&room, 5);
    vbus_t vb;   vbus_init(&vb, &room);
    pmsa003i_t d; pmsa003i_init(&d, vbus_hal(&vb), 0);
    CHECK_EQ_I(d.addr, 0x12);

    CHECK_EQ_I(pmsa003i_read(&d, &s), AERIS_OK);
    CHECK_NEAR((float)s.pm2_5_env, room.pm2_5_ugm3, 3.0f);

    /* A heavy cooking event drives the particle counts past 65535. The model
     * must still emit a valid frame -- this is the regression for the
     * out-of-range float conversion found during bring-up. */
    room.pm2_5_ugm3 = 900.0f;
    for (int i = 0; i < 50; i++) {
        CHECK_EQ_I(pmsa003i_read(&d, &s), AERIS_OK);
    }
    CHECK_EQ_I(d.frame_errors, 0);
    CHECK(s.pm2_5_env > 800);

    vbus_set_present(&vb, PMSA003I_I2C_ADDR, false);
    CHECK_EQ_I(pmsa003i_read(&d, &s), AERIS_ERR_IO);
}
