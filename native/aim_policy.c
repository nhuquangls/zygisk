#include "aim_policy.h"
#include <math.h>
#include <string.h>

void cf_target_predictor_reset(CfTargetPredictor *predictor) {
    if (predictor) memset(predictor, 0, sizeof(*predictor));
}

bool cf_target_predict(CfTargetPredictor *predictor, uint64_t target_id,
                       const CfVec3 *head, uint64_t sample_ns, CfVec3 *out) {
    if (!out) return false;
    *out = (CfVec3){0};
    if (!predictor || !target_id || !head || !sample_ns ||
        !isfinite(head->x) || !isfinite(head->y) || !isfinite(head->z)) {
        cf_target_predictor_reset(predictor);
        return false;
    }
    if (predictor->target_id != target_id || !predictor->sample_ns ||
        sample_ns < predictor->sample_ns ||
        sample_ns - predictor->sample_ns > 100000000ULL) {
        cf_target_predictor_reset(predictor);
        predictor->target_id = target_id;
        predictor->sample_ns = sample_ns;
        predictor->last = *head;
        predictor->samples = 1;
        *out = *head;
        return true;
    }
    if (sample_ns > predictor->sample_ns) {
        float dt = (float)(sample_ns - predictor->sample_ns) * 1.0e-9f;
        if (!isfinite(dt) || dt < .002f || dt > .1f) {
            cf_target_predictor_reset(predictor);
            predictor->target_id = target_id;
            predictor->sample_ns = sample_ns;
            predictor->last = *head;
            predictor->samples = 1;
            *out = *head;
            return true;
        }
        CfVec3 raw = {(head->x - predictor->last.x) / dt,
                      (head->y - predictor->last.y) / dt,
                      (head->z - predictor->last.z) / dt};
        float speed = sqrtf(raw.x*raw.x + raw.z*raw.z);
        if (!isfinite(speed)) {
            cf_target_predictor_reset(predictor);
            return false;
        }
        const float max_speed = 12.0f;
        if (speed > max_speed) {
            float scale = max_speed / speed;
            raw.x *= scale; raw.z *= scale;
        }
        if (raw.y > 3.0f) raw.y = 3.0f;
        else if (raw.y < -3.0f) raw.y = -3.0f;
        if (predictor->samples == 1) predictor->velocity = raw;
        else {
            const float alpha = .30f, vertical_alpha = .12f;
            predictor->velocity.x += alpha * (raw.x - predictor->velocity.x);
            predictor->velocity.y += vertical_alpha * (raw.y - predictor->velocity.y);
            predictor->velocity.z += alpha * (raw.z - predictor->velocity.z);
        }
        predictor->last = *head;
        predictor->sample_ns = sample_ns;
        if (predictor->samples < UINT32_MAX) ++predictor->samples;
    }
    *out = (CfVec3){head->x + predictor->velocity.x * CF_TARGET_LEAD_SECONDS,
                    head->y + predictor->velocity.y * CF_TARGET_LEAD_SECONDS * .20f,
                    head->z + predictor->velocity.z * CF_TARGET_LEAD_SECONDS};
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
