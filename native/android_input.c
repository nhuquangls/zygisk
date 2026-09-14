#include "android_input.h"
#include "aim_policy.h"
#include "bridge_dex.h"
#include "input_protocol.h"
#include <errno.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <unistd.h>
#include <math.h>
#include <pthread.h>
#include <string.h>

static pthread_mutex_t g_mutex = PTHREAD_MUTEX_INITIALIZER;
static CfSceneSnapshot g_snapshot;
static bool g_valid, g_enabled;
static bool g_shutdown;
static int g_status = 8;
static uint64_t g_frame_time;
static int32_t g_frame = -1;
static uintptr_t g_camera_owner;
static CfTargetPredictor g_predictor;
static jclass g_bridge; // Keep DEX/class loader reachable for registered natives.
static int g_input_fd = -1;
static bool g_pending;
static int g_rotation = -1;
static int g_layout;

__attribute__((visibility("default"))) int cf_payload_input_fd(int fd) {
    if (fd < 0 || g_input_fd >= 0) return EINVAL;
    if (fcntl(fd, F_SETFD, FD_CLOEXEC) < 0) return errno;
    // Check IPC access without issuing a command or consuming any stream data.
    if (send(fd, "", 0, MSG_DONTWAIT | MSG_NOSIGNAL) < 0) return errno;
    char byte;
    ssize_t n = recv(fd, &byte, 1, MSG_PEEK | MSG_DONTWAIT);
    if (n == 0) return ECONNRESET;
    if (n > 0) return EPROTO;
    if (errno != EAGAIN && errno != EWOULDBLOCK) return errno;
    g_input_fd = fd;
    return 0;
}

static jboolean calibrate(JNIEnv *env, jclass cl, jint rotation, jint layout) {
    (void)env; (void)cl;
    if ((rotation != 1 && rotation != 3) || (layout & ~1)) return false;
    pthread_mutex_lock(&g_mutex);
    g_rotation = rotation;
    g_layout = layout & 1;
    pthread_mutex_unlock(&g_mutex);
    return true;
}

static jboolean apply(JNIEnv *env, jclass cl, jint action, jint x, jint y) {
    (void)env; (void)cl;
    CfTouchCommand command;
    pthread_mutex_lock(&g_mutex);
    bool sent = false;
    if (!cf_touch_pack((uint8_t)action, g_rotation, g_layout, x, y, &command)) goto done;
    if (!g_enabled || g_shutdown || g_input_fd < 0) goto done;
    if (g_pending) {
        int32_t status;
        ssize_t n = recv(g_input_fd, &status, sizeof(status), MSG_DONTWAIT);
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)) goto done;
        if (n != sizeof(status) || status != 0) goto failed;
        g_pending = false;
    }
    if (send(g_input_fd, &command, sizeof(command), MSG_DONTWAIT | MSG_NOSIGNAL) != sizeof(command)) goto failed;
    g_pending = sent = true;
    goto done;
failed:
    close(g_input_fd);
    g_input_fd = -1;
    __atomic_store_n(&g_status, 17, __ATOMIC_RELEASE);
done:
    pthread_mutex_unlock(&g_mutex);
    return sent;
}

void cf_input_enable(bool enabled) {
    pthread_mutex_lock(&g_mutex);
    g_enabled = enabled;
    pthread_mutex_unlock(&g_mutex);
}
void cf_input_shutdown(void) {
    pthread_mutex_lock(&g_mutex);
    g_shutdown = true;
    g_enabled = false;
    if (g_input_fd >= 0) { close(g_input_fd); g_input_fd = -1; }
    pthread_mutex_unlock(&g_mutex);
}
static jboolean running(JNIEnv *env, jclass cl) {
    (void)env; (void)cl;
    pthread_mutex_lock(&g_mutex);
    bool active = !g_shutdown;
    pthread_mutex_unlock(&g_mutex);
    return active;
}
void cf_input_snapshot(const CfSceneSnapshot *snapshot) {
    pthread_mutex_lock(&g_mutex);
    g_valid = snapshot != NULL;
    if (snapshot) {
        g_snapshot = *snapshot;
        if (snapshot->frame != g_frame || snapshot->local_id != g_camera_owner) {
            g_frame = snapshot->frame;
            g_camera_owner = snapshot->local_id;
            g_frame_time = snapshot->time_ns;
        }
    }
    pthread_mutex_unlock(&g_mutex);
}
int cf_input_status(void) { return __atomic_load_n(&g_status, __ATOMIC_ACQUIRE); }
static void report(JNIEnv *env, jclass cl, jint status) {
    (void)env; (void)cl;
    if (__atomic_load_n(&g_status, __ATOMIC_ACQUIRE) == 17) return;
    __atomic_store_n(&g_status, status, __ATOMIC_RELEASE);
}
static jlong poll(JNIEnv *env, jclass cl, jfloatArray output, jint width, jint height, jlong locked) {
    (void)cl;
    if (!output || (*env)->GetArrayLength(env, output) < 8) return 0;
    jfloat values[8] = {0};
    CfSceneSnapshot snapshot;
    uint64_t now = cf_monotonic_ns();
    pthread_mutex_lock(&g_mutex);
    bool valid = g_valid && g_enabled && now >= g_frame_time && now-g_frame_time <= 100000000ULL;
    if (valid) snapshot = g_snapshot;
    pthread_mutex_unlock(&g_mutex);
    uint64_t target_id = 0;
    if (valid) {
        values[2] = snapshot.projection_y;
        values[3] = (float)snapshot.frame;
        values[4] = snapshot.doing_aim_assist ? 1.0f : 0.0f;
        bool scope = cf_scene_sniper_scope_active(&snapshot);
        values[5] = scope ? 1.0f : 0.0f;
        values[6] = snapshot.sniper_aim_enabled ? 1.0f : 0.0f;
        values[7] = 1.0f;
        uint64_t requested = (uint64_t)locked;
        bool retain_requested = requested &&
                                (!snapshot.game_aim_target || snapshot.game_aim_target == requested);
        uint64_t desired = retain_requested ? requested : snapshot.game_aim_target;
        if (scope && desired) {
            for (size_t i = 0; i < snapshot.count; ++i) {
                CfCandidate *candidate = &snapshot.candidates[i];
                if (candidate->id != desired) continue;
                CfVec3 measured = candidate->head, predicted;
                if (cf_target_predict(&g_predictor, candidate->id, &measured,
                                      snapshot.time_ns, &predicted))
                    candidate->head = predicted;
                CfViewport viewport = {(float)width, (float)height};
                CfAimResult result;
                uint64_t policy_lock = retain_requested ? requested : 0;
                if (!cf_aim_from_snapshot(&snapshot, &viewport, policy_lock,
                                          0.0f, now, &result)) {
                    candidate->head = measured;
                    if (!cf_aim_from_snapshot(&snapshot, &viewport, policy_lock,
                                              0.0f, now, &result)) break;
                }
                values[0] = result.delta.x;
                values[1] = result.delta.y;
                target_id = result.target_id;
                break;
            }
        } else {
            cf_target_predictor_reset(&g_predictor);
        }
    } else {
        cf_target_predictor_reset(&g_predictor);
    }
    (*env)->SetFloatArrayRegion(env, output, 0, 8, values);
    return (*env)->ExceptionCheck(env) ? 0 : (jlong)target_id;
}

