#define _GNU_SOURCE

#include "il2cpp_resolver.h"

#include <elf.h>
#include <link.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "xdl.h"

typedef struct {
  uintptr_t base;
  const ElfW(Phdr) *phdr;
  size_t phnum;
  const ElfW(Sym) *dynsym;
  const char *dynstr;
  const uint32_t *buckets;
  const uint32_t *chains;
  uint32_t bucket_count;
  uint32_t chain_count;
} ElfView;

typedef struct {
  uintptr_t array_element_size;
  uintptr_t class_from_il2cpp_type;
  uintptr_t class_get_methods;
  uintptr_t class_is_valuetype;
  uintptr_t domain_get;
  uintptr_t raise_exception;
  uintptr_t method_get_object;
  uintptr_t string_new;
  uintptr_t type_get_object;
} ApiAnchors;

#define AIM_ROTATION_SCAN_SIZE 0x1000u
#define AIM_ROTATE_CALL_PREV_WORD 0xaa1f03e1u
#define AIM_ROTATE_CALL_NEXT_WORD 0x14000089u

static void set_error(char *error, size_t error_size, const char *format, ...) {
  if (error == NULL || error_size == 0) {
    return;
  }
  va_list args;
  va_start(args, format);
  vsnprintf(error, error_size, format, args);
  va_end(args);
}

static uint32_t elf_sysv_hash(const char *name) {
  uint32_t hash = 0;
  uint32_t high = 0;
  while (*name != '\0') {
    hash = (hash << 4) + (uint8_t)*name++;
    high = hash & 0xf0000000u;
    if (high != 0) {
      hash ^= high >> 24;
    }
    hash &= ~high;
  }
  return hash;
}

static bool rva_in_segment(const ElfView *view, uintptr_t rva,
                           uint32_t required_flags) {
  for (size_t i = 0; i < view->phnum; ++i) {
    const ElfW(Phdr) *segment = &view->phdr[i];
    if (segment->p_type != PT_LOAD ||
        (segment->p_flags & required_flags) != required_flags) {
      continue;
    }
    uintptr_t begin = (uintptr_t)segment->p_vaddr;
    uintptr_t end = begin + (uintptr_t)segment->p_memsz;
    if (rva >= begin && rva < end) {
      return true;
    }
  }
  return false;
}

static bool address_is_executable(const ElfView *view, uintptr_t address) {
  if (address < view->base) {
    return false;
  }
  return rva_in_segment(view, address - view->base, PF_X);
}

static bool load_elf_view(void *handle, ElfView *view, char *error,
                          size_t error_size) {
  xdl_info_t info = {0};
  if (xdl_info(handle, XDL_DI_DLINFO, &info) != 0 || info.dli_fbase == NULL ||
      info.dlpi_phdr == NULL || info.dlpi_phnum == 0) {
    set_error(error, error_size, "xdl_info failed");
    return false;
  }

  memset(view, 0, sizeof(*view));
  view->base = (uintptr_t)info.dli_fbase;
  view->phdr = info.dlpi_phdr;
  view->phnum = info.dlpi_phnum;

  const ElfW(Dyn) *dynamic = NULL;
  for (size_t i = 0; i < view->phnum; ++i) {
    if (view->phdr[i].p_type == PT_DYNAMIC) {
      dynamic = (const ElfW(Dyn) *)(view->base + view->phdr[i].p_vaddr);
      break;
    }
  }
  if (dynamic == NULL) {
    set_error(error, error_size, "libil2cpp has no PT_DYNAMIC");
    return false;
  }

  const uint32_t *sysv_hash = NULL;
  for (const ElfW(Dyn) *entry = dynamic; entry->d_tag != DT_NULL; ++entry) {
    switch (entry->d_tag) {
      case DT_SYMTAB:
        view->dynsym =
            (const ElfW(Sym) *)(view->base + (uintptr_t)entry->d_un.d_ptr);
        break;
      case DT_STRTAB:
        view->dynstr =
            (const char *)(view->base + (uintptr_t)entry->d_un.d_ptr);
        break;
      case DT_HASH:
        sysv_hash =
            (const uint32_t *)(view->base + (uintptr_t)entry->d_un.d_ptr);
        break;
      default:
        break;
    }
  }

  if (view->dynsym == NULL || view->dynstr == NULL || sysv_hash == NULL) {
    set_error(error, error_size, "missing DT_SYMTAB/DT_STRTAB/DT_HASH");
    return false;
  }

  view->bucket_count = sysv_hash[0];
  view->chain_count = sysv_hash[1];
  if (view->bucket_count == 0 || view->chain_count == 0 ||
      view->bucket_count > (1u << 20) || view->chain_count > (1u << 20)) {
    set_error(error, error_size, "invalid SysV hash dimensions %u/%u",
              view->bucket_count, view->chain_count);
    return false;
  }
  view->buckets = &sysv_hash[2];
  view->chains = &view->buckets[view->bucket_count];
  return true;
}

