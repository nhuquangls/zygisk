#ifndef CF_SCENE_SNAPSHOT_H
#define CF_SCENE_SNAPSHOT_H
#include "aim_math.h"
#include "readonly_module.h"
#define CF_SNIPER_SCOPE_PROJECTION_MIN 5.30f
typedef struct CfScene CfScene;
typedef struct CfSceneSnapshot {
    CfMatrix matrix;
    CfCandidate candidates[256];
    size_t count;
    uint64_t time_ns;
    int32_t frame;
    float projection_y;
    CfVec3 camera_position;
    uintptr_t local_id;
    /* Keep this byte at its established ABI offset for the policy/test harness. */
    bool builtin_aim;
    bool aim_enabled;
    bool sniper_aim_enabled;
    bool doing_aim_assist;
    uintptr_t game_aim_target;
} CfSceneSnapshot;
CfScene *cf_scene_open(const CfModule *, char *, size_t);
void cf_scene_close(CfScene *);
bool cf_scene_read(CfScene *, CfSceneSnapshot *);
bool cf_scene_read_target(CfScene *, CfSceneSnapshot *, uintptr_t);
bool cf_scene_poll(CfScene *, CfSceneSnapshot *, bool *);
bool cf_scene_sniper_scope_active(const CfSceneSnapshot *);
bool cf_scene_needs_targets(const CfSceneSnapshot *, bool, bool, uintptr_t);
bool cf_transform_position(uintptr_t, size_t, CfVec3 *);
bool cf_chest_point(const CfVec3 *, const CfVec3 *, CfVec3 *);
bool cf_scene_candidate_is_enemy(bool, int32_t, int32_t, uintptr_t, uintptr_t);
uint64_t cf_monotonic_ns(void);
#endif
