#include "scene_snapshot.h"
#include "il2cpp_api.h"
#include "il2cpp_metadata.h"
#include "readonly_memory.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

enum Extra { UPPER, CAMP, ASSIST, SNIPER, DOING, AIM_TARGET, CACHED, EXTRA_COUNT };
enum Static { MANAGER, CAMERA, POSITION, FORWARD, FRAME, STATIC_COUNT };
struct CfScene {
    MetadataApi api;
    CfMetadataLayout layout;
    void *attached, *pawn_class, *controller_class;
    uintptr_t individual_game_class;
    bool (*assignable)(void *, void *);
    size_t extra[EXTRA_COUNT], list_offsets[3];
    uintptr_t list_class;
    void *statics[STATIC_COUNT];
    struct { uintptr_t klass; unsigned roles; } allowed[32];
    size_t allowed_count;
    uintptr_t cached_pawn, cached_controller, cached_local_player;
    uint64_t next_bind_ns;
};
bool cf_scene_candidate_is_enemy(bool individual_mode, int32_t camp,
                                 int32_t local_camp, uintptr_t id,
                                 uintptr_t local_id) {
    return id != local_id && (individual_mode || camp != local_camp);
}
bool cf_scene_sniper_scope_active(const CfSceneSnapshot *snapshot) {
    return snapshot && snapshot->sniper_aim_enabled &&
           isfinite(snapshot->projection_y) &&
           snapshot->projection_y >= CF_SNIPER_SCOPE_PROJECTION_MIN &&
           snapshot->projection_y <= 20.0f;
}
bool cf_scene_needs_targets(const CfSceneSnapshot *snapshot, bool tracking,
                            bool trigger_armed, uintptr_t locked_target) {
    if (!cf_scene_sniper_scope_active(snapshot)) return false;
    bool trigger = snapshot->doing_aim_assist && snapshot->game_aim_target;
    bool fresh = trigger && (trigger_armed ||
                             snapshot->game_aim_target != locked_target);
    return tracking || fresh;
}
uint64_t cf_monotonic_ns(void) {
    struct timespec now;
    if (clock_gettime(CLOCK_MONOTONIC, &now) != 0) return 0;
    return (uint64_t)now.tv_sec * 1000000000ULL + (uint64_t)now.tv_nsec;
}
static bool read_at(uintptr_t base, size_t offset, void *out, size_t size) {
    return base >= 4096 && base <= UINTPTR_MAX - offset &&
           cf_read_self(base + offset, out, size);
}
static uintptr_t pointer(uintptr_t base, size_t offset) {
    uintptr_t p = 0;
    if (!read_at(base, offset, &p, sizeof(p)) || (p & 7)) return 0;
    return p;
}
static bool finite_array(const float *v, size_t n) {
    for (size_t i = 0; i < n; ++i) if (!isfinite(v[i])) return false;
    return true;
}
static CfVec3 cross(CfVec3 a, CfVec3 b) {
    return (CfVec3){a.y*b.z-a.z*b.y, a.z*b.x-a.x*b.z, a.x*b.y-a.y*b.x};
}
bool cf_chest_point(const CfVec3 *root, const CfVec3 *upper, CfVec3 *out) {
    if (!out) return false;
    *out = (CfVec3){0};
    if (!root || !upper || !isfinite(root->x) || !isfinite(root->y) ||
        !isfinite(root->z) || !isfinite(upper->x) || !isfinite(upper->y) ||
        !isfinite(upper->z)) return false;
    float x = upper->x-root->x, y = upper->y-root->y, z = upper->z-root->z;
    float span = sqrtf(x*x + y*y + z*z);
    if (!isfinite(span) || span < .25f || span > 2.5f) return false;
    *out = (CfVec3){root->x + x*.78f, root->y + y*.78f, root->z + z*.78f};
    return true;
}
bool cf_transform_position(uintptr_t managed, size_t cached, CfVec3 *out) {
    if (!out) return false;
    *out = (CfVec3){0};
    uintptr_t native = pointer(managed, cached), data = pointer(native, 0x40);
    uintptr_t trs = pointer(data, 8), parents = pointer(data, 16);
    uint32_t index;
    if (!trs || !parents || !read_at(native, 0x48, &index, 4) || index >= 65536) return false;
    CfVec3 value;
    int32_t parent;
    uint32_t seen[128], depth = 1;
    seen[0] = index;
    if (!read_at(trs, index * 40, &value, 12) ||
        !read_at(parents, index * 4, &parent, 4)) return false;
    while (parent >= 0) {
        if (parent >= 65536 || depth >= 128) return false;
        for (uint32_t i = 0; i < depth; ++i) if (seen[i] == (uint32_t)parent) return false;
        seen[depth++] = (uint32_t)parent;
        float row[10];
        if (!read_at(trs, (size_t)parent * 40, row, sizeof(row)) || !finite_array(row, 10)) return false;
        float norm = row[3]*row[3]+row[4]*row[4]+row[5]*row[5]+row[6]*row[6];
        if (fabsf(norm - 1) > .01f) return false;
        CfVec3 v = {value.x*row[7], value.y*row[8], value.z*row[9]};
        CfVec3 q = {row[3], row[4], row[5]}, a = cross(q, v), b = cross(q, a);
        value = (CfVec3){row[0]+v.x+2*row[6]*a.x+2*b.x,
                         row[1]+v.y+2*row[6]*a.y+2*b.y,
                         row[2]+v.z+2*row[6]*a.z+2*b.z};
        if (!read_at(parents, (size_t)parent * 4, &parent, 4)) return false;
    }
    if (parent != -1 || !isfinite(value.x) || !isfinite(value.y) || !isfinite(value.z)) return false;
    // Reject a Transform reparented while its chain was being copied.
    if (pointer(native, 0x40) != data) return false;
    uint32_t after;
    if (!read_at(native, 0x48, &after, 4) || after != index) return false;
    *out = value;
    return true;
}
static void *get_field(CfScene *s, void *klass, const char *name, int kind, bool stat) {
    if (!klass) return NULL;
    void *f = s->api.class_get_field_from_name(klass, name);
    if (!f || !!(s->api.field_get_flags(f)&0x10) != stat || (s->api.field_get_flags(f)&0x40) ||
        s->api.type_get_type(s->api.field_get_type(f)) != kind) return NULL;
    return f;
}
static bool offset(CfScene *s, void *klass, const char *name, int kind, size_t width, size_t *out) {
    void *f = get_field(s, klass, name, kind, false);
    if (!f) return false;
    *out = s->api.field_get_offset(f);
    return cf_metadata_instance_field_valid(*out, width, s->api.class_instance_size(klass),
                                            s->api.field_get_flags(f), kind, kind);
}
CfScene *cf_scene_open(const CfModule *module, char *error, size_t size) {
    CfModule unity;
    if (!cf_find_module("libunity.so", &unity) ||
        strcmp(unity.build_id, "1a60ff52f7bb4ad5de0465b12a83aba3d7af0700") != 0 ||
        strcmp(module->build_id, "a8793b51fee671e98de0cc0ad42bb85ffd5d0677") != 0) {
        snprintf(error, size, "unsupported Unity/IL2CPP build ID");
        return NULL;
    }
    // Verified on the pinned IL2CPP build: the thread-registration routine
    // called by thread_attach aborts while this runtime readiness flag is zero.
    // A published assembly list alone is insufficient during cold startup.
    const uintptr_t registration_rva = 0xbe000e0;
    uint32_t registration_ready = 0;
    if (module->load_bias > UINTPTR_MAX - registration_rva ||
        !cf_read_self(module->load_bias + registration_rva, &registration_ready,
                      sizeof(registration_ready)) || registration_ready != 1) {
        snprintf(error, size, "runtime thread registration not ready");
        return NULL;
    }
    CfScene *s = calloc(1, sizeof(*s));
    if (!s) return NULL;
    CfExports ex;
    if (!cf_metadata_resolve(module, &s->layout, error, size) || !cf_exports_open(module, &ex) ||
        !cf_metadata_api_open(&ex, &s->api, error, size)) goto fail;
    s->assignable = (bool (*)(void *, void *))cf_exports_function(&ex, "il2cpp_class_is_assignable_from");
    if (!s->assignable) goto fail;
    if (!s->api.thread_current()) {
        s->attached = s->api.thread_attach(s->api.domain_get());
        if (!s->attached) goto fail;
    }
    size_t n = 0;
    const void **assemblies = s->api.domain_get_assemblies(s->api.domain_get(), &n);
    const void *game = NULL, *unity_image = NULL;
    if (!assemblies || n > 1024) goto fail;
    for (size_t i = 0; i < n; ++i) {
        const void *im = s->api.assembly_get_image(assemblies[i]);
        const char *name = im ? s->api.image_get_name(im) : NULL;
        if (name && !strcmp(name, "Assembly-CSharp.dll")) game = im;
        if (name && !strcmp(name, "UnityEngine.dll")) unity_image = im;
    }
    if (!game || !unity_image) goto fail;
    struct { const char *ns, *cl, *field; int kind; size_t width; } specs[] = {
        {"WNEngine", "Pawn", "m_CachedUpperBodyTransform", 18, 8},
        {"WNEngine", "PlayerInfo", "m_Camp", 17, 4},
        {"WNEngine", "PlayerController", "m_EnableAimAssistance", 2, 1},
        {"WNEngine", "PlayerController", "m_EnableAimAssistanceForSniper", 2, 1},
        {"WNEngine", "PlayerController", "m_DoingAimAssist", 2, 1},
        {"WNEngine", "PlayerController", "m_CurrentAimAssistTarget", 18, 8},
        {"UnityEngine", "Object", "m_CachedPtr", 24, 8},
    };
    for (size_t i = 0; i < EXTRA_COUNT; ++i) {
        void *cl = s->api.class_from_name(i == CACHED ? unity_image : game, specs[i].ns, specs[i].cl);
        if (!offset(s, cl, specs[i].field, specs[i].kind, specs[i].width, &s->extra[i])) {
            snprintf(error, size, "invalid field %s.%s", specs[i].cl, specs[i].field);
            goto fail;
        }
    }
    s->pawn_class = s->api.class_from_name(game, "WNGameBase", "WNPawn");
    s->controller_class = s->api.class_from_name(game, "WNEngine", "PlayerController");
    void *mc = s->api.class_from_name(game, "WNEngine", "AttackableTargetsManager");
    void *cc = s->api.class_from_name(game, "WNEngine", "PlayerCamera");
    const char *names[] = {"_Instance", "m_WorldCamera", "m_WorldCameraPosition", "m_WorldCameraForwardDir", "LastWorldCameraUpdateFrame"};
    const int kinds[] = {18, 18, 17, 17, 8};
    for (int i = 0; i < STATIC_COUNT; ++i) {
        s->statics[i] = get_field(s, i == MANAGER ? mc : cc, names[i], kinds[i], true);
        if (!s->statics[i]) { snprintf(error, size, "invalid static %s", names[i]); goto fail; }
    }
    if (!s->pawn_class || !s->controller_class) goto fail;
    void *(*from_type)(void *) = (void *(*)(void *))cf_exports_function(&ex, "il2cpp_class_from_type");
    if (!from_type) { snprintf(error, size, "missing il2cpp_class_from_type"); goto fail; }
    void *info_class = s->api.class_from_name(game, "WNEngine", "PlayerInfo");
    void *lp_class = s->api.class_from_name(game, "WNEngine", "LocalPlayer");
    s->individual_game_class = (uintptr_t)s->api.class_from_name(
        game, "WNIndivdualGame", "IndivdualGame");
    if (!info_class || !lp_class || !s->individual_game_class) goto fail;
    // This IL2CPP exports no image_get_class/count. Resolve the class names seen
    // in live metadata; unknown subclasses are skipped without dereferencing a
    // possibly raced object header inside an IL2CPP API.
    const char *known[][2] = {
        {"WNGameBase", "WNPawn"}, {"WNPVPGame", "PVPPlayerPawn"},
        {"WNPVPGame.WNShootingTrainGame", "ShootingTrainPlayerPawn"},
        {"WNPVPGame.WNBombGame", "BombGamePlayerPawn"},
        {"WNEngine", "PlayerController"}, {"WNGameBase", "WNPlayerController"},
        {"WNPVPGame", "PVPPlayerController"},
        {"WNPVPGame.WNTeamGame", "TeamGamePlayerController"},
        {"WNPVPGame.WNBombGame", "BombGamePlayerController"},
        {"WNPVPGame.WNShootingTrainGame", "ShootingTrainPlayerController"},
        {"WNEngine", "PlayerInfo"}, {"WNGameBase", "WNPlayerInfo"},
        {"WNPVPGame", "PVPPlayerInfo"},
        {"WNPVPGame.WNShootingTrainGame", "ShootingTrainPlayerInfo"},
        {"WNEngine", "LocalPlayer"}
    };
    for (size_t i = 0; i < sizeof(known)/sizeof(known[0]); ++i) {
        void *cl = s->api.class_from_name(game, known[i][0], known[i][1]);
        if (!cl) continue;
        unsigned roles = (s->assignable(s->pawn_class, cl) ? 1u : 0u) |
                         (s->assignable(s->controller_class, cl) ? 2u : 0u) |
                         (s->assignable(info_class, cl) ? 4u : 0u) |
                         (s->assignable(lp_class, cl) ? 8u : 0u);
        if (!roles) continue;
        s->allowed[s->allowed_count].klass = (uintptr_t)cl;
        s->allowed[s->allowed_count++].roles = roles;
    }
    void *lf = get_field(s, mc, "m_AttackableTargetList", 21, false);
    void *lc = lf ? from_type(s->api.field_get_type(lf)) : NULL;
    if (!lc || !offset(s, lc, "_items", 29, 8, &s->list_offsets[0]) ||
        !offset(s, lc, "_size", 8, 4, &s->list_offsets[1]) ||
        !offset(s, lc, "_version", 8, 4, &s->list_offsets[2])) {
        snprintf(error, size, "target list class/fields invalid: class=%p items=%zu size=%zu version=%zu", lc,
                 s->list_offsets[0], s->list_offsets[1], s->list_offsets[2]); goto fail;
    }
    s->list_class = (uintptr_t)lc;
    error[0] = 0;
    return s;
fail:
    if (!error[0]) snprintf(error, size, "scene metadata unavailable");
    cf_scene_close(s);
    return NULL;
}
void cf_scene_close(CfScene *s) {
    if (!s) return;
    if (s->attached) s->api.thread_detach(s->attached);
    free(s);
}
static uintptr_t static_pointer(CfScene *s, int id) {
    uintptr_t p = 0;
    s->api.field_static_get_value(s->statics[id], &p);
    return p;
}
static bool is_instance(CfScene *s, uintptr_t object, unsigned role) {
    uintptr_t cl = pointer(object, 0);
    if (!cl) return false;
    for (size_t i = 0; i < s->allowed_count; ++i)
        if (s->allowed[i].klass == cl) return !!(s->allowed[i].roles & role);
    return false;
}
#define CF_SCENE_REBIND_NS 250000000ULL

