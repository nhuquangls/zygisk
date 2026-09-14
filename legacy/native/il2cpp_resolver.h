#ifndef CF_IL2CPP_RESOLVER_H
#define CF_IL2CPP_RESOLVER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct CfIl2CppApi {
  uintptr_t base;
  uintptr_t executable_begin;
  uintptr_t executable_end;
  unsigned protected_symbols;

  void *(*domain_get)(void);
  const void **(*domain_get_assemblies)(const void *domain, size_t *size);
  const void *(*assembly_get_image)(const void *assembly);
  const char *(*image_get_name)(const void *image);
  void *(*class_from_name)(const void *image, const char *namespaze,
                           const char *name);
  void *(*class_get_field_from_name)(void *klass, const char *name);
  size_t (*field_get_offset)(void *field);
  void (*field_get_value)(void *object, void *field, void *value);
  const void *(*class_get_method_from_name)(void *klass, const char *name,
                                             int args_count);
  void *(*thread_attach)(void *domain);
  void (*thread_detach)(void *thread);
} CfIl2CppApi;

typedef struct CfGameTargets {
  void *aim_assist_offset;
  void *quaternion_rotate_towards;
  void *time_get_delta_time;
  void *get_aim_assistance_target_rotation;
  size_t controller_current_target_offset;
  size_t pawn_last_simulate_velocity_offset;
} CfGameTargets;

// Resolves the exported IL2CPP API with xDL first. For the protected CrossFire
// build, missing API symbols are recovered from the still-intact SysV ELF hash
// table and the encoded anonymous .dynsym entries. The fallback is fail-closed:
// exactly one executable candidate must exist in the expected API group.
bool cf_il2cpp_resolve(CfIl2CppApi *api, char *error, size_t error_size);

// Resolves CrossFire classes, methods and fields by assembly/namespace/name.
// These metadata-only APIs do not require il2cpp_thread_attach on this build;
// avoiding it is important during startup because IL2CPP aborts when its
// thread subsystem is not initialized yet. No live object is accessed and no
// code is modified.
bool cf_il2cpp_resolve_game_targets(const CfIl2CppApi *api,
                                    CfGameTargets *targets, char *error,
                                    size_t error_size);

// Resolves only the methods required by the fixed aim-offset and adaptive
// speed hooks. It deliberately does not depend on target/velocity fields used
// by the optional latency predictor.
bool cf_il2cpp_resolve_aim_speed_targets(const CfIl2CppApi *api,
                                         CfGameTargets *targets, char *error,
                                         size_t error_size);

// Finds the unique guarded callsite inside
// PlayerController.GetAimAssistanceTargetRotation. Production hook setup must
// pass allow_patched=false. Observe-only probes may pass true to identify a
// callsite that an existing instrumentation hook changed from BL to B.
bool cf_il2cpp_find_aim_rotate_callsite(const CfIl2CppApi *api,
                                       const CfGameTargets *targets,
                                       bool allow_patched, void **callsite,
                                       bool *already_patched, char *error,
                                       size_t error_size);

// Update-tolerant variant: first uses the guarded instruction signature, then
// falls back to accepting exactly one direct BL from the resolved aim method
// to the resolved Quaternion.RotateTowards method.
bool cf_il2cpp_find_aim_rotate_callsite_runtime(
    const CfIl2CppApi *api, const CfGameTargets *targets, void **callsite,
    bool *used_fallback, char *error, size_t error_size);

// Returns the native method pointer stored at the beginning of MethodInfo for
// this Unity build. The pointer is only accepted if it belongs to libil2cpp's
// executable PT_LOAD mapping.
void *cf_il2cpp_method_pointer(const CfIl2CppApi *api,
                               const void *method_info);

#ifdef __cplusplus
}
#endif

#endif
