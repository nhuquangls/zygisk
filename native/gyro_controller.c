#include "gyro_controller.h"
#include <math.h>
#include <string.h>

static float clamp_rate(float value) {
    if (value > CF_GYRO_RATE_CAP) return CF_GYRO_RATE_CAP;
    if (value < -CF_GYRO_RATE_CAP) return -CF_GYRO_RATE_CAP;
    return value;
}

static void clear_lock(CfGyroController *controller) {
    controller->locked_target = 0;
    controller->last_seen_ns = 0;
    controller->lock_started_ns = 0;
    controller->filtered_x = 0;
    controller->filtered_y = 0;
    controller->settled = false;
    cf_target_predictor_reset(&controller->predictor);
}

void cf_gyro_controller_reset(CfGyroController *controller) {
    if (!controller) return;
    memset(controller, 0, sizeof(*controller));
    controller->trigger_armed = true;
}

static const CfCandidate *find_target(const CfSceneSnapshot *snapshot,
                                      uint64_t target_id) {
    for (size_t i = 0; i < snapshot->count; ++i) {
        const CfCandidate *candidate = &snapshot->candidates[i];
        if (candidate->id != target_id) continue;
        return candidate->enemy && candidate->alive &&
               !candidate->invulnerable ? candidate : NULL;
    }
    return NULL;
}

bool cf_gyro_controller_step(CfGyroController *controller,
                             const CfSceneSnapshot *snapshot,
                             const CfViewport *viewport, uint64_t now,
                             CfGyroCommand *out) {
    if (!out) return false;
    memset(out, 0, sizeof(*out));
    if (!controller || !snapshot || !viewport || !snapshot->local_id ||
        snapshot->count > 256 || !snapshot->time_ns || now < snapshot->time_ns ||
        now - snapshot->time_ns > 100000000ULL ||
        !isfinite(viewport->width) || !isfinite(viewport->height) ||
        viewport->width <= 0 || viewport->height <= 0) {
        if (controller) cf_gyro_controller_reset(controller);
        return false;
    }

    if (!cf_scene_sniper_scope_active(snapshot)) {
        cf_gyro_controller_reset(controller);
        return false;
    }

    bool game_trigger = snapshot->doing_aim_assist && snapshot->game_aim_target;
    if (!game_trigger) {
        if (!controller->locked_target) {
            cf_gyro_controller_reset(controller);
            return false;
        }
        if (!controller->trigger_lost_ns) controller->trigger_lost_ns = now;
        controller->trigger_armed = true;
        if (now - controller->trigger_lost_ns >=
            CF_GYRO_TRIGGER_LOST_GRACE_NS) {
            cf_gyro_controller_reset(controller);
            return false;
        }
    } else {
        controller->trigger_lost_ns = 0;
        if (controller->trigger_armed ||
            snapshot->game_aim_target != controller->locked_target) {
            clear_lock(controller);
            controller->locked_target = snapshot->game_aim_target;
            controller->lock_started_ns = now;
            controller->trigger_armed = false;
        }
    }

    if (!controller->locked_target) return false;
    out->target_id = controller->locked_target;
    if (controller->settled) {
        out->mode = CF_GYRO_LATCHED;
        out->settled = true;
        return true;
    }
    if (controller->lock_started_ns &&
        now - controller->lock_started_ns > CF_GYRO_LOCK_TIMEOUT_NS) {
        controller->settled = true;
        controller->filtered_x = 0;
        controller->filtered_y = 0;
        out->mode = CF_GYRO_LATCHED;
        out->settled = true;
        return true;
    }
    out->mode = CF_GYRO_TRACKING;

    const CfCandidate *candidate = find_target(snapshot, controller->locked_target);
    if (!candidate) {
        controller->filtered_x = 0;
        controller->filtered_y = 0;
        if (!controller->last_seen_ns ||
            now - controller->last_seen_ns > CF_GYRO_TARGET_GRACE_NS) {
            controller->settled = true;
            out->mode = CF_GYRO_LATCHED;
            out->settled = true;
        }
        return false;
    }

    CfVec3 predicted;
    if (!cf_target_predict(&controller->predictor, candidate->id, &candidate->head,
                           snapshot->time_ns, &predicted)) {
        controller->filtered_x = 0;
        controller->filtered_y = 0;
        return false;
    }
    CfVec2 screen;
    if (!cf_world_to_screen(&snapshot->matrix, &predicted, viewport, &screen)) {
        controller->filtered_x = 0;
        controller->filtered_y = 0;
        return false;
    }
    controller->last_seen_ns = now;
    out->active = true;
    out->error_x = screen.x - viewport->width * .5f;
    out->error_y = screen.y - viewport->height * .5f;
    float distance = sqrtf(out->error_x*out->error_x + out->error_y*out->error_y);
    if (!isfinite(distance)) {
        controller->settled = true;
        controller->filtered_x = 0;
        controller->filtered_y = 0;
        out->mode = CF_GYRO_LATCHED;
        out->settled = true;
        return false;
    }

    if (distance <= CF_GYRO_DEADBAND_PX) {
        controller->settled = true;
        controller->filtered_x = 0;
        controller->filtered_y = 0;
        out->active = false;
        out->mode = CF_GYRO_LATCHED;
        out->settled = true;
        return true;
    }

    /*
     * For this 2944x1840 viewport, P00 = P11/aspect, so both screen axes
     * share the same pixels-to-angle denominator.  The ordered physical
     * calibration measured camera-right as -sensor-X and camera-up as
     * -sensor-Y.  Screen Y grows downward, hence the opposite signs below.
     */
    float denominator = viewport->height * .5f * snapshot->projection_y;
    if (!isfinite(denominator) || denominator <= 1.0f) {
        controller->settled = true;
        controller->filtered_x = 0;
        controller->filtered_y = 0;
        out->active = false;
        out->mode = CF_GYRO_LATCHED;
        out->settled = true;
        return false;
    }
    float desired_x = clamp_rate(-CF_GYRO_KP * out->error_x / denominator);
    float desired_y = clamp_rate( CF_GYRO_KP * out->error_y / denominator);
    const float alpha = .25f;
    controller->filtered_x += alpha * (desired_x - controller->filtered_x);
    controller->filtered_y += alpha * (desired_y - controller->filtered_y);
    out->sensor_x = controller->filtered_x;
    out->sensor_y = controller->filtered_y;
    return true;
}
