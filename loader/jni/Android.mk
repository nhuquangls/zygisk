LOCAL_PATH := $(call my-dir)

include $(CLEAR_VARS)
LOCAL_MODULE := rt_shim
LOCAL_SRC_FILES := loader.cpp input_companion.c
LOCAL_C_INCLUDES := $(LOCAL_PATH)
LOCAL_CPPFLAGS := -std=c++17 -fno-exceptions -fno-rtti \
    -fvisibility=hidden -fvisibility-inlines-hidden -Wall -Wextra -Werror
LOCAL_CFLAGS := -fvisibility=hidden -Wall -Wextra -Werror
LOCAL_LDFLAGS := -Wl,--gc-sections -Wl,--exclude-libs,ALL
LOCAL_LDLIBS := -ldl -lstdc++
include $(BUILD_SHARED_LIBRARY)