// CrossFire's protected entries store bswap64(real_rva << 8 | 0x80) in
// st_value. Depending on the loader/build, st_name is either cleared or points
// at an unrelated decoy string, so it is not used as an authenticity check.
// This is not treated as a general IL2CPP format: a candidate is accepted only
// together with the intact SysV name hash, FUNC metadata, an executable
// address, the API-group bounds supplied by the caller, and uniqueness.
static bool decode_protected_rva(const ElfView *view, const ElfW(Sym) *symbol,
                                 uintptr_t *rva) {
  if (symbol->st_shndx == SHN_UNDEF ||
      ELF64_ST_TYPE(symbol->st_info) != STT_FUNC) {
    return false;
  }

  uint64_t decoded = __builtin_bswap64((uint64_t)symbol->st_value);
  if ((decoded & 0xffu) != 0x80u) {
    return false;
  }
  uintptr_t candidate = (uintptr_t)(decoded >> 8);
  if ((candidate & 3u) != 0 || !rva_in_segment(view, candidate, PF_X)) {
    return false;
  }
  *rva = candidate;
  return true;
}

static void *resolve_protected_symbol(const ElfView *view, const char *name,
                                      uintptr_t lower, uintptr_t upper,
                                      char *error, size_t error_size) {
  uint32_t bucket = elf_sysv_hash(name) % view->bucket_count;
  uint32_t index = view->buckets[bucket];
  uintptr_t match = 0;
  unsigned matches = 0;
  unsigned traversed = 0;

  while (index != STN_UNDEF && index < view->chain_count &&
         traversed++ < view->chain_count) {
    const ElfW(Sym) *symbol = &view->dynsym[index];
    uintptr_t rva = 0;
    if (decode_protected_rva(view, symbol, &rva)) {
      uintptr_t address = view->base + rva;
      if (address > lower && address < upper) {
        match = address;
        ++matches;
      }
    }
    index = view->chains[index];
  }

  if (matches != 1) {
    set_error(error, error_size,
              "%s protected bucket produced %u in-range candidates", name,
              matches);
    return NULL;
  }
  return (void *)match;
}

static void *resolve_anchor(void *handle, const ElfView *view,
                            const char *name, char *error,
                            size_t error_size) {
  void *result = xdl_sym(handle, name, NULL);
  if (result == NULL || !address_is_executable(view, (uintptr_t)result)) {
    set_error(error, error_size, "missing executable anchor %s", name);
    return NULL;
  }
  return result;
}

static void *resolve_api(void *handle, const ElfView *view, const char *name,
                         uintptr_t lower, uintptr_t upper,
                         unsigned *protected_symbols, char *error,
                         size_t error_size) {
  void *result = xdl_sym(handle, name, NULL);
  if (result != NULL) {
    if (!address_is_executable(view, (uintptr_t)result)) {
      set_error(error, error_size, "%s export is outside executable mapping",
                name);
      return NULL;
    }
    return result;
  }

  result =
      resolve_protected_symbol(view, name, lower, upper, error, error_size);
  if (result != NULL) {
    ++*protected_symbols;
  }
  return result;
}

