#include "gyro_controller.h"
#include <math.h>
#include <string.h>

static float clamp_rate(float value, float cap) {
    if (value > cap) return cap;
    if (value < -cap) return -cap;
    return value;
}

static float rate_cap_for_distance(float distance) {
    if (distance <= CF_GYRO_RATE_RAMP_START_PX)
        return CF_GYRO_RATE_CAP_NEAR;
    if (distance >= CF_GYRO_RATE_RAMP_END_PX)
        return CF_GYRO_RATE_CAP_FAR;
    float position = (distance - CF_GYRO_RATE_RAMP_START_PX) /
                     (CF_GYRO_RATE_RAMP_END_PX -
                      CF_GYRO_RATE_RAMP_START_PX);
    /* Ease the cap at both ends so deceleration toward the target has no
     * linear-curve corner at either distance threshold. */
    position = position * position * (3.0f - 2.0f * position);
    return CF_GYRO_RATE_CAP_NEAR +
           position * (CF_GYRO_RATE_CAP_FAR - CF_GYRO_RATE_CAP_NEAR);
}

static void clear_lock(CfGyroController *controller) {
    controller->locked_target = 0;
    controller->last_seen_ns = 0;
    controller->lock_started_ns = 0;
    controller->settled = false;
    cf_target_predictor_reset(&controller->predictor);
}

static void start_lock(CfGyroController *controller, uint64_t target,
                       uint64_t now, bool preserve_prediction) {
    CfTargetPredictor predictor = controller->predictor;
    clear_lock(controller);
    if (preserve_prediction) controller->predictor = predictor;
    controller->locked_target = target;
    controller->lock_started_ns = now;
    controller->trigger_armed = false;
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
        bool resume_prediction = controller->trigger_armed &&
            controller->locked_target == snapshot->game_aim_target &&
            controller->trigger_lost_ns && now >= controller->trigger_lost_ns &&
            now - controller->trigger_lost_ns <
                CF_GYRO_TRIGGER_LOST_GRACE_NS;
        controller->trigger_lost_ns = 0;
        if (controller->trigger_armed ||
            snapshot->game_aim_target != controller->locked_target) {
            start_lock(controller, snapshot->game_aim_target, now,
                       resume_prediction);
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
        out->mode = CF_GYRO_LATCHED;
        out->settled = true;
        return true;
    }
    out->mode = CF_GYRO_TRACKING;

    const CfCandidate *candidate = find_target(snapshot, controller->locked_target);
    if (!candidate) {
        if (!controller->last_seen_ns ||
            now - controller->last_seen_ns > CF_GYRO_TARGET_GRACE_NS) {
            controller->settled = true;
            out->mode = CF_GYRO_LATCHED;
            out->settled = true;
        }
        return false;
    }

    const CfVec3 *motion = snapshot->motion_target_id == candidate->id
        ? &snapshot->motion_anchor : &candidate->head;
    CfVec3 predicted;
    if (!cf_target_predict(&controller->predictor, candidate->id, motion,
                           &candidate->head, snapshot->time_ns, &predicted)) {
        return false;
    }
    CfVec2 current_screen;
    if (!cf_world_to_screen(&snapshot->matrix, &candidate->head, viewport,
                            &current_screen)) {
        return false;
    }
    CfVec2 screen = current_screen, predicted_screen;
    float applied_lead_pixels = 0.0f;
    if (cf_world_to_screen(&snapshot->matrix, &predicted, viewport,
                           &predicted_screen)) {
        float lead_x = predicted_screen.x - current_screen.x;
        float lead_y = predicted_screen.y - current_screen.y;
        float lead_pixels = sqrtf(lead_x*lead_x + lead_y*lead_y);
        screen = predicted_screen;
        applied_lead_pixels = lead_pixels;
    }
    controller->last_seen_ns = now;
    out->active = true;
    out->error_x = screen.x - viewport->width * .5f;
    out->error_y = screen.y - viewport->height * .5f;
    float distance = sqrtf(out->error_x*out->error_x + out->error_y*out->error_y);
    if (!isfinite(distance)) {
        controller->settled = true;
        out->mode = CF_GYRO_LATCHED;
        out->settled = true;
        return false;
    }

    if (distance <= CF_GYRO_DEADBAND_PX &&
        applied_lead_pixels < CF_TARGET_LEAD_FOLLOW_PIXELS) {
        controller->settled = true;
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
        out->active = false;
        out->mode = CF_GYRO_LATCHED;
        out->settled = true;
        return false;
    }
    float cap = rate_cap_for_distance(distance);
    float desired_x = clamp_rate(-CF_GYRO_KP * out->error_x / denominator, cap);
    float desired_y = clamp_rate( CF_GYRO_KP * out->error_y / denominator, cap);
    /* Sensor-cadence slew limiting is applied after the real gyro read. */
    out->sensor_x = desired_x;
    out->sensor_y = desired_y;
    return true;
}
