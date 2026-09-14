#define _GNU_SOURCE
#include "il2cpp_metadata.h"
#include "readonly_memory.h"
#include "readonly_exports.h"
#include "il2cpp_api.h"
#include <stdio.h>
#include <string.h>

typedef struct FieldSpec {
    const char *namespaze, *klass, *field;
    int kind;
    size_t width;
} FieldSpec;

// Names/types verified against runtime metadata from the running game. Offsets
// are deliberately resolved again instead of compiling process-specific values.
static const FieldSpec k_fields[] = {
    {"WNEngine", "AttackableTargetsManager", "m_AttackableTargetList", 21, 8},
    {"WNEngine", "AttackableTarget", "m_Health", 12, 4},
    {"WNEngine", "AttackableTarget", "m_IsHidden", 2, 1},
    {"WNCore", "BaseObject", "m_Transform", 18, 8},
    {"WNEngine", "BaseComponent", "m_Root", 18, 8},
    {"WNEngine", "BaseComponent", "m_Game", 18, 8},
    {"WNEngine", "BaseComponent", "bHasDestroy", 2, 1},
    {"WNEngine", "Pawn", "m_HeadTransform", 18, 8},
    {"WNEngine", "Pawn", "HeadCharacterPosition", 17, 12},
    {"WNEngine", "Pawn", "FootCharacterPosition", 17, 12},
    {"WNEngine", "Pawn", "LastPlayerPosition", 17, 12},
    {"WNEngine", "Pawn", "m_PlayerInfo", 18, 8},
    {"WNEngine", "Pawn", "m_FirstPersonCamera", 18, 8},
    {"WNEngine", "Pawn", "m_Controller", 18, 8},
    {"WNEngine", "Pawn", "m_LocalPlayerController", 18, 8},
    {"WNEngine", "PlayerController", "m_LocalPlayer", 18, 8},
    {"WNEngine", "PlayerController", "m_CurrentAimAssistTarget", 18, 8},
    {"WNEngine", "Controller", "m_Pawn", 18, 8},
    {"WNEngine", "PlayerController", "RotationSensity", 12, 4},
    {"WNEngine", "PlayerController", "m_AndroidCameraRotateRate", 12, 4},
    {"WNEngine", "LocalPlayer", "m_Camp", 17, 4},
    {"WNEngine", "LocalPlayer", "m_PlayerController", 18, 8},
};
_Static_assert(sizeof(k_fields) / sizeof(k_fields[0]) == CF_METADATA_FIELD_COUNT,
               "field enum and specifications must agree");

const char *cf_metadata_field_name(size_t index) {
    return index < CF_METADATA_FIELD_COUNT ? k_fields[index].field : NULL;
}

bool cf_metadata_instance_field_valid(size_t offset, size_t width,
                                      uint32_t instance_size, int flags,
                                      int actual_kind, int expected_kind) {
    return !(flags & (0x10 | 0x40)) && actual_kind == expected_kind &&
           width > 0 && offset >= 16 && offset <= instance_size &&
           width <= instance_size - offset;
}

