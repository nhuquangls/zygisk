# Runtime Shim — gyro aim takeover

Current module: `2.8.5-spectator-guard` (`versionCode=287`). The game remains
responsible for sniper target acquisition. Once the current weapon is a sniper,
that weapon reports zooming, and its `doing + target` trigger is present, a
bounded correction is added to the real NDK gyro samples and takes over
tracking the locked target. Custom touch actuation is disabled.

## Current behavior

- `libunity.so` imports the Android NDK sensor API directly. The probe redirects
  only its writable `ASensorEventQueue_getEvents` GOT entry.
- The wrapper calls the original API and modifies only type-4 gyro X/Y values
  while a target captured from the game's aim assist is locked. Gravity,
  accelerometer and all other events are untouched.
- The release binaries contain no loader, runtime or gyro logging.
- No executable code page is modified and no page permission is changed.
- The Android touch bridge remains disabled for the entire probe session.
- Activation is sniper-only and requires the current weapon class to derive
  from `WNWeaponSniper`, its own `m_IsZooming` flag, the
  `m_EnableAimAssistanceForSniper` flag, and
  `m_DoingAimAssist && m_CurrentAimAssistTarget != null`. `projection_y` is no
  longer used to infer whether the scope is open; it is only an input to the
  gyro projection calculation.
- With a sniper cached and its scope open, `m_DoingAimAssist` is checked on
  every 8 ms tick. If it drops before the aim reaches the 3.5 px settle radius,
  the same locked pawn may finish for at most 250 ms while it remains valid.
  Closing scope or an invalid/dead/hidden target stops immediately.
  `m_IsPawnVisible` is not resolved or used.
  If the same trigger returns inside that grace, the new aim cycle retains the
  filtered target velocity instead of restarting prediction from zero.
- Scene sampling uses three cadences. With no bound in-match local pawn, scene
  binding is retried every 5000 ms. In a bound match, the pinned
  `Pawn.get_CurrentWeapon()` getter refreshes weapon classification every
  500 ms. Once a sniper is cached, the worker wakes every 8 ms; a closed scope
  returns after its direct `m_IsZooming` read, while an open scope checks the
  final aim flags/current target. Only an active trigger (or the retained pawn
  during finish grace) reads target transforms and full camera matrices; it
  never traverses unrelated enemies on these fast ticks.
- Match binding and direct-target validation use assignability to the base pawn,
  controller and local-player classes plus reciprocal ownership links. No map,
  mode subclass or camp profile participates in target selection: the active
  pawn comes only from the game's `m_CurrentAimAssistTarget`.
- `PlayerController.m_IsSpectating` is resolved by name and checked before the
  current weapon. Spectating clears cached sniper state, stops gyro correction
  immediately, and uses the 500 ms idle cadence until the next live round.
- At 3.5 px a stationary/slow cycle latches with zero gyro output. A target with
  a meaningful predicted screen lead remains in tracking until the trigger ends
  or the two-second fail-safe expires. A false-to-true game trigger starts a new
  cycle; the 250 ms finish grace applies only while the cycle is unsettled.
- The measured mapping is camera-right = sensor X negative and camera-up =
  sensor Y negative. The calibration controller uses `Kp=7.0`; its per-axis
  cap follows a smoothstep curve from `0.13 rad/s` at 64 px to `0.23 rad/s`
  at 300 px and beyond, then proportional control slows it near the 3.5 px
  settle radius.
  Instead of the old 25% filter on the 8 ms controller tick, the sensor hook
  slews only the added correction at each type-4 gyro sample. It reaches the
  far cap in about 12.5 ms at 400 Hz and an inactive command stops immediately;
  the player's physical gyro is untouched.
- Isolated scene-read races retain the last validated command for no more than
  64 ms, preventing zero/nonzero correction pulses. A separate 100 ms timeout
  in the sensor consumer clears correction if the worker stalls. Horizontal
  velocity is measured from the pawn root, never the animated chest point.
  Above the low-speed noise gate, lead is linear in filtered velocity with a
  fixed 60 ms horizon. A smoothstep weight suppresses root jitter at or below
  0.25 m/s and reaches full prediction at 0.80 m/s; there are no discrete speed
  bands. A new direction begins at 30% of its measured velocity and converges
  through the same EMA, avoiding a one-tick jump. The aim point remains 78%
  from root to upper body and vertical motion is not predicted.

The legacy input bridge remains packaged but is disabled for the whole runtime;
this release performs no synthetic touch actuation.

## Build and test

```powershell
.\build_zygisk.ps1
.\tools\test-venv\Scripts\python.exe tests\test_readonly.py
```

Output: `output/rt_shim.zip`. Current host result: 72/72 ARM64 tests pass.

## Supported profile

- Package: `com.vnggames.cfl.crossfirelegends`
- Device/HUD used for actuation: Lenovo TB375FC, `2944x1840`
- ABI/API: ARM64, Android API 26+, Zygisk API v5 with root companion
- Unity build ID: `1304f8f523fbba8d98ab8d775ae4e3d696efdb04`
- IL2CPP build ID: `33384ad3538f357d057987fa88c9f4eba485e563`

Other binary builds, HUD layouts and zoom profiles fail closed. Historical
metadata, loader and earlier 200 px experiments remain under `docs/`,
`knowledge.md` and `knowledge-phase2.md`.
