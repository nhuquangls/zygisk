# Zygisk 200 px build validation - 2026-09-10

## Loader fix 2.2.1

Actual installed 2.2.0 startup returned from `preAppSpecialize` when
`exemptFd(payload_fd)` returned false. It never reached payload loading or the
scene resolver, so the failure did not depend on login/lobby state.

2.2.1 buffers the payload before specialization and closes the source descriptor.
It retains the existing app-local file and Android linker path. The companion
socket is handed off only if its socket type, device and inode still match after
specialization. A closed or reused descriptor is discarded without closing its
replacement. Read/write loops retry EINTR, and allocation failure is handled.

Validation: build succeeded; 29 existing ARM64 tests passed. The standalone
`tests/test_specialize_socket.cpp` executable passed five cases on the Android
device: successful ownership transfer, descriptor closure, replacement by a
different socket, replacement by a file during cleanup, and rejection of a
non-socket descriptor. These tests do not emulate Zygisk specialization itself.

KernelSU installed the fixed ZIP successfully; staged binary hashes matched the
local build. Backup and installer evidence:
`output/installs/fd-fix-20260910_170623/`.
`znctl znmod reload cf_aim_speed_zygisk` reported zero modules to reload, so a
device reboot was requested. ADB then disconnected and TCP port 5555 refused
connection. ADB was subsequently restored. The payload loaded, but thread_attach aborted before runtime thread registration was enabled. The original fstat socket validation also failed; the current loader uses SO_COOKIE and reports its validation error explicitly.

## Final implementation

- Native worker resolves metadata and copies a coherent, bounded Pawn/Transform/camera snapshot approximately every 16 ms.
- Named metadata fields are resolved again for each process. No process-specific absolute addresses are compiled in.
- Unity native Transform and cached camera layouts are pinned to the verified Unity and IL2CPP build IDs listed in the root README.
- `aim_policy.c` applies a 200 px circle to both acquisition and an existing target. Stale, invalid, friendly/dead/hidden targets and built-in aim assistance are rejected.
- An Android main-thread DEX observes the foreground Unity view and existing Window.Callback. Every original touch event and result is forwarded.
- Once touches have been released for 500 ms, it requests a bounded swipe over a nonblocking companion FD. The root companion executes the fixed Android input command. At most one request is outstanding.
- Each swipe lasts 120 ms, with maximum travel 40 px per axis. A committed swipe finishes with UP, including after a new touch, a focus change, or a target leaving the 200 px circle. The next request requires a fresh valid target again.
- The fixed anchor (1943,736) belongs to the tested 2944x1840 HUD. Other viewports stay inactive.

## Device results

Device: Lenovo TB375FC, Android 16, ARM64. Training room, stationary bots, approximately 20 degree vertical camera FOV, built-in normal and sniper aim flags disabled.

| Trial | Input duration | Initial error | Final error | Result |
|---|---:|---:|---:|---|
| Native payload + companion v7 | 3 s | 187.82 px | 7.91 px | Worker stopped, game stayed alive |
| Final native payload + companion v8 | 10 s | 83.96 px | 4.87 px | Reached 8 px deadband and stayed there; worker stopped |

Artifacts:
- [v7 measurements](../output/runtime_metadata/native-aim200-companion-v7.json)
- [v8 measurements](../output/runtime_metadata/native-aim200-companion-v8.json)
- [final manifest and cleanup](../output/runtime_metadata/aim200-build-result.json)

The validation tool temporarily loaded the native payload and passed a JavaVM plus a test loopback socket to the same C companion handler compiled as a standalone executable. That test listener is not in the ZIP; production uses `connectCompanion()` and an inherited Unix socket.
Targeting, projection, the 200 px policy, touch gating and swipe requests were performed by payload/companion code. Python/Frida only loaded the build and sampled results.
One preliminary ADB swipe positioned the target for the v8 trial; the v8 report's initial sample was recorded after that preparatory swipe.

During the original v7/v8 trials no ZIP was installed and no reboot was performed.
Those results tested the payload and companion, not the production loader. See
the 2.2.1 section above for subsequent installation and lifecycle work.

## Earlier failure

Direct Java MotionEvent delivery through UnityPlayer.injectEvent and Activity dispatch accepted events but did not rotate the camera. One such run ended with PID 7162 receiving signal 7 (SIGBUS) at 11:24:17.191; Android provided no tombstone/backtrace identifying the cause. This is not evidence of a specific ACE mechanism.
The user reopened the game into training (PID 18164). The final build removes direct MotionEvent injection and uses root companion/system input instead.
[Exit information](../output/runtime_metadata/aim200-exit.txt) and [last process log](../output/runtime_metadata/aim200-mainlog.txt) were preserved.

## Automated checks

29 ARM64 Unicorn tests passed against compiled C code, covering read failures and pointer bounds, ELF export validation, native Transform composition and bad parent chains, projection, the 200 px acquisition/retention boundary, stale/zoom/missing-lock rejection, runtime failure states and current ZIP payload identity.
Companion protocol tests verify fragmented packet handling and rejection of bad magic, anchor, movement bounds, duration and no-op requests before a child can execute.
Shipping payload symbol checks reject patch engines and game-memory-write APIs. The standalone loopback test entry is excluded from the module build.

The tests do not emulate Android UI/JNI, SELinux or Zygisk specialization. Device trials cover the native/JNI/input path but are short and stationary; moving targets, other zoom sensitivities, alternate HUDs, active multitouch and long sessions remain unverified.

## Sources used for Android bridge/build behavior

- [Android Window.Callback](https://developer.android.com/reference/android/view/Window.Callback)
- [Android MotionEvent](https://developer.android.com/reference/android/view/MotionEvent)
- [Google R8/D8 project and distribution](https://r8.googlesource.com/r8/)

The game-specific layouts and observed input results come from the local runtime artifacts, not these API references.

## Startup fix 2.2.2

The thread_attach abort was symbolized to il2cpp_metadata.c. Checking only for a published assembly did not prevent it. The pinned IL2CPP binary calls a registration routine that branches to abort while the 32-bit flag at load_bias + 0xbe000e0 is zero. The reader now waits for that flag to equal one without modifying it. This is specific to the already pinned IL2CPP build.

The cold-start trial at 17:18:46 (PID 11363) logged waiting for thread registration, then scene resolver ready at 17:18:49. The game remained alive after 25 seconds. This trial still used the cached 2.2.1 loader; it does not validate the new SO_COOKIE handoff. Logs: output/runtime_metadata/zygisk-startup-222b.log.

The 31 ARM64 tests include regressions for an absent game assembly and a zero/unreadable/invalid registration flag. The five Android socket tests cover ownership and FD reuse using the current cookie guard.
