#define _GNU_SOURCE
#include "readonly_runtime.h"
#include "readonly_memory.h"
#include "readonly_module.h"
#include "il2cpp_metadata.h"
#include "scene_snapshot.h"
#include "android_input.h"
#include "aim_policy.h"
#include "gyro_controller.h"
#include "sensor_probe.h"
#include <errno.h>
#include <jni.h>
#include <pthread.h>
#include <string.h>
#include <unistd.h>

#define EXPORT __attribute__((visibility("default")))
static int g_started, g_status;
static JavaVM *g_vm;
static int g_stop;

static void *readonly_worker(void *unused) {
    (void)unused;
    CfModule module;
    bool found = false;
    for (unsigned i = 0; i < 120; ++i) {
        if (__atomic_load_n(&g_stop, __ATOMIC_ACQUIRE)) {
            __atomic_store_n(&g_status, CF_STOPPED, __ATOMIC_RELEASE);
            return NULL;
        }
        if (cf_find_module("libil2cpp.so", &module)) {
            found = true;
            break;
        }
        sleep(1);
    }
    if (!found) {
        __atomic_store_n(&g_status, CF_LIBRARY_TIMEOUT, __ATOMIC_RELEASE);
        return NULL;
    }
    unsigned char header[4];
    if (!cf_read_self(module.image_begin, header, sizeof(header))) {
        __atomic_store_n(&g_status, CF_READ_UNAVAILABLE, __ATOMIC_RELEASE);
        return NULL;
    }
    char error[256];
    CfScene *scene = NULL;
    for (unsigned i = 0; i < 120; ++i) {
        if (__atomic_load_n(&g_stop, __ATOMIC_ACQUIRE)) {
            __atomic_store_n(&g_status, CF_STOPPED, __ATOMIC_RELEASE);
            return NULL;
        }
        error[0] = 0;
        scene = cf_scene_open(&module, error, sizeof(error));
        if (scene) break;
        if (i + 1 < 120) sleep(1);
    }
    if (!scene) {
        __atomic_store_n(&g_status, CF_METADATA_UNAVAILABLE, __ATOMIC_RELEASE);
        return NULL;
    }
    CfModule unity;
    bool sensor_probe = false;
    if (cf_find_module("libunity.so", &unity)) {
        error[0] = 0;
        sensor_probe = cf_sensor_probe_install(&unity, error, sizeof(error));
    } else {
        static const char unavailable[] = "libunity.so unavailable";
        memcpy(error, unavailable, sizeof(unavailable));
    }

    /* Gyro takeover must not share a run with synthetic touch actuation. */
    bool input = false;
    cf_input_enable(false);
    CfSceneSnapshot snapshot = {0};
    const CfViewport viewport = {2944.0f, 1840.0f};
    CfGyroController controller;
    cf_gyro_controller_reset(&controller);
    unsigned missed_scene_reads = 0;
    while (!__atomic_load_n(&g_stop, __ATOMIC_ACQUIRE)) {
        bool complete = false;
        bool scene_ok = cf_scene_poll(scene, &snapshot, &complete);
        if (scene_ok) {
            bool tracking = controller.locked_target && !controller.settled;
            bool needs_targets = cf_scene_needs_targets(
                &snapshot, tracking, controller.trigger_armed,
                controller.locked_target);
            if (needs_targets && !complete)
                scene_ok = cf_scene_read_target(scene, &snapshot,
                                                controller.locked_target);
        }
        if (scene_ok) {
            missed_scene_reads = 0;
            CfGyroCommand command;
            cf_gyro_controller_step(&controller, &snapshot, &viewport,
                                    snapshot.time_ns, &command);
            cf_sensor_probe_set_adjustment(sensor_probe && command.active,
                                           command.sensor_x, command.sensor_y);
            __atomic_store_n(&g_status, input ? cf_input_status() : CF_INPUT_UNAVAILABLE, __ATOMIC_RELEASE);
        } else {
            /*
             * Transform data can race Unity for an isolated pass. Preserve the
             * last validated correction for at most 64 ms instead of pulsing
             * the filter off between good snapshots. The sensor consumer's
             * 100 ms expiry remains the final fail-closed bound.
             */
            if (missed_scene_reads < 8) ++missed_scene_reads;
            if (missed_scene_reads >= 8)
                cf_sensor_probe_set_adjustment(false, 0, 0);
            __atomic_store_n(&g_status, CF_WAITING_SCENE, __ATOMIC_RELEASE);
        }
        usleep(cf_scene_poll_interval_us(&snapshot));
    }
    cf_sensor_probe_shutdown();
    cf_input_shutdown();
    cf_input_snapshot(NULL);
    cf_scene_close(scene);
    __atomic_store_n(&g_status, CF_STOPPED, __ATOMIC_RELEASE);
    return NULL;
}

EXPORT int cf_payload_status(void) {
    return __atomic_load_n(&g_status, __ATOMIC_ACQUIRE);
}

EXPORT int cf_payload_start(void) {
    if (__atomic_exchange_n(&g_started, 1, __ATOMIC_ACQ_REL) != 0) return 0;
    __atomic_store_n(&g_status, CF_WAITING_LIBRARY, __ATOMIC_RELEASE);
    pthread_t thread;
    int error = pthread_create(&thread, NULL, readonly_worker, NULL);
    if (error != 0) {
        __atomic_store_n(&g_status, CF_STOPPED, __ATOMIC_RELEASE);
        __atomic_store_n(&g_started, 0, __ATOMIC_RELEASE);
        return error;
    }
    (void)pthread_detach(thread);
    return 0;
}

EXPORT int cf_payload_start_vm(JavaVM *vm) {
    if (!vm) return EINVAL;
    // Called once by the target process's Zygisk loader, before worker creation.
    if (__atomic_load_n(&g_started, __ATOMIC_ACQUIRE)) return EALREADY;
    g_vm = vm;
    return cf_payload_start();
}

// Stop submitting swipes; an already submitted 120ms swipe finishes with UP.
// The worker then exits. Keep the library mapped because
// registered Java natives and the posted bridge callback can still reference it.
EXPORT void cf_payload_stop(void) {
    cf_input_shutdown();
    __atomic_store_n(&g_stop, 1, __ATOMIC_RELEASE);
}

JNIEXPORT jint JNICALL JNI_OnLoad(JavaVM *vm, void *reserved) {
    (void)reserved;
    return cf_payload_start_vm(vm) == 0 ? JNI_VERSION_1_6 : JNI_ERR;
}
