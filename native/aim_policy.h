#ifndef CF_AIM_POLICY_H
#define CF_AIM_POLICY_H
#include "scene_snapshot.h"
#define CF_TARGET_LEAD_SECONDS 0.090f
typedef struct CfTargetPredictor {
    uint64_t target_id;
    uint64_t sample_ns;
    CfVec3 last;
    CfVec3 velocity;
    uint32_t samples;
} CfTargetPredictor;
void cf_target_predictor_reset(CfTargetPredictor *);
bool cf_target_predict(CfTargetPredictor *, uint64_t, const CfVec3 *, uint64_t, CfVec3 *);
bool cf_aim_from_snapshot(const CfSceneSnapshot *, const CfViewport *, uint64_t locked,
                          float lock_projection, uint64_t now, CfAimResult *);
#endif
