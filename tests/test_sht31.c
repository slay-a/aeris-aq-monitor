#include <string.h>
#include "aeris/crc8.h"
#include "aeris/sht31.h"
#include "room_model.h"
#include "test.h"
#include "virtual_i2c.h"

void suite_sht31(void)
{
    SUITE("sht31");

    /* Same transfer function as the SCD40's RH/T words. */
    CHECK_NEAR(sht31_decode_temperature(0x6666), 25.0f, 0.02f);
    CHECK_NEAR(sht31_decode_temperature(0x0000), -45.0f, 0.001f);
    CHECK_NEAR(sht31_decode_temperature(0xFFFF), 130.0f, 0.001f);
    CHECK_NEAR(sht31_decode_humidity(0x8000),    50.0f, 0.02f);
    CHECK_NEAR(sht31_decode_humidity(0xFFFF),   100.0f, 0.001f);

    room_t room; room_init(&room, 9);
    vbus_t vb;   vbus_init(&vb, &room);
    sht31_t d;   sht31_init(&d, vbus_hal(&vb), 0);
    CHECK_EQ_I(d.addr, 0x44);

    CHECK_EQ_I(sht31_soft_reset(&d), AERIS_OK);

    uint16_t st = 0;
    CHECK_EQ_I(sht31_read_status(&d, &st), AERIS_OK);

    sht31_sample_t s;
    CHECK_EQ_I(sht31_measure(&d, &s), AERIS_OK);
    CHECK_NEAR(s.temperature_c, room.temp_c, 0.4f);
    CHECK_NEAR(s.humidity_rh,   room.rh,     2.0f);

    CHECK_EQ_I(sht31_heater(&d, true),  AERIS_OK);
    CHECK_EQ_I(sht31_heater(&d, false), AERIS_OK);

    /* The 15 ms measurement delay is the driver's responsibility, because the
     * part does not clock-stretch in the mode we use. The virtual sensor NACKs
     * a read issued early, so removing the delay would fail here. */
    vb.sht31.cmd_ready_at_ms = vb.now_ms + 50;
    CHECK_EQ_I(aeris_i2c_read(vbus_hal(&vb), SHT31_I2C_ADDR_A, (uint8_t[6]){0}, 6),
               AERIS_ERR_IO);

    vbus_set_present(&vb, SHT31_I2C_ADDR_A, false);
    CHECK_EQ_I(sht31_measure(&d, &s), AERIS_ERR_IO);
}