static void clear_cached_local(CfScene *s) {
    s->cached_pawn = 0;
    s->cached_controller = 0;
    s->cached_local_player = 0;
}

/*
 * The idle path deliberately reads only stable ownership links, the three
 * aim flags/current target and the single projection coefficient used as the
 * sniper-scope gate. It does not touch the attackable list, transforms or the
 * full view/projection matrices.
 */
static bool poll_cached_local(CfScene *s, CfSceneSnapshot *out, uint64_t now) {
    const size_t *o = s->layout.offsets;
    uintptr_t pawn = s->cached_pawn;
    uintptr_t controller = s->cached_controller;
    uintptr_t local_player = s->cached_local_player;
    if (!is_instance(s, pawn, 1) || !is_instance(s, controller, 2) ||
        !is_instance(s, local_player, 8) ||
        pointer(pawn, o[CF_PAWN_CONTROLLER]) != controller ||
        pointer(controller, o[CF_CONTROLLER_PAWN]) != pawn ||
        pointer(controller, o[CF_CONTROLLER_LOCAL_PLAYER]) != local_player ||
        pointer(local_player, o[CF_LOCAL_PLAYER_CONTROLLER]) != controller)
        return false;

    int32_t camp = 0;
    if (!read_at(local_player, o[CF_LOCAL_PLAYER_CAMP], &camp, sizeof(camp)) ||
        (camp != 1 && camp != 2))
        return false;

    uintptr_t camera = static_pointer(s, CAMERA);
    uintptr_t native_camera = pointer(camera, s->extra[CACHED]);
    float projection_y = 0, projection_after = 0;
    if (!read_at(native_camera, 0x88 + 5 * sizeof(float), &projection_y,
                 sizeof(projection_y)) ||
        !isfinite(projection_y) || projection_y <= 0 || projection_y > 20.0f)
        return false;

    uint8_t flags[3];
    for (int j = 0; j < 3; ++j)
        if (!read_at(controller, s->extra[ASSIST+j], &flags[j], 1))
            return false;
    uintptr_t target = pointer(controller, s->extra[AIM_TARGET]);

    if (static_pointer(s, CAMERA) != camera ||
        pointer(pawn, o[CF_PAWN_CONTROLLER]) != controller ||
        pointer(controller, o[CF_CONTROLLER_PAWN]) != pawn ||
        !read_at(native_camera, 0x88 + 5 * sizeof(float), &projection_after,
                 sizeof(projection_after)) || projection_after != projection_y)
        return false;

    memset(out, 0, sizeof(*out));
    out->time_ns = now;
    out->local_id = pawn;
    out->projection_y = projection_y;
    out->aim_enabled = flags[0] != 0;
    out->sniper_aim_enabled = flags[1] != 0;
    out->doing_aim_assist = flags[2] != 0;
    out->game_aim_target = target;
    out->builtin_aim = flags[0] || flags[1] || flags[2];
    return true;
}