bool cf_android_input_start(JavaVM *vm) {
    if (!vm || g_input_fd < 0) return false;
    JNIEnv *env = NULL;
    if ((*vm)->AttachCurrentThread(vm, &env, NULL) != JNI_OK) return false;
    bool ok = false;
    if ((*env)->PushLocalFrame(env, 32) != JNI_OK) goto done;
    jclass at = (*env)->FindClass(env, "android/app/ActivityThread");
    if (!at) goto pop;
    jmethodID current = (*env)->GetStaticMethodID(env, at, "currentApplication", "()Landroid/app/Application;");
    if (!current) goto pop;
    jobject application = (*env)->CallStaticObjectMethod(env, at, current);
    if (!application || (*env)->ExceptionCheck(env)) goto pop;
    jclass context = (*env)->FindClass(env, "android/content/Context");
    if (!context) goto pop;
    jmethodID get_loader = (*env)->GetMethodID(env, context, "getClassLoader", "()Ljava/lang/ClassLoader;");
    if (!get_loader) goto pop;
    jobject app_loader = (*env)->CallObjectMethod(env, application, get_loader);
    if (!app_loader || (*env)->ExceptionCheck(env)) goto pop;
    jclass dex = (*env)->FindClass(env, "dalvik/system/InMemoryDexClassLoader");
    if (!dex) goto pop;
    jmethodID ctor = (*env)->GetMethodID(env, dex, "<init>", "(Ljava/nio/ByteBuffer;Ljava/lang/ClassLoader;)V");
    if (!ctor) goto pop;
    jobject bytes = (*env)->NewDirectByteBuffer(env, (void *)cf_bridge_dex, sizeof(cf_bridge_dex));
    if (!bytes) goto pop;
    jobject loader = (*env)->NewObject(env, dex, ctor, bytes, app_loader);
    if (!loader || (*env)->ExceptionCheck(env)) goto pop;
    jclass loader_class = (*env)->FindClass(env, "java/lang/ClassLoader");
    if (!loader_class) goto pop;
    jmethodID load = (*env)->GetMethodID(env, loader_class, "loadClass", "(Ljava/lang/String;)Ljava/lang/Class;");
    if (!load) goto pop;
    jstring name = (*env)->NewStringUTF(env, "rt.internal.Bridge");
    if (!name) goto pop;
    jclass bridge = (jclass)(*env)->CallObjectMethod(env, loader, load, name);
    if (!bridge || (*env)->ExceptionCheck(env)) goto pop;
    JNINativeMethod methods[] = {{"read", "([FIIJ)J", (void *)poll}, {"status", "(I)V", (void *)report},
                                 {"active", "()Z", (void *)running}, {"apply", "(III)Z", (void *)apply},
                                 {"calibrate", "(II)Z", (void *)calibrate}};
    if ((*env)->RegisterNatives(env, bridge, methods,
                                sizeof(methods) / sizeof(methods[0])) != JNI_OK) goto pop;
    jmethodID start = (*env)->GetStaticMethodID(env, bridge, "start", "(Ljava/lang/ClassLoader;)Z");
    if (!start) goto pop;
    g_bridge = (*env)->NewGlobalRef(env, bridge);
    if (!g_bridge) goto pop;
    ok = (*env)->CallStaticBooleanMethod(env, bridge, start, app_loader) && !(*env)->ExceptionCheck(env);
    if (!ok) { (*env)->DeleteGlobalRef(env, g_bridge); g_bridge = NULL; }
pop:
    if ((*env)->ExceptionCheck(env)) (*env)->ExceptionClear(env);
    (*env)->PopLocalFrame(env, NULL);
done:
    (*vm)->DetachCurrentThread(vm);
    return ok;
}
