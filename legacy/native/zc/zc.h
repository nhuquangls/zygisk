#ifndef ZC_ZERO_CODE_H
#define ZC_ZERO_CODE_H

// Zero-Code Modification cheat engine for CrossFire Legends.
// Mimics the proven mod goc (libTuanMeta.so) architecture:
//   - NO byte patched in libanort / libanogs / libil2cpp.
//   - Only hooks /system/lib64/libEGL.so (eglSwapBuffers) and
//     /system/lib64/libandroid.so (input/screen size) with Dobby.
//   - Reads entity positions read-only from Unity heap (pointer chasing).
//   - Renders an ImGui overlay on the game's existing EGL context.
//   - Aimbot by injecting fake touch deltas (never writes game code).

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Thread-safe, immutable-after-init globals for the render/aim paths.
typedef struct ZcVec2 {
  float x, y;
} ZcVec2;

typedef struct ZcVec3 {
  float x, y, z;
} ZcVec3;

// A single tracked entity produced by the heap reader.
typedef struct ZcEntity {
  uintptr_t player_ptr;
  ZcVec3 head;     // world-space position (head)
  ZcVec3 feet;     // world-space position (feet)
  bool is_local;
  bool valid;
  int team;
} ZcEntity;

typedef struct ZcWorldToScreenResult {
  ZcVec2 screen;
  bool visible;
} ZcWorldToScreenResult;

// --- Render (eglSwapBuffers) ---
// Installs the Dobby hook on eglSwapBuffers once libEGL.so is loaded.
// Safe to call multiple times; only the first call installs the hook.
void zc_egl_init(void);

// --- Input / screen size (libandroid.so) ---
// Installs the Dobby hook on ANativeWindow_getHeight once libandroid.so is
// loaded. Returns the display dimensions consumed by W2S.
void zc_android_init(void);

// --- Heap reader ---
// Resolves the single BattleManager base pointer (from a static IL2CPP field)
// once the game is fully loaded. Returns false until the class is available.
bool zc_resolve_battle_manager(void);

// Produces the current list of valid enemy entities (0 if none / not in match).
size_t zc_read_entities(ZcEntity *out, size_t capacity);

// --- Math ---
ZcWorldToScreenResult zc_world_to_screen(const ZcVec3 *world);
ZcVec2 zc_screen_center(void);

// --- Aimbot ---
// Given the best target's screen position, computes a smooth fake-touch
// delta from screen center and injects it (libandroid.so path).
void zc_aim_at(const ZcVec2 *target_screen);

// --- Entry ---
// Called from the Zygisk payload after the game is attached. Starts worker
// threads and installs hooks. Returns 0 on success.
int zc_start(void);

#ifdef __cplusplus
}
#endif

#endif // ZC_ZERO_CODE_H
