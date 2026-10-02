#ifndef CF_GYRO_RAMP_H
#define CF_GYRO_RAMP_H

#include <stdbool.h>
#include <stdint.h>

/*
 * Commands are refreshed by the 8 ms controller loop.  Expire independently
 * in the sensor consumer so a stalled worker can never leave a correction on.
 */
#define CF_GYRO_COMMAND_TIMEOUT_NS 100000000ULL
#define CF_GYRO_NOMINAL_SAMPLE_NS    2500000ULL
#define CF_GYRO_RAMP_RESET_GAP_NS   50000000ULL
#define CF_GYRO_SLEW_PER_SECOND          20.0f
#define CF_GYRO_RAMP_LIMIT                 .25f

typedef struct CfGyroRamp {
    float applied_x;
    float applied_y;
    uint64_t last_event_ns;
} CfGyroRamp;

void cf_gyro_ramp_reset(CfGyroRamp *);
bool cf_gyro_command_fresh(bool, uint64_t, uint64_t);
bool cf_gyro_ramp_step(CfGyroRamp *, bool, const float[2], uint64_t,
                       float[2]);

#endif
