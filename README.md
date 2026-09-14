# Runtime Shim — gyro aim takeover

Current module: `2.5.9-trigger-grace500` (`versionCode=259`). The game remains
responsible for sniper target acquisition. Once a fully opened sniper scope and
its `doing + target` trigger are both present, a bounded correction is added to
the real NDK gyro samples and takes over tracking the locked target. Custom
touch actuation is disabled.

## Current behavior

- `libunity.so` imports the Android NDK sensor API directly. The probe redirects
  only its writable `ASensorEventQueue_getEvents` GOT entry.
- The wrapper calls the original API and modifies only type-4 gyro X/Y values
  while a target captured from the game's aim assist is locked. Gravity,
  accelerometer and all other events are untouched.
- The release binaries contain no loader, runtime or gyro logging.
- No executable code page is modified and no page permission is changed.
- The Android touch bridge remains disabled for the entire probe session.
- Activation is sniper-only and requires all three gates:
  `m_EnableAimAssistanceForSniper`, a fully opened scope
  (`projection_y >= 5.30`), and
  `m_DoingAimAssist && m_CurrentAimAssistTarget != null`.
- `m_DoingAimAssist` is checked on every 8 ms tick. If it drops before the aim
  reaches the 3.5 px settle radius, the same locked pawn may finish for at most
  500 ms while it remains valid. Closing scope or an invalid/dead/hidden target
  stops immediately. `m_IsPawnVisible` is not resolved or used.
- Scene sampling is split into two tiers. The idle 8 ms poll reads only the
  cached local ownership links, aim flags/current target and one projection
  coefficient. While active, the reader validates and reads only the pawn in
  `m_CurrentAimAssistTarget` (or the same retained pawn during finish grace),
  its two transforms and the camera matrices; it does not traverse unrelated
  enemies. A missing scene/local binding is retried at 250 ms instead of
  forcing a full scan every 8 ms.
- Shooting training, regular team PVP and bomb-mode pawn/controller subclasses
  are in the validated exact-class profile. Individual 1v9 mode is detected by
  its exact `WNIndivdualGame.IndivdualGame` class and treats every non-local pawn
  as an enemy; team and bomb modes retain camp-based enemy classification.
- At 3.5 px the cycle latches with zero gyro output. A false-to-true game
  trigger starts a new cycle; the 500 ms finish grace applies only while the
  previous cycle is unsettled. A cycle also has a hard two-second fail-safe.
- The measured mapping is camera-right = sensor X negative and camera-up =
  sensor Y negative. The calibration controller uses `Kp=5.5`, a `0.15 rad/s`
  per-axis cap, 25% smoothing and a 3.5 px settle radius.
- Isolated scene-read races retain the last validated command for no more than
  64 ms, preventing zero/nonzero correction pulses. Target prediction leads
  horizontal motion by 90 ms, and the aim point is 78% from root to upper body.

The legacy input bridge remains packaged but is disabled for the whole runtime;
this release performs no synthetic touch actuation.

## Build and test

```powershell
.\build_zygisk.ps1
.\tools\test-venv\Scripts\python.exe tests\test_readonly.py
```

Output: `output/rt_shim.zip`. Current host result: 59/59 ARM64 tests pass.

## Supported profile

- Package: `com.vnggames.cfl.crossfirelegends`
- Device/HUD used for actuation: Lenovo TB375FC, `2944x1840`
- ABI/API: ARM64, Android API 26+, Zygisk API v5 with root companion
- Unity build ID: `1a60ff52f7bb4ad5de0465b12a83aba3d7af0700`
- IL2CPP build ID: `a8793b51fee671e98de0cc0ad42bb85ffd5d0677`

Other binary builds, HUD layouts and zoom profiles fail closed. Historical
metadata, loader and earlier 200 px experiments remain under `docs/`,
`knowledge.md` and `knowledge-phase2.md`.
