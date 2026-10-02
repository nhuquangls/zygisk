#ifndef CF_AIM_POLICY_H
#define CF_AIM_POLICY_H
#include "scene_snapshot.h"
#define CF_TARGET_LEAD_SECONDS 0.060f
#define CF_TARGET_LEAD_MIN_SPEED 0.25f
#define CF_TARGET_LEAD_FULL_SPEED 0.80f
#define CF_TARGET_LEAD_MAX_SPEED 12.0f
#define CF_TARGET_LEAD_DIRECTION_COS_MIN 0.50f
#define CF_TARGET_LEAD_FOLLOW_PIXELS 1.0f
typedef struct CfTargetPredictor {
    uint64_t target_id;
    uint64_t sample_ns;
    CfVec3 last;
    CfVec3 velocity;
    uint32_t samples;
} CfTargetPredictor;
void cf_target_predictor_reset(CfTargetPredictor *);
bool cf_target_predict(CfTargetPredictor *, uint64_t, const CfVec3 *,
                       const CfVec3 *, uint64_t, CfVec3 *);
float cf_target_lead_speed_weight(float);
bool cf_aim_from_snapshot(const CfSceneSnapshot *, const CfViewport *, uint64_t locked,
                          float lock_projection, uint64_t now, CfAimResult *);
#endif
