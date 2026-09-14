// Always-on ARM64 patches, restricted to the two supported branch sites.
#define _GNU_SOURCE
#include <jni.h>
#include <android/log.h>
#include <math.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>
#include "arm64_context.h"
#include "il2cpp_resolver.h"
#include "libanort_patch.h"
#include "maps_spoof.h"
#include "memory_patch.h"
#include "shadow_memory.h"

#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, "aimhook", __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, "aimhook", __VA_ARGS__)
#define START_DELAY_SECONDS 20
typedef struct { float x, y, z; } Vector3;
typedef float (*GetDeltaTime)(void *unused, const void *method);
static GetDeltaTime g_get_delta_time;
static uint32_t g_adaptive_hits;
static MemoryPatch g_patches[2];
static int g_worker_started;
// Initialized before any branch can enter the bridge; immutable afterwards.
uintptr_t cf_original_rotate;
extern void cf_rotate_bridge(void);

__attribute__((noinline)) static Vector3 hooked_aim_offset(void *self,
                                                         const void *method) {
  (void)self;
  (void)method;
  return (Vector3){0.0f, 0.125f, 0.0f};
}

static float clamp01(float value) {
  if (value < 0.0f) return 0.0f;
  if (value > 1.0f) return 1.0f;
  return value;
}

static bool normalize_quaternion(float q[4]) {
  float norm = q[0]*q[0] + q[1]*q[1] + q[2]*q[2] + q[3]*q[3];
  if (!isfinite(norm) || norm < 0.25f || norm > 2.25f) return false;
  float inverse = 1.0f / sqrtf(norm);
  for (size_t i = 0; i < 4; ++i) q[i] *= inverse;
  return true;
}

void cf_adaptive_apply(Arm64Context *ctx) {
  GetDeltaTime get_delta_time = __atomic_load_n(&g_get_delta_time, __ATOMIC_ACQUIRE);
  if (ctx == NULL || ctx->sp == 0 || get_delta_time == NULL) return;
  // Two Quaternion HFAs fill s0-s7; the float argument is at the original SP.
  float *slot = (float *)ctx->sp;
  float original = *slot;
  float dt = get_delta_time(NULL, NULL);
  if (!isfinite(original) || original <= 0.0f || original > 10.0f ||
      !isfinite(dt) || dt < 0.001f || dt > 0.25f) return;
  float from[4] = {ctx->q[0][0], ctx->q[1][0], ctx->q[2][0], ctx->q[3][0]};
  float to[4] = {ctx->q[4][0], ctx->q[5][0], ctx->q[6][0], ctx->q[7][0]};
  if (!normalize_quaternion(from) || !normalize_quaternion(to)) return;
  float dot = fabsf(from[0]*to[0] + from[1]*to[1] + from[2]*to[2] + from[3]*to[3]);
  if (dot > 1.0f) dot = 1.0f;
  float angle = 2.0f * acosf(dot) * 57.29577951308232f;
  float speed = original / dt;
  float speed_factor = 4.0f - 3.0f * clamp01((speed - 10.6f) / (18.4f - 10.6f));
  float angle_factor = 1.0f + 3.0f * clamp01((angle - 1.0f) / (5.0f - 1.0f));
  float factor = fminf(speed_factor, angle_factor);
  float modified = fminf(original * factor, angle);
  if (isfinite(modified) && modified > original) *slot = modified;
  uint32_t hit = __atomic_add_fetch(&g_adaptive_hits, 1u, __ATOMIC_RELAXED);
  if (hit == 1u || hit % 120u == 0u)
    LOGI("adaptive apply: base=%.2fdeg/s angle=%.2f factor=%.2f delta=%.4f->%.4f",
         (double)speed, (double)angle, (double)factor, (double)original, (double)*slot);
}

static bool executable_range(const CfIl2CppApi *api, uintptr_t addr, size_t size) {
  return addr >= api->executable_begin && addr < api->executable_end &&
         size <= api->executable_end - addr && (addr & 3u) == 0;
}