bool cf_il2cpp_resolve(CfIl2CppApi *api, char *error, size_t error_size) {
  if (api == NULL) {
    set_error(error, error_size, "api is null");
    return false;
  }
  memset(api, 0, sizeof(*api));
  if (error != NULL && error_size != 0) {
    error[0] = '\0';
  }

  void *handle = xdl_open("libil2cpp.so", XDL_DEFAULT);
  if (handle == NULL) {
    set_error(error, error_size, "libil2cpp.so is not loaded");
    return false;
  }

  ElfView view = {0};
  if (!load_elf_view(handle, &view, error, error_size)) {
    xdl_close(handle);
    return false;
  }

  ApiAnchors anchors = {0};
#define GET_ANCHOR(member, symbol_name)                                      \
  do {                                                                        \
    anchors.member = (uintptr_t)resolve_anchor(handle, &view, symbol_name,    \
                                                error, error_size);            \
    if (anchors.member == 0) {                                                 \
      xdl_close(handle);                                                       \
      return false;                                                            \
    }                                                                          \
  } while (0)

  GET_ANCHOR(array_element_size, "il2cpp_array_element_size");
  GET_ANCHOR(class_from_il2cpp_type, "il2cpp_class_from_il2cpp_type");
  GET_ANCHOR(class_get_methods, "il2cpp_class_get_methods");
  GET_ANCHOR(class_is_valuetype, "il2cpp_class_is_valuetype");
  GET_ANCHOR(domain_get, "il2cpp_domain_get");
  GET_ANCHOR(raise_exception, "il2cpp_raise_exception");
  GET_ANCHOR(method_get_object, "il2cpp_method_get_object");
  GET_ANCHOR(string_new, "il2cpp_string_new");
  GET_ANCHOR(type_get_object, "il2cpp_type_get_object");
#undef GET_ANCHOR

  // The named anchors also fingerprint the API table layout. Refuse unknown
  // layouts rather than widening ranges and risking a call to the wrong stub.
  if (!(anchors.array_element_size < anchors.class_from_il2cpp_type &&
        anchors.class_from_il2cpp_type < anchors.class_get_methods &&
        anchors.class_get_methods < anchors.class_is_valuetype &&
        anchors.class_is_valuetype < anchors.domain_get &&
        anchors.domain_get < anchors.raise_exception &&
        anchors.raise_exception < anchors.method_get_object &&
        anchors.method_get_object < anchors.string_new &&
        anchors.string_new < anchors.type_get_object)) {
    set_error(error, error_size, "unrecognized IL2CPP API anchor order");
    xdl_close(handle);
    return false;
  }

  api->base = view.base;
  for (size_t i = 0; i < view.phnum; ++i) {
    const ElfW(Phdr) *segment = &view.phdr[i];
    if (segment->p_type != PT_LOAD || (segment->p_flags & PF_X) == 0) {
      continue;
    }
    uintptr_t begin = view.base + (uintptr_t)segment->p_vaddr;
    uintptr_t end = begin + (uintptr_t)segment->p_memsz;
    if (api->executable_begin == 0 || begin < api->executable_begin) {
      api->executable_begin = begin;
    }
    if (end > api->executable_end) {
      api->executable_end = end;
    }
  }
  api->domain_get = (void *(*)(void))anchors.domain_get;

#define GET_API(member, type, symbol_name, lower, upper)                      \
  do {                                                                        \
    api->member = (type)resolve_api(handle, &view, symbol_name, lower, upper, \
                                     &api->protected_symbols, error,           \
                                     error_size);                              \
    if (api->member == NULL) {                                                 \
      xdl_close(handle);                                                       \
      memset(api, 0, sizeof(*api));                                            \
      return false;                                                            \
    }                                                                          \
  } while (0)

  GET_API(assembly_get_image, const void *(*)(const void *),
          "il2cpp_assembly_get_image", anchors.array_element_size,
          anchors.class_from_il2cpp_type);
  GET_API(class_from_name,
          void *(*)(const void *, const char *, const char *),
          "il2cpp_class_from_name", anchors.class_from_il2cpp_type,
          anchors.class_get_methods);
  GET_API(class_get_field_from_name, void *(*)(void *, const char *),
          "il2cpp_class_get_field_from_name",
          anchors.class_from_il2cpp_type, anchors.class_get_methods);
  GET_API(class_get_method_from_name,
          const void *(*)(void *, const char *, int),
          "il2cpp_class_get_method_from_name", anchors.class_get_methods,
          anchors.class_is_valuetype);
  GET_API(domain_get_assemblies,
          const void **(*)(const void *, size_t *),
          "il2cpp_domain_get_assemblies", anchors.domain_get,
          anchors.raise_exception);
  GET_API(field_get_offset, size_t(*)(void *), "il2cpp_field_get_offset",
          anchors.raise_exception, anchors.method_get_object);
  GET_API(field_get_value, void (*)(void *, void *, void *),
          "il2cpp_field_get_value", anchors.raise_exception,
          anchors.method_get_object);
  GET_API(thread_attach, void *(*)(void *), "il2cpp_thread_attach",
          anchors.string_new, anchors.type_get_object);
  GET_API(thread_detach, void (*)(void *), "il2cpp_thread_detach",
          anchors.string_new, anchors.type_get_object);
  GET_API(image_get_name, const char *(*)(const void *),
          "il2cpp_image_get_name", anchors.type_get_object,
          anchors.type_get_object + 0x1000u);
#undef GET_API

  xdl_close(handle);
  return true;
}

