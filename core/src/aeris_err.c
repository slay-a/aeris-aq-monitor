#include "aeris/aeris_err.h"

const char *aeris_strerr(aeris_err_t e)
{
    switch (e) {
    case AERIS_OK:            return "ok";
    case AERIS_ERR_IO:        return "i2c-io";
    case AERIS_ERR_CRC:       return "crc";
    case AERIS_ERR_ARG:       return "bad-arg";
    case AERIS_ERR_NOT_READY: return "not-ready";
    case AERIS_ERR_FRAME:     return "bad-frame";
    case AERIS_ERR_SELFTEST:  return "selftest";
    case AERIS_ERR_RANGE:     return "out-of-range";
    case AERIS_ERR_NOMEM:     return "no-mem";
    }
    return "unknown";
}