static bool install_patches(const CfIl2CppApi *api, const CfGameTargets *targets,
                            uintptr_t callsite) {
  uintptr_t aim = (uintptr_t)targets->aim_assist_offset;
  uintptr_t rotate = (uintptr_t)targets->quaternion_rotate_towards;
  uintptr_t delta = (uintptr_t)targets->time_get_delta_time;
  if (!executable_range(api, aim, 12) || !executable_range(api, callsite, 4) ||
      !executable_range(api, rotate, 4) || !executable_range(api, delta, 4) ||
      (callsite >= aim && callsite < aim + 12)) {
    LOGE("patch refused: invalid or overlapping code ranges");
    return false;
  }
  uint32_t words[3], call_word;
  memcpy(words, (const void *)aim, sizeof(words));
  memcpy(&call_word, (const void *)callsite, sizeof(call_word));
  uintptr_t tail, original_call;
  // Patch only the final B of the known getter that first zeros x0/x1.
  // Its initial instructions and the following function remain intact.
  if (words[0] != 0xaa1f03e0u || words[1] != 0xaa1f03e1u ||
      !cf_branch_decode(aim + 8, words[2], false, &tail) ||
      !executable_range(api, tail, 4) ||
      !cf_branch_decode(callsite, call_word, true, &original_call) ||
      original_call != rotate) {
    LOGE("patch refused: unsupported getter or changed RotateTowards call");
    return false;
  }
  char error[256] = {0};
  if (!cf_patch_prepare(&g_patches[0], aim + 8, words[2],
                         (uintptr_t)hooked_aim_offset, false, error, sizeof(error)) ||
      !cf_patch_prepare(&g_patches[1], callsite, call_word,
                         (uintptr_t)cf_rotate_bridge, true, error, sizeof(error))) {
    LOGE("patch preparation failed: %s", error);
    cf_patch_discard_unpublished(g_patches, 2);
    return false;
  }
  __atomic_store_n(&g_get_delta_time, (GetDeltaTime)delta, __ATOMIC_RELEASE);
  __atomic_store_n(&cf_original_rotate, rotate, __ATOMIC_RELEASE);

  /* ── Shadow Memory: save clean pages BEFORE commit writes ── */
  for (int i = 0; i < 2; ++i) {
    shadow_page_save(g_patches[i].page, g_patches[i].page_size);
  }

  if (!cf_patch_commit(g_patches, 2, error, sizeof(error))) {
    LOGE("patch installation failed: %s", error);
    cf_patch_discard_unpublished(g_patches, 2);
    // Keep callback data valid for any in-flight execution after rollback.
    return false;
  }

  /* ── Shadow Memory: strip PROT_READ AFTER commit writes ── */
  for (int i = 0; i < 2; ++i) {
    shadow_page_protect(g_patches[i].page, g_patches[i].page_size);
  }

  LOGI("always-on patches active: aim-tail=%p rotate-call=%p offsetY=0.125",
       (void *)(aim + 8), (void *)callsite);
  return true;
}

static void *hook_worker(void *unused) {
  (void)unused;
  LOGI("payload worker: waiting %ds before metadata resolution", START_DELAY_SECONDS);
  for (int i = 0; i < START_DELAY_SECONDS; ++i) sleep(1);
  CfIl2CppApi api = {0};
  CfGameTargets targets = {0};
  char error[256] = {0};
  bool ready = false;
  for (int attempt = 0; attempt < 600; ++attempt) {
    if (cf_il2cpp_resolve(&api, error, sizeof(error))) { ready = true; break; }
    usleep(50 * 1000);
  }
  if (!ready) { LOGE("IL2CPP API resolution failed: %s", error); return NULL; }

  /* ── Pre-emptive patching: disable libanort.so CRC dispatchers FIRST.
   * Must happen before we patch libil2cpp.so, otherwise libanort could run
   * a CRC scan between now and when we finish patching and see the changes. */
  cf_libanort_patch_dispatchers();

  ready = false;
  bool fallback = false;
  void *callsite = NULL;
  for (int attempt = 0; attempt < 1200; ++attempt) {
    memset(&targets, 0, sizeof(targets));
    if (cf_il2cpp_resolve_aim_speed_targets(&api, &targets, error, sizeof(error)) &&
        cf_il2cpp_find_aim_rotate_callsite_runtime(
            &api, &targets, &callsite, &fallback, error, sizeof(error))) {
      ready = true;
      break;
    }
    usleep(50 * 1000);
  }
  if (!ready) { LOGE("game target resolution failed: %s", error); return NULL; }
  LOGI("runtime targets: aim=%p rotate=%p callsite=%p scan=%s",
       targets.aim_assist_offset, targets.quaternion_rotate_towards, callsite,
       fallback ? "unique-bl" : "guarded");
  install_patches(&api, &targets, (uintptr_t)callsite);
  return NULL;
}

// Zygisk explicitly calls this after android_dlopen_ext has returned.
__attribute__((visibility("default"))) int cf_payload_start(void) {
  if (__atomic_exchange_n(&g_worker_started, 1, __ATOMIC_ACQ_REL) != 0) return 0;

  /* ── Shadow Memory engine: SIGSEGV handler for CRC-invisible patches ── */
  shadow_init();

  /* ── /proc/self/maps spoofing: hide module from memory scanners ── */
  maps_spoof_init();

  pthread_t thread;
  int error = pthread_create(&thread, NULL, hook_worker, NULL);
  if (error != 0) {
    __atomic_store_n(&g_worker_started, 0, __ATOMIC_RELEASE);
    LOGE("payload pthread_create failed: %d", error);
    return error;
  }
  pthread_detach(thread);
  return 0;
}

JNIEXPORT jint JNICALL JNI_OnLoad(JavaVM *vm, void *reserved) {
  (void)vm;
  (void)reserved;
  return cf_payload_start() == 0 ? JNI_VERSION_1_6 : JNI_ERR;
}