void *cf_il2cpp_method_pointer(const CfIl2CppApi *api,
                               const void *method_info) {
  if (api == NULL || method_info == NULL) {
    return NULL;
  }

  uintptr_t pointer = 0;
  memcpy(&pointer, method_info, sizeof(pointer));
  if ((pointer & 3u) != 0 || pointer < api->executable_begin ||
      pointer >= api->executable_end) {
    return NULL;
  }
  return (void *)pointer;
}

static const void *find_image(const CfIl2CppApi *api, const char *wanted,
                              char *error, size_t error_size) {
  void *domain = api->domain_get();
  if (domain == NULL) {
    set_error(error, error_size, "il2cpp_domain_get returned null");
    return NULL;
  }

  size_t count = 0;
  const void **assemblies = api->domain_get_assemblies(domain, &count);
  if (assemblies == NULL || count == 0 || count > 1024) {
    set_error(error, error_size, "invalid assembly list %p/%zu", assemblies,
              count);
    return NULL;
  }

  for (size_t i = 0; i < count; ++i) {
    const void *image = api->assembly_get_image(assemblies[i]);
    if (image == NULL) {
      continue;
    }
    const char *name = api->image_get_name(image);
    if (name != NULL && strcmp(name, wanted) == 0) {
      return image;
    }
  }
  set_error(error, error_size, "image not found: %s", wanted);
  return NULL;
}

static void *find_class(const CfIl2CppApi *api, const void *image,
                        const char *namespaze, const char *class_name,
                        char *error, size_t error_size) {
  void *klass = api->class_from_name(image, namespaze, class_name);
  if (klass == NULL) {
    set_error(error, error_size, "class not found: %s.%s", namespaze,
              class_name);
  }
  return klass;
}

static bool find_field_offset(const CfIl2CppApi *api, void *klass,
                              const char *class_name, const char *field_name,
                              size_t *offset, char *error,
                              size_t error_size) {
  void *field = api->class_get_field_from_name(klass, field_name);
  if (field == NULL) {
    set_error(error, error_size, "field not found: %s::%s", class_name,
              field_name);
    return false;
  }
  *offset = api->field_get_offset(field);
  return true;
}

static void *find_method_pointer(const CfIl2CppApi *api, void *klass,
                                 const char *class_name,
                                 const char *method_name, int args_count,
                                 char *error, size_t error_size) {
  const void *method =
      api->class_get_method_from_name(klass, method_name, args_count);
  if (method == NULL) {
    set_error(error, error_size, "method not found: %s::%s/%d", class_name,
              method_name, args_count);
    return NULL;
  }
  void *pointer = cf_il2cpp_method_pointer(api, method);
  if (pointer == NULL) {
    set_error(error, error_size, "invalid MethodInfo pointer: %s::%s/%d",
              class_name, method_name, args_count);
  }
  return pointer;
}

static bool decode_arm64_bl_target(uintptr_t instruction_address,
                                   uint32_t instruction,
                                   uintptr_t *target) {
  if ((instruction & 0xfc000000u) != 0x94000000u || target == NULL) {
    return false;
  }
  int64_t immediate = (int64_t)(instruction & 0x03ffffffu);
  if ((immediate & 0x02000000) != 0) {
    immediate -= 0x04000000;
  }
  *target = (uintptr_t)((int64_t)instruction_address + immediate * 4);
  return true;
}

