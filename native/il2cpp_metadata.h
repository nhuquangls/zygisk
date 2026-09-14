#ifndef CF_IL2CPP_METADATA_H
#define CF_IL2CPP_METADATA_H
#include "readonly_module.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef enum CfFieldId {
    CF_TARGET_LIST,
    CF_TARGET_HEALTH,
    CF_TARGET_HIDDEN,
    CF_OBJECT_TRANSFORM,
    CF_COMPONENT_ROOT,
    CF_COMPONENT_GAME,
    CF_COMPONENT_DESTROYED,
    CF_PAWN_HEAD_TRANSFORM,
    CF_PAWN_HEAD_POSITION,
    CF_PAWN_FOOT_POSITION,
    CF_PAWN_LAST_POSITION,
    CF_PAWN_PLAYER_INFO,
    CF_PAWN_CAMERA,
    CF_PAWN_CONTROLLER,
    CF_PAWN_LOCAL_CONTROLLER,
    CF_CONTROLLER_LOCAL_PLAYER,
    CF_CONTROLLER_CURRENT_TARGET,
    CF_CONTROLLER_PAWN,
    CF_CONTROLLER_SENSITIVITY,
    CF_CONTROLLER_ANDROID_RATE,
    CF_LOCAL_PLAYER_CAMP,
    CF_LOCAL_PLAYER_CONTROLLER,
    CF_METADATA_FIELD_COUNT
} CfFieldId;

typedef struct CfMetadataLayout {
    size_t offsets[CF_METADATA_FIELD_COUNT];
    uintptr_t manager;
    uintptr_t target_list;
} CfMetadataLayout;

// Lookup cached metadata and read an existing singleton. No managed/game
// method invocation, runtime_class_init, field setters, or code patches.
// Metadata queries can initialize IL2CPP's internal metadata caches.
bool cf_metadata_resolve(const CfModule *module, CfMetadataLayout *layout,
                         char *error, size_t error_size);
const char *cf_metadata_field_name(size_t index);

// Validate instance fields separately from static fields. Exported internally
// so boundary checks can be exercised against the compiled ARM64 implementation.
bool cf_metadata_instance_field_valid(size_t offset, size_t width,
                                      uint32_t instance_size, int flags,
                                      int actual_kind, int expected_kind);
#endif
