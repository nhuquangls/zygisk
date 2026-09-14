#ifndef CF_RESOLVED_TARGETS_H
#define CF_RESOLVED_TARGETS_H

// CrossFire: Legends ARM64 targets verified by the manual xDL/IL2CPP probe.
// libil2cpp.so SHA-256:
// A27253B3B9E0E77725464A65D200BAF3DE677D5A7B448BA1FFF99412F76977E5
// GNU build ID: a8793b51fee671e98de0cc0ad42bb85ffd5d0677
//
// These values are consumed only after libil2cpp.so is loaded. Production
// code also validates the expected AArch64 words before installing a hook.
#define CF_AIM_OFFSET_RVA 0x04C3D178u
#define CF_AIM_ROTATE_CALL_RVA 0x068EBBBCu
#define CF_TIME_DELTA_RVA 0x05A63188u
#define CF_ROTATE_TOWARDS_RVA 0x05A820C0u
#define CF_AIM_ROTATION_METHOD_RVA 0x068EB51Cu

#define CF_CONTROLLER_CURRENT_TARGET_OFFSET 0x110u
#define CF_PAWN_LAST_SIMULATE_VELOCITY_OFFSET 0x218u

#endif