bool cf_il2cpp_find_aim_rotate_callsite(const CfIl2CppApi *api,
                                       const CfGameTargets *targets,
                                       bool allow_patched, void **callsite,
                                       bool *already_patched, char *error,
                                       size_t error_size) {
  if (api == NULL || targets == NULL || callsite == NULL ||
      targets->get_aim_assistance_target_rotation == NULL ||
      targets->quaternion_rotate_towards == NULL) {
    set_error(error, error_size, "invalid aim callsite arguments");
    return false;
  }
  *callsite = NULL;
  if (already_patched != NULL) {
    *already_patched = false;
  }

  uintptr_t rotation_method =
      (uintptr_t)targets->get_aim_assistance_target_rotation;
  uintptr_t rotate_towards =
      (uintptr_t)targets->quaternion_rotate_towards;
  uintptr_t scan_end = rotation_method + AIM_ROTATION_SCAN_SIZE;
  if (scan_end < rotation_method || scan_end > api->executable_end) {
    scan_end = api->executable_end;
  }

  uintptr_t original_match = 0;
  uintptr_t patched_match = 0;
  unsigned original_matches = 0;
  unsigned patched_matches = 0;
  for (uintptr_t address = rotation_method + sizeof(uint32_t);
       address + sizeof(uint32_t) < scan_end;
       address += sizeof(uint32_t)) {
    uint32_t words[3] = {0};
    memcpy(words, (const void *)(address - sizeof(uint32_t)), sizeof(words));
    if (words[0] != AIM_ROTATE_CALL_PREV_WORD ||
        words[2] != AIM_ROTATE_CALL_NEXT_WORD) {
      continue;
    }
    uintptr_t branch_target = 0;
    if (decode_arm64_bl_target(address, words[1], &branch_target) &&
        branch_target == rotate_towards) {
      original_match = address;
      ++original_matches;
    } else if ((words[1] & 0xfc000000u) == 0x14000000u) {
      patched_match = address;
      ++patched_matches;
    }
  }

  if (original_matches == 1 && patched_matches == 0) {
    *callsite = (void *)original_match;
    return true;
  }
  if (allow_patched && original_matches == 0 && patched_matches == 1) {
    *callsite = (void *)patched_match;
    if (already_patched != NULL) {
      *already_patched = true;
    }
    return true;
  }

  if (!allow_patched && original_matches == 0 && patched_matches == 1) {
    set_error(error, error_size, "aim RotateTowards callsite is already patched");
  } else {
    set_error(error, error_size,
              "aim RotateTowards callsite produced original=%u patched=%u",
              original_matches, patched_matches);
  }
  return false;
}

bool cf_il2cpp_find_aim_rotate_callsite_runtime(
    const CfIl2CppApi *api, const CfGameTargets *targets, void **callsite,
    bool *used_fallback, char *error, size_t error_size) {
  if (used_fallback != NULL) {
    *used_fallback = false;
  }
  bool already_patched = false;
  if (cf_il2cpp_find_aim_rotate_callsite(
          api, targets, false, callsite, &already_patched, error,
          error_size)) {
    return true;
  }
  if (api == NULL || targets == NULL || callsite == NULL ||
      targets->get_aim_assistance_target_rotation == NULL ||
      targets->quaternion_rotate_towards == NULL) {
    set_error(error, error_size, "invalid runtime callsite arguments");
    return false;
  }

  uintptr_t rotation_method =
      (uintptr_t)targets->get_aim_assistance_target_rotation;
  uintptr_t rotate_towards =
      (uintptr_t)targets->quaternion_rotate_towards;
  uintptr_t scan_end = rotation_method + AIM_ROTATION_SCAN_SIZE;
  if (scan_end < rotation_method || scan_end > api->executable_end) {
    scan_end = api->executable_end;
  }

  uintptr_t match = 0;
  unsigned matches = 0;
  for (uintptr_t address = rotation_method;
       address + sizeof(uint32_t) <= scan_end;
       address += sizeof(uint32_t)) {
    uint32_t instruction = 0;
    memcpy(&instruction, (const void *)address, sizeof(instruction));
    uintptr_t branch_target = 0;
    if (decode_arm64_bl_target(address, instruction, &branch_target) &&
        branch_target == rotate_towards) {
      match = address;
      ++matches;
    }
  }

  if (matches != 1) {
    set_error(error, error_size,
              "runtime RotateTowards scan produced %u direct calls", matches);
    return false;
  }
  *callsite = (void *)match;
  if (used_fallback != NULL) {
    *used_fallback = true;
  }
  return true;
}

