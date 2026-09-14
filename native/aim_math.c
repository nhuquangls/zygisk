#include "aim_math.h"
#include <math.h>
#include <string.h>

static bool valid_viewport(const CfViewport *viewport) {
    return viewport != NULL && isfinite(viewport->width) &&
           isfinite(viewport->height) && viewport->width > 0 &&
           viewport->height > 0 && viewport->width <= 32768 &&
           viewport->height <= 32768;
}

bool cf_world_to_screen(const CfMatrix *matrix, const CfVec3 *point,
                        const CfViewport *viewport, CfVec2 *result) {
    if (result == NULL) return false;
    *result = (CfVec2){0};
    if (matrix == NULL || point == NULL || !valid_viewport(viewport) ||
        !isfinite(point->x) || !isfinite(point->y) || !isfinite(point->z))
        return false;
    for (size_t i = 0; i < 16; ++i)
        if (!isfinite(matrix->m[i])) return false;
    const float *m = matrix->m;
    float x = m[0]*point->x + m[1]*point->y + m[2]*point->z + m[3];
    float y = m[4]*point->x + m[5]*point->y + m[6]*point->z + m[7];
    float w = m[12]*point->x + m[13]*point->y + m[14]*point->z + m[15];
    if (!isfinite(x) || !isfinite(y) || !isfinite(w) || w <= 0.01f)
        return false;
    x /= w;
    y /= w;
    if (!isfinite(x) || !isfinite(y) || fabsf(x) > 1.0f || fabsf(y) > 1.0f)
        return false;
    result->x = (1.0f + x) * viewport->width * 0.5f;
    result->y = (1.0f - y) * viewport->height * 0.5f;
    return true;
}

bool cf_select_aim(const CfMatrix *matrix, const CfViewport *viewport,
                   const CfCandidate *candidates, size_t count,
                   const CfAimSettings *settings, uint64_t sample_ns,
                   uint64_t now_ns, CfAimResult *result) {
    if (result == NULL) return false;
    memset(result, 0, sizeof(*result));
    if (matrix == NULL || !valid_viewport(viewport) || candidates == NULL ||
        count == 0 || count > 256 || settings == NULL ||
        !isfinite(settings->fov_pixels) || settings->fov_pixels <= 0 ||
        settings->fov_pixels > 32768 || !isfinite(settings->smooth) ||
        settings->smooth <= 0 || settings->smooth > 1 ||
        settings->max_age_ns == 0 || now_ns < sample_ns ||
        now_ns - sample_ns > settings->max_age_ns)
        return false;
    float closest = settings->fov_pixels * settings->fov_pixels;
    bool found = false;
    for (size_t i = 0; i < count; ++i) {
        const CfCandidate *candidate = &candidates[i];
        if (candidate->id == 0 || !candidate->enemy || !candidate->alive ||
            !candidate->visible || candidate->invulnerable) continue;
        CfVec2 screen;
        if (!cf_world_to_screen(matrix, &candidate->head, viewport, &screen))
            continue;
        float dx = screen.x - viewport->width * 0.5f;
        float dy = screen.y - viewport->height * 0.5f;
        float distance = dx*dx + dy*dy;
        if (distance > closest || (found && distance == closest)) continue;
        closest = distance;
        found = true;
        result->target_id = candidate->id;
        result->screen = screen;
        result->delta = (CfVec2){dx * settings->smooth, dy * settings->smooth};
    }
    return found;
}
