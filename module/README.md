# Runtime Shim

- Compatibility shim for the tested 2944x1840 HUD build of com.vnggames.cfl.crossfirelegends.
- ARM64 Android API 26+, Zygisk API v5. Keep the app's current tested game build.
- Read-only engine data; correction is added only to type-4 NDK gyro X/Y samples.
- Sniper-only acquisition requires a current `WNWeaponSniper`, that weapon's
  direct `m_IsZooming` state, sniper aim enabled, and the game's
  `m_DoingAimAssist + m_CurrentAimAssistTarget` trigger. `projection_y` is used
  for gyro projection only, not as a scope-open threshold.
- `m_DoingAimAssist` starts acquisition. If it drops before the controller has
  settled, a still-valid locked target gets at most 500 ms to finish; closing
  scope or an invalid target stops immediately. `m_IsPawnVisible` is not used.
- Polling is adaptive: 5000 ms while no in-match local pawn is bound, a
  500 ms current-weapon refresh while in a match, and an 8 ms trigger tick once
  a sniper is cached. While active, only the pawn in
  `m_CurrentAimAssistTarget` (or that same retained pawn during the grace) is
  read; unrelated enemy lists are not scanned.
- Custom touch actuation and runtime logging are disabled in this release build.
- No overlay, no joystick/shooting/multitouch blending, no game patches.

Build from the source repository with `build_zygisk.ps1`. Output is `output/rt_shim.zip`.
Module ID `rt_shim`; it replaces the older `cf_aim_speed_zygisk` module (remove or disable that module before enabling this one).

See the repository validation notes for measured runtime results.
