cmake_minimum_required(VERSION 3.14)
project(cf_readonly LANGUAGES C)
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
if(NOT ANDROID OR NOT ANDROID_ABI STREQUAL "arm64-v8a")
    message(FATAL_ERROR "The payload requires Android arm64-v8a")
endif()
# Read-only game data plus Android MotionEvent input; no native game patches.
add_library(cf_readonly SHARED
    readonly_runtime.c readonly_memory.c readonly_module.c readonly_exports.c
    il2cpp_metadata.c aim_math.c aim_policy.c gyro_controller.c scene_snapshot.c
    android_input.c sensor_probe.c)
target_include_directories(cf_readonly PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/../build/bridge")
target_compile_options(cf_readonly PRIVATE
    -fPIC -O2 -g -Wall -Wextra -Werror -fvisibility=hidden)
target_link_libraries(cf_readonly PRIVATE m dl)
set_target_properties(cf_readonly PROPERTIES OUTPUT_NAME "gcloudsync")