bool cf_metadata_api_open(const CfExports *exports, MetadataApi *api,
                         char *error, size_t size) {
#define GET(member, type) do { \
    uintptr_t address = cf_exports_function(exports, "il2cpp_" #member); \
    if (address == 0) { \
        snprintf(error, size, "missing/unverified API: il2cpp_%s", #member); \
        return false; \
    } \
    api->member = (type)address; \
} while (0)
    GET(domain_get, void *(*)(void));
    GET(domain_get_assemblies, const void **(*)(const void *, size_t *));
    GET(assembly_get_image, const void *(*)(const void *));
    GET(image_get_name, const char *(*)(const void *));
    GET(class_from_name, void *(*)(const void *, const char *, const char *));
    GET(class_get_field_from_name, void *(*)(void *, const char *));
    GET(class_instance_size, uint32_t (*)(void *));
    GET(field_get_offset, size_t (*)(void *));
    GET(field_get_flags, int (*)(void *));
    GET(field_get_type, void *(*)(void *));
    GET(type_get_type, int (*)(void *));
    GET(field_static_get_value, void (*)(void *, void *));
    GET(thread_current, void *(*)(void));
    GET(thread_attach, void *(*)(void *));
    GET(thread_detach, void (*)(void *));
#undef GET
    return true;
}

bool cf_metadata_resolve(const CfModule *module, CfMetadataLayout *layout,
                         char *error, size_t error_size) {
    if (layout == NULL || error == NULL || error_size == 0) return false;
    memset(layout, 0, sizeof(*layout));
    error[0] = '\0';
    CfExports exports;
    if (!cf_exports_open(module, &exports)) {
        snprintf(error, error_size, "IL2CPP ELF export table unavailable");
        return false;
    }
    MetadataApi api = {0};
    bool ok = false;
    void *attached = NULL;
    if (!cf_metadata_api_open(&exports, &api, error, error_size)) goto done;
    void *domain = api.domain_get();
    if (domain == NULL) {
        snprintf(error, error_size, "IL2CPP domain not initialized");
        goto done;
    }
    // domain_get may return its static domain before runtime initialization.
    // The installed startup path hit an abort in thread_attach at that point.
    // Wait for the game's assembly to be published before attaching a thread.
    size_t count = 0;
    const void **assemblies = api.domain_get_assemblies(domain, &count);
    if (assemblies == NULL || count == 0 || count > 1024) {
        snprintf(error, error_size, "invalid assembly list");
        goto done;
    }
    const void *game = NULL;
    for (size_t i = 0; i < count; ++i) {
        const void *image = api.assembly_get_image(assemblies[i]);
        if (image == NULL) continue;
        const char *name = api.image_get_name(image);
        if (name != NULL && strcmp(name, "Assembly-CSharp.dll") == 0) {
            game = image;
            break;
        }
    }
    if (game == NULL) {
        snprintf(error, error_size, "Assembly-CSharp.dll not ready");
        goto done;
    }
    if (api.thread_current() == NULL) {
        attached = api.thread_attach(domain);
        if (attached == NULL) {
            snprintf(error, error_size, "IL2CPP thread attach failed");
            goto done;
        }
    }
    for (size_t i = 0; i < CF_METADATA_FIELD_COUNT; ++i) {
        const FieldSpec *spec = &k_fields[i];
        void *klass = api.class_from_name(game, spec->namespaze, spec->klass);
        void *field = klass != NULL ? api.class_get_field_from_name(klass, spec->field) : NULL;
        if (field == NULL) {
            snprintf(error, error_size, "missing field %s.%s", spec->klass, spec->field);
            goto done;
        }
        size_t offset = api.field_get_offset(field);
        if (!cf_metadata_instance_field_valid(offset, spec->width,
                api.class_instance_size(klass), api.field_get_flags(field),
                api.type_get_type(api.field_get_type(field)), spec->kind)) {
            snprintf(error, error_size, "unexpected layout/type %s.%s", spec->klass, spec->field);
            goto done;
        }
        layout->offsets[i] = offset;
    }
    void *manager_class = api.class_from_name(game, "WNEngine", "AttackableTargetsManager");
    void *singleton = api.class_get_field_from_name(manager_class, "_Instance");
    if (singleton == NULL || !(api.field_get_flags(singleton) & 0x10) ||
        (api.field_get_flags(singleton) & 0x40) ||
        api.type_get_type(api.field_get_type(singleton)) != 18) {
        snprintf(error, error_size, "manager singleton metadata is invalid");
        goto done;
    }
    api.field_static_get_value(singleton, &layout->manager);
    if (layout->manager != 0 &&
        (layout->manager > UINTPTR_MAX - layout->offsets[CF_TARGET_LIST] ||
         !cf_read_self(layout->manager + layout->offsets[CF_TARGET_LIST],
                       &layout->target_list, sizeof(layout->target_list)))) {
        snprintf(error, error_size, "manager list reference unreadable");
        goto done;
    }
    ok = true;
done:
    if (attached != NULL) api.thread_detach(attached);
    if (!ok) memset(layout, 0, sizeof(*layout));
    return ok;
}
