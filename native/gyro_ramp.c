#include "gyro_ramp.h"
#include <math.h>
#include <string.h>

static float move_towards(float current, float target, float maximum_delta) {
    float delta = target - current;
    if (fabsf(delta) <= maximum_delta) return target;
    return current + (delta < 0.0f ? -maximum_delta : maximum_delta);
}

void cf_gyro_ramp_reset(CfGyroRamp *ramp) {
    if (ramp) memset(ramp, 0, sizeof(*ramp));
}

bool cf_gyro_command_fresh(bool active, uint64_t published_ns,
                           uint64_t now_ns) {
    return active && published_ns && now_ns >= published_ns &&
           now_ns - published_ns <= CF_GYRO_COMMAND_TIMEOUT_NS;
}

bool cf_gyro_ramp_step(CfGyroRamp *ramp, bool active,
                       const float target[2], uint64_t event_ns,
                       float output[2]) {
    if (output) output[0] = output[1] = 0.0f;
    if (!ramp || !output) return false;
    if (!active || !target || !event_ns ||
        !isfinite(target[0]) || !isfinite(target[1]) ||
        fabsf(target[0]) > CF_GYRO_RAMP_LIMIT ||
        fabsf(target[1]) > CF_GYRO_RAMP_LIMIT) {
        cf_gyro_ramp_reset(ramp);
        return false;
    }

    uint64_t elapsed_ns = CF_GYRO_NOMINAL_SAMPLE_NS;
    if (ramp->last_event_ns) {
        if (event_ns < ramp->last_event_ns ||
            event_ns - ramp->last_event_ns > CF_GYRO_RAMP_RESET_GAP_NS) {
            ramp->applied_x = 0.0f;
            ramp->applied_y = 0.0f;
        } else {
            elapsed_ns = event_ns - ramp->last_event_ns;
        }
    }
    ramp->last_event_ns = event_ns;
    if (!isfinite(ramp->applied_x) || !isfinite(ramp->applied_y)) {
        ramp->applied_x = 0.0f;
        ramp->applied_y = 0.0f;
    }

    float maximum_delta = CF_GYRO_SLEW_PER_SECOND *
                          ((float)elapsed_ns / 1000000000.0f);
    ramp->applied_x = move_towards(ramp->applied_x, target[0], maximum_delta);
    ramp->applied_y = move_towards(ramp->applied_y, target[1], maximum_delta);
    output[0] = ramp->applied_x;
    output[1] = ramp->applied_y;
    return true;
}