bool cf_il2cpp_resolve_aim_speed_targets(const CfIl2CppApi *api,
                                         CfGameTargets *targets, char *error,
                                         size_t error_size) {
  if (api == NULL || targets == NULL || api->base == 0) {
    set_error(error, error_size, "invalid aim/speed resolver arguments");
    return false;
  }
  memset(targets, 0, sizeof(*targets));

  const void *game = find_image(api, "Assembly-CSharp.dll", error, error_size);
  const void *unity = find_image(api, "UnityEngine.dll", error, error_size);
  if (game == NULL || unity == NULL) {
    return false;
  }

  void *attackable =
      find_class(api, game, "WNEngine", "AttackableTarget", error, error_size);
  void *controller =
      find_class(api, game, "WNEngine", "PlayerController", error, error_size);
  void *quaternion =
      find_class(api, unity, "UnityEngine", "Quaternion", error, error_size);
  void *time =
      find_class(api, unity, "UnityEngine", "Time", error, error_size);
  if (attackable == NULL || controller == NULL || quaternion == NULL ||
      time == NULL) {
    memset(targets, 0, sizeof(*targets));
    return false;
  }

  targets->aim_assist_offset =
      find_method_pointer(api, attackable, "AttackableTarget",
                          "get_AimAssistOffset", 0, error, error_size);
  targets->quaternion_rotate_towards =
      find_method_pointer(api, quaternion, "Quaternion", "RotateTowards", 3,
                          error, error_size);
  targets->time_get_delta_time =
      find_method_pointer(api, time, "Time", "get_deltaTime", 0, error,
                          error_size);
  targets->get_aim_assistance_target_rotation = find_method_pointer(
      api, controller, "PlayerController",
      "GetAimAssistanceTargetRotation", 1, error, error_size);

  if (targets->aim_assist_offset == NULL ||
      targets->quaternion_rotate_towards == NULL ||
      targets->time_get_delta_time == NULL ||
      targets->get_aim_assistance_target_rotation == NULL) {
    memset(targets, 0, sizeof(*targets));
    return false;
  }
  return true;
}

bool cf_il2cpp_resolve_game_targets(const CfIl2CppApi *api,
                                    CfGameTargets *targets, char *error,
                                    size_t error_size) {
  if (api == NULL || targets == NULL || api->base == 0) {
    set_error(error, error_size, "invalid resolver arguments");
    return false;
  }
  memset(targets, 0, sizeof(*targets));

  bool ok = false;
  const void *game = find_image(api, "Assembly-CSharp.dll", error, error_size);
  const void *unity = find_image(api, "UnityEngine.dll", error, error_size);
  if (game == NULL || unity == NULL) {
    goto cleanup;
  }

  void *attackable =
      find_class(api, game, "WNEngine", "AttackableTarget", error, error_size);
  void *controller =
      find_class(api, game, "WNEngine", "PlayerController", error, error_size);
  void *pawn = find_class(api, game, "WNEngine", "Pawn", error, error_size);
  void *quaternion =
      find_class(api, unity, "UnityEngine", "Quaternion", error, error_size);
  void *time = find_class(api, unity, "UnityEngine", "Time", error,
                          error_size);
  if (attackable == NULL || controller == NULL || pawn == NULL ||
      quaternion == NULL || time == NULL) {
    goto cleanup;
  }

  targets->aim_assist_offset =
      find_method_pointer(api, attackable, "AttackableTarget",
                          "get_AimAssistOffset", 0, error, error_size);
  targets->quaternion_rotate_towards =
      find_method_pointer(api, quaternion, "Quaternion", "RotateTowards", 3,
                          error, error_size);
  targets->time_get_delta_time = find_method_pointer(
      api, time, "Time", "get_deltaTime", 0, error, error_size);
  targets->get_aim_assistance_target_rotation = find_method_pointer(
      api, controller, "PlayerController",
      "GetAimAssistanceTargetRotation", 1, error, error_size);
  if (targets->aim_assist_offset == NULL ||
      targets->quaternion_rotate_towards == NULL ||
      targets->time_get_delta_time == NULL ||
      targets->get_aim_assistance_target_rotation == NULL) {
    goto cleanup;
  }

  if (!find_field_offset(api, controller, "PlayerController",
                         "m_CurrentAimAssistTarget",
                         &targets->controller_current_target_offset, error,
                         error_size) ||
      !find_field_offset(api, pawn, "Pawn", "m_LastSimulateVelocity",
                         &targets->pawn_last_simulate_velocity_offset, error,
                         error_size)) {
    goto cleanup;
  }

  ok = true;

cleanup:
  if (!ok) {
    memset(targets, 0, sizeof(*targets));
  }
  return ok;
}