static bool scene_read_full(CfScene *s, CfSceneSnapshot *out) {
    if (!s || !out) return false;
    memset(out, 0, sizeof(*out));
    uint64_t started = cf_monotonic_ns();
    const size_t *o = s->layout.offsets;
    uintptr_t manager = static_pointer(s, MANAGER);
    uintptr_t list = pointer(manager, o[CF_TARGET_LIST]), list_class = pointer(list, 0);
    if (!list_class || list_class != s->list_class) return false;
    int32_t count = 0, version = 0;
    uintptr_t items = pointer(list, s->list_offsets[0]);
    uint64_t capacity = 0;
    if (!read_at(list, s->list_offsets[1], &count, 4) || !read_at(list, s->list_offsets[2], &version, 4) ||
        count <= 0 || count > 256 || !read_at(items, 24, &capacity, 8) || capacity < (uint32_t)count || capacity > 65536) return false;
    uintptr_t objects[256];
    if (!read_at(items, 32, objects, (size_t)count * 8)) return false;
    uintptr_t camera = static_pointer(s, CAMERA), native_camera = pointer(camera, s->extra[CACHED]);
    float view[16], projection[16], after[16];
    CfVec3 forward;
    s->api.field_static_get_value(s->statics[FRAME], &out->frame);
    s->api.field_static_get_value(s->statics[POSITION], &out->camera_position);
    s->api.field_static_get_value(s->statics[FORWARD], &forward);
    if (!read_at(native_camera, 0x48, view, sizeof(view)) || !read_at(native_camera, 0x88, projection, sizeof(projection)) ||
        !finite_array(view, 16) || !finite_array(projection, 16) ||
        !isfinite(out->camera_position.x) || !isfinite(out->camera_position.y) || !isfinite(out->camera_position.z) ||
        !isfinite(forward.x) || !isfinite(forward.y) || !isfinite(forward.z)) return false;
    CfVec3 p = out->camera_position;
    float x = view[0]*p.x+view[4]*p.y+view[8]*p.z+view[12];
    float y = view[1]*p.x+view[5]*p.y+view[9]*p.z+view[13];
    float z = view[2]*p.x+view[6]*p.y+view[10]*p.z+view[14];
    if (x*x+y*y+z*z > .01f || fabsf(view[2]+forward.x) > .02f ||
        fabsf(view[6]+forward.y) > .02f || fabsf(view[10]+forward.z) > .02f ||
        projection[0] <= 0 || projection[5] <= 0 || fabsf(projection[11]+1) > .00001f || fabsf(projection[15]) > .00001f) return false;
    out->projection_y = projection[5];
    for (int r = 0; r < 4; ++r) for (int c = 0; c < 4; ++c) {
        float sum = 0;
        for (int k = 0; k < 4; ++k) sum += projection[k*4+r]*view[c*4+k];
        out->matrix.m[r*4+c] = sum;
    }
    int32_t camps[256], local_camp = 0;
    uintptr_t local_game = 0, local_controller = 0, local_player = 0;
    bool individual_mode = false;
    for (int32_t i = 0; i < count; ++i) {
        uintptr_t obj = objects[i];
        uint8_t destroyed, hidden;
        float health;
        int32_t camp;
        if (!is_instance(s, obj, 1) || !read_at(obj, o[CF_COMPONENT_DESTROYED], &destroyed, 1) || destroyed) continue;
        uintptr_t info = pointer(obj, o[CF_PAWN_PLAYER_INFO]);
        if (!is_instance(s, info, 4)) continue;
        if (!read_at(info, s->extra[CAMP], &camp, 4) || (camp != 1 && camp != 2) ||
            !read_at(obj, o[CF_TARGET_HEALTH], &health, 4) || !isfinite(health) || health <= 0 ||
            !read_at(obj, o[CF_TARGET_HIDDEN], &hidden, 1)) continue;
        uintptr_t ctrl = pointer(obj, o[CF_PAWN_CONTROLLER]);
        if (ctrl && is_instance(s, ctrl, 2) && pointer(ctrl, o[CF_CONTROLLER_PAWN]) == obj) {
            uintptr_t lp = pointer(ctrl, o[CF_CONTROLLER_LOCAL_PLAYER]);
            int32_t lc;
            if (lp && is_instance(s, lp, 8) && pointer(lp, o[CF_LOCAL_PLAYER_CONTROLLER]) == ctrl &&
                read_at(lp, o[CF_LOCAL_PLAYER_CAMP], &lc, 4) && lc == camp) {
                if (out->local_id && out->local_id != obj) return false;
                out->local_id = obj;
                local_camp = camp;
                local_controller = ctrl;
                local_player = lp;
                local_game = pointer(obj, o[CF_COMPONENT_GAME]);
                uintptr_t game_class = pointer(local_game, 0);
                if (!game_class) return false;
                individual_mode = game_class == s->individual_game_class;
                uint8_t flags[3];
                for (int j = 0; j < 3; ++j)
                    if (!read_at(ctrl, s->extra[ASSIST+j], &flags[j], 1)) return false;
                out->aim_enabled = flags[0] != 0;
                out->sniper_aim_enabled = flags[1] != 0;
                out->doing_aim_assist = flags[2] != 0;
                out->game_aim_target = pointer(ctrl, s->extra[AIM_TARGET]);
                out->builtin_aim = flags[0] || flags[1] || flags[2];
            }
        }
        CfVec3 root, upper, point;
        if (hidden ||
            !cf_transform_position(pointer(obj, o[CF_OBJECT_TRANSFORM]), s->extra[CACHED], &root) ||
            !cf_transform_position(pointer(obj, s->extra[UPPER]), s->extra[CACHED], &upper)) continue;
        // UpperBody sits near the shoulder/neck and bobs with animation. Blend
        // from the pawn root to 78% of that span for a stable mid-chest point.
        if (!cf_chest_point(&root, &upper, &point)) continue;
        camps[out->count] = camp;
        out->candidates[out->count++] = (CfCandidate){obj, point, false, true, true, false};
    }
    if (!out->local_id) return false;
    for (size_t i = 0; i < out->count; ++i)
        out->candidates[i].enemy = cf_scene_candidate_is_enemy(
            individual_mode, camps[i], local_camp,
            out->candidates[i].id, out->local_id);
    int32_t frame_after, count_after, version_after;
    s->api.field_static_get_value(s->statics[FRAME], &frame_after);
    if (frame_after != out->frame || static_pointer(s, CAMERA) != camera || static_pointer(s, MANAGER) != manager ||
        pointer(out->local_id, o[CF_COMPONENT_GAME]) != local_game ||
        pointer(manager, o[CF_TARGET_LIST]) != list || pointer(list, s->list_offsets[0]) != items ||
        !read_at(list, s->list_offsets[1], &count_after, 4) || count_after != count ||
        !read_at(list, s->list_offsets[2], &version_after, 4) || version_after != version ||
        !read_at(native_camera, 0x48, after, sizeof(after)) || memcmp(view, after, sizeof(after)) ||
        !read_at(native_camera, 0x88, after, sizeof(after)) || memcmp(projection, after, sizeof(after))) return false;
    out->time_ns = started;
    uint64_t finished = cf_monotonic_ns();
    if (!started || finished < started || finished-started > 50000000ULL)
        return false;
    s->cached_pawn = out->local_id;
    s->cached_controller = local_controller;
    s->cached_local_player = local_player;
    s->next_bind_ns = 0;
    return true;
}

