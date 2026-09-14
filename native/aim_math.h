#ifndef CF_AIM_MATH_H
#define CF_AIM_MATH_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
typedef struct CfVec3 { float x, y, z; } CfVec3;
typedef struct CfVec2 { float x, y; } CfVec2;
typedef struct CfViewport { float width, height; } CfViewport;
// Logical row-major view-projection matrix. A game adapter must convert its
// actual storage layout into this format; no Unity memory offsets are assumed.
typedef struct CfMatrix { float m[16]; } CfMatrix;
typedef struct CfCandidate {
    uint64_t id;
    CfVec3 head;
    bool enemy, alive, visible, invulnerable;
} CfCandidate;
typedef struct CfAimSettings {
    float fov_pixels, smooth;
    uint64_t max_age_ns;
} CfAimSettings;
typedef struct CfAimResult {
    uint64_t target_id;
    CfVec2 screen, delta;
} CfAimResult;
// Pure math only: neither function reads game memory or sends touch events.
bool cf_world_to_screen(const CfMatrix *matrix, const CfVec3 *point,
                        const CfViewport *viewport, CfVec2 *result);
bool cf_select_aim(const CfMatrix *matrix, const CfViewport *viewport,
                   const CfCandidate *candidates, size_t count,
                   const CfAimSettings *settings, uint64_t sample_ns,
                   uint64_t now_ns, CfAimResult *result);
#endif
