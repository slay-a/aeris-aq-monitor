/* aeris_err.h - error codes shared by every core module. */
#ifndef AERIS_ERR_H
#define AERIS_ERR_H

typedef enum {
    AERIS_OK            =  0,
    AERIS_ERR_IO        = -1,  /* bus-level NACK / timeout                  */
    AERIS_ERR_CRC       = -2,  /* CRC-8 mismatch on a received word         */
    AERIS_ERR_ARG       = -3,  /* caller passed something impossible        */
    AERIS_ERR_NOT_READY = -4,  /* sensor has no fresh sample yet            */
    AERIS_ERR_FRAME     = -5,  /* malformed frame (PMSA003I header/length)  */
    AERIS_ERR_SELFTEST  = -6,  /* on-chip self-test reported a failure      */
    AERIS_ERR_RANGE     = -7,  /* value decoded but outside plausible range */
    AERIS_ERR_NOMEM     = -8,
} aeris_err_t;

const char *aeris_strerr(aeris_err_t e);

#endif /* AERIS_ERR_H */