static void suppress_active_target(CfSceneSnapshot *out) {
    out->count = 0;
    out->local_id = 0;
    out->doing_aim_assist = false;
    out->game_aim_target = 0;
}

/*
 * Active aim trusts the target already selected by the game's aim-assist
 * state. Read that pawn directly; do not traverse AttackableTargetList or
 * inspect unrelated pawns on every 8 ms controller tick.
 */
static bool scene_read_current_target(CfScene *s, CfSceneSnapshot *out,
                                      uintptr_t retained_target) {
    if (!s || !out) return false;
    uint64_t started = cf_monotonic_ns();
    if (!started || !poll_cached_local(s, out, started)) return false;
    if (!cf_scene_sniper_scope_active(out)) return true;

    const size_t *o = s->layout.offsets;
    bool game_trigger = out->doing_aim_assist && out->game_aim_target;
    uintptr_t target = game_trigger ? out->game_aim_target : retained_target;
    if (!target) return true;
    if (target == out->local_id || !is_instance(s, target, 1)) {
        suppress_active_target(out);
        return true;
    }

    uint8_t destroyed = 0, hidden = 0;
    float health = 0;
    if (!read_at(target, o[CF_COMPONENT_DESTROYED], &destroyed, 1) ||
        !read_at(target, o[CF_TARGET_HIDDEN], &hidden, 1) ||
        !read_at(target, o[CF_TARGET_HEALTH], &health, sizeof(health)))
        return false;
    if (destroyed || hidden || !isfinite(health) || health <= 0) {
        suppress_active_target(out);
        return true;
    }

    CfVec3 root, upper, point;
    if (!cf_transform_position(pointer(target, o[CF_OBJECT_TRANSFORM]),
                               s->extra[CACHED], &root) ||
        !cf_transform_position(pointer(target, s->extra[UPPER]),
                               s->extra[CACHED], &upper))
        return false;
    if (!cf_chest_point(&root, &upper, &point)) {
        suppress_active_target(out);
        return true;
    }

    uintptr_t camera = static_pointer(s, CAMERA);
    uintptr_t native_camera = pointer(camera, s->extra[CACHED]);
    float view[16], projection[16], after[16];
    CfVec3 forward;
    s->api.field_static_get_value(s->statics[FRAME], &out->frame);
    s->api.field_static_get_value(s->statics[POSITION], &out->camera_position);
    s->api.field_static_get_value(s->statics[FORWARD], &forward);
    if (!read_at(native_camera, 0x48, view, sizeof(view)) ||
        !read_at(native_camera, 0x88, projection, sizeof(projection)) ||
        !finite_array(view, 16) || !finite_array(projection, 16) ||
        !isfinite(out->camera_position.x) ||
        !isfinite(out->camera_position.y) ||
        !isfinite(out->camera_position.z) ||
        !isfinite(forward.x) || !isfinite(forward.y) || !isfinite(forward.z))
        return false;

    CfVec3 p = out->camera_position;
    float x = view[0]*p.x+view[4]*p.y+view[8]*p.z+view[12];
    float y = view[1]*p.x+view[5]*p.y+view[9]*p.z+view[13];
    float z = view[2]*p.x+view[6]*p.y+view[10]*p.z+view[14];
    if (x*x+y*y+z*z > .01f || fabsf(view[2]+forward.x) > .02f ||
        fabsf(view[6]+forward.y) > .02f ||
        fabsf(view[10]+forward.z) > .02f || projection[0] <= 0 ||
        projection[5] <= 0 || fabsf(projection[11]+1) > .00001f ||
        fabsf(projection[15]) > .00001f)
        return false;
    out->projection_y = projection[5];
    for (int r = 0; r < 4; ++r) for (int c = 0; c < 4; ++c) {
        float sum = 0;
        for (int k = 0; k < 4; ++k)
            sum += projection[k*4+r]*view[c*4+k];
        out->matrix.m[r*4+c] = sum;
    }

    int32_t frame_after;
    s->api.field_static_get_value(s->statics[FRAME], &frame_after);
    if (frame_after != out->frame || static_pointer(s, CAMERA) != camera ||
        !read_at(native_camera, 0x48, after, sizeof(after)) ||
        memcmp(view, after, sizeof(after)) ||
        !read_at(native_camera, 0x88, after, sizeof(after)) ||
        memcmp(projection, after, sizeof(after)))
        return false;

    CfSceneSnapshot final_hint;
    if (!poll_cached_local(s, &final_hint, started)) return false;
    if (!cf_scene_sniper_scope_active(&final_hint)) {
        *out = final_hint;
        return true;
    }
    bool final_trigger = final_hint.doing_aim_assist &&
                         final_hint.game_aim_target;
    if (final_trigger && final_hint.game_aim_target != target) {
        *out = final_hint;
        return true;
    }
    if (final_hint.projection_y != projection[5]) return false;
    out->builtin_aim = final_hint.builtin_aim;
    out->aim_enabled = final_hint.aim_enabled;
    out->sniper_aim_enabled = final_hint.sniper_aim_enabled;
    out->doing_aim_assist = final_hint.doing_aim_assist;
    out->game_aim_target = final_hint.game_aim_target;
    out->candidates[0] = (CfCandidate){target, point, true, true, true, false};
    out->count = 1;
    out->time_ns = started;

    uint64_t finished = cf_monotonic_ns();
    return finished >= started && finished-started <= 50000000ULL;
}

bool cf_scene_read(CfScene *s, CfSceneSnapshot *out) {
    return scene_read_current_target(s, out, 0);
}

bool cf_scene_read_target(CfScene *s, CfSceneSnapshot *out,
                          uintptr_t retained_target) {
    return scene_read_current_target(s, out, retained_target);
}

bool cf_scene_poll(CfScene *s, CfSceneSnapshot *out, bool *complete) {
    if (!s || !out || !complete) return false;
    memset(out, 0, sizeof(*out));
    *complete = false;
    uint64_t now = cf_monotonic_ns();
    if (!now) return false;

    if (s->cached_pawn) {
        if (poll_cached_local(s, out, now)) return true;
        clear_cached_local(s);
        s->next_bind_ns = 0;
    }

    if (now < s->next_bind_ns) {
        out->time_ns = now;
        return true;
    }
    if (scene_read_full(s, out)) {
        *complete = true;
        return true;
    }

    memset(out, 0, sizeof(*out));
    out->time_ns = now;
    s->next_bind_ns = now <= UINT64_MAX - CF_SCENE_REBIND_NS
                      ? now + CF_SCENE_REBIND_NS : UINT64_MAX;
    return true;
}
