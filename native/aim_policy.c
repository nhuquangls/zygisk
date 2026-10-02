#include "aim_policy.h"
#include <math.h>
#include <string.h>

void cf_target_predictor_reset(CfTargetPredictor *predictor) {
    if (predictor) memset(predictor, 0, sizeof(*predictor));
}

static bool finite_vec3(const CfVec3 *value) {
    return value && isfinite(value->x) && isfinite(value->y) &&
           isfinite(value->z);
}

float cf_target_lead_speed_weight(float speed) {
    if (!isfinite(speed) || speed <= CF_TARGET_LEAD_MIN_SPEED) return 0.0f;
    if (speed >= CF_TARGET_LEAD_FULL_SPEED) return 1.0f;
    float position = (speed - CF_TARGET_LEAD_MIN_SPEED) /
                     (CF_TARGET_LEAD_FULL_SPEED -
                      CF_TARGET_LEAD_MIN_SPEED);
    return position * position * (3.0f - 2.0f * position);
}

bool cf_target_predict(CfTargetPredictor *predictor, uint64_t target_id,
                       const CfVec3 *motion, const CfVec3 *aim,
                       uint64_t sample_ns, CfVec3 *out) {
    if (!out) return false;
    *out = (CfVec3){0};
    if (!predictor || !target_id || !finite_vec3(motion) ||
        !finite_vec3(aim) || !sample_ns) {
        cf_target_predictor_reset(predictor);
        return false;
    }
    if (predictor->target_id != target_id || !predictor->sample_ns ||
        sample_ns < predictor->sample_ns ||
        sample_ns - predictor->sample_ns > 100000000ULL) {
        cf_target_predictor_reset(predictor);
        predictor->target_id = target_id;
        predictor->sample_ns = sample_ns;
        predictor->last = *motion;
        predictor->samples = 1;
        *out = *aim;
        return true;
    }
    if (sample_ns > predictor->sample_ns) {
        float dt = (float)(sample_ns - predictor->sample_ns) * 1.0e-9f;
        if (!isfinite(dt) || dt < .002f || dt > .1f) {
            cf_target_predictor_reset(predictor);
            predictor->target_id = target_id;
            predictor->sample_ns = sample_ns;
            predictor->last = *motion;
            predictor->samples = 1;
            *out = *aim;
            return true;
        }
        CfVec3 raw = {(motion->x - predictor->last.x) / dt, 0.0f,
                      (motion->z - predictor->last.z) / dt};
        float speed = sqrtf(raw.x*raw.x + raw.z*raw.z);
        if (!isfinite(speed)) {
            cf_target_predictor_reset(predictor);
            return false;
        }
        if (speed > CF_TARGET_LEAD_MAX_SPEED) {
            float scale = CF_TARGET_LEAD_MAX_SPEED / speed;
            raw.x *= scale; raw.z *= scale;
            speed = CF_TARGET_LEAD_MAX_SPEED;
        }

        float previous_speed = sqrtf(predictor->velocity.x*predictor->velocity.x +
                                     predictor->velocity.z*predictor->velocity.z);
        bool same_direction = false;
        if (speed > 0.0f && isfinite(previous_speed) && previous_speed > 0.0f) {
            float cosine = (raw.x*predictor->velocity.x +
                            raw.z*predictor->velocity.z) /
                           (speed * previous_speed);
            same_direction = isfinite(cosine) &&
                             cosine >= CF_TARGET_LEAD_DIRECTION_COS_MIN;
        }
        const float alpha = .30f;
        if (predictor->samples == 1 || !same_direction) {
            /* A new direction starts small instead of producing a one-tick jump. */
            predictor->velocity.x = raw.x * alpha;
            predictor->velocity.z = raw.z * alpha;
        } else {
            predictor->velocity.x += alpha *
                (raw.x - predictor->velocity.x);
            predictor->velocity.z += alpha *
                (raw.z - predictor->velocity.z);
        }
        predictor->velocity.y = 0.0f;
        predictor->last = *motion;
        predictor->sample_ns = sample_ns;
        if (predictor->samples < UINT32_MAX) ++predictor->samples;
    }

    float filtered_speed = sqrtf(predictor->velocity.x*predictor->velocity.x +
                                 predictor->velocity.z*predictor->velocity.z);
    float weight = cf_target_lead_speed_weight(filtered_speed);
    *out = (CfVec3){aim->x + predictor->velocity.x *
                             CF_TARGET_LEAD_SECONDS * weight,
                    aim->y,
                    aim->z + predictor->velocity.z *
                             CF_TARGET_LEAD_SECONDS * weight};
    if (!isfinite(out->x) || !isfinite(out->y) || !isfinite(out->z)) {
        cf_target_predictor_reset(predictor);
        *out = (CfVec3){0};
        return false;
    }
    return true;
}

bool cf_aim_from_snapshot(const CfSceneSnapshot *snapshot, const CfViewport *viewport,
                          uint64_t locked, float lock_projection, uint64_t now, CfAimResult *out) {
    (void)lock_projection;
    if (!out) return false;
    memset(out, 0, sizeof(*out));
    uint64_t target_id = snapshot ? (locked ? locked : snapshot->game_aim_target) : 0;
    if (!snapshot || !cf_scene_sniper_scope_active(snapshot) ||
        !snapshot->local_id || snapshot->count > 256 ||
        !target_id || !snapshot->doing_aim_assist ||
        !snapshot->game_aim_target ||
        (locked && locked != snapshot->game_aim_target) ||
        !snapshot->time_ns ||
        now < snapshot->time_ns || now - snapshot->time_ns > 100000000ULL)
        return false;
    for (size_t i = 0; i < snapshot->count; ++i) {
        const CfCandidate *candidate = &snapshot->candidates[i];
        if (candidate->id != target_id) continue;
        if (!candidate->enemy || !candidate->alive ||
            candidate->invulnerable) return false;
        CfVec2 screen;
        if (!cf_world_to_screen(&snapshot->matrix, &candidate->head, viewport, &screen))
            return false;
        out->target_id = candidate->id;
        out->screen = screen;
        out->delta = (CfVec2){screen.x - viewport->width * .5f,
                              screen.y - viewport->height * .5f};
        return true;
    }
    return false;
}
