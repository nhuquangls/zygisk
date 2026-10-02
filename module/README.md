# Runtime Shim

- Compatibility shim for the tested 2944x1840 HUD build of com.vnggames.cfl.crossfirelegends.
- ARM64 Android API 26+, Zygisk API v5. Keep the app's current tested game build.
- Read-only engine data; correction is added only to type-4 NDK gyro X/Y samples.
- Sniper-only acquisition requires a current `WNWeaponSniper`, that weapon's
  direct `m_IsZooming` state, sniper aim enabled, and the game's
  `m_DoingAimAssist + m_CurrentAimAssistTarget` trigger. `projection_y` is used
  for gyro projection only, not as a scope-open threshold.
- `m_DoingAimAssist` starts acquisition. If it drops before the controller has
  settled, a still-valid locked target gets at most 250 ms to finish; closing
  scope or an invalid target stops immediately. `m_IsPawnVisible` is not used.
  Reacquiring the same trigger inside that grace starts a fresh aim cycle but
  preserves its filtered target velocity.
- The 8 ms controller publishes a raw bounded correction. `Kp=7.0`; its
  per-axis cap follows a smoothstep curve from `0.13 rad/s` at 64 px to
  `0.23 rad/s` at 300 px and beyond. The sensor hook slews it at each type-4 gyro sample
  (up to 400 Hz on the tested device), reaches the far cap in about 12.5 ms,
  and expires a stale command after 100 ms. Inactive commands stop immediately.
- Polling is adaptive: 5000 ms while no in-match local pawn is bound, a
  500 ms current-weapon refresh while in a match, and an 8 ms trigger tick once
  a sniper is cached. While active, only the pawn in
  `m_CurrentAimAssistTarget` (or that same retained pawn during the grace) is
  read; unrelated enemy lists are not scanned.
- Spectator mode is gated by the dynamically resolved
  `PlayerController.m_IsSpectating` field before any weapon read. It clears
  stale sniper state, publishes no gyro correction, and waits at 500 ms until
  the player returns to a live round.
- Target validation is map/mode independent. Base pawn/controller/local-player
  relationships establish the local player, while the game-selected pawn is
  checked for identity, destroyed/hidden/health state and valid transforms.
- Horizontal prediction uses root motion rather than animated upper-body sway.
  Above 0.80 m/s it uses a fixed 60 ms horizon, making lead distance linear in
  filtered velocity. A smoothstep from 0.25 to 0.80 m/s suppresses low-speed
  root jitter without discrete bands. A new direction starts at 30% measured
  velocity and converges through the EMA.
- Custom touch actuation and runtime logging are disabled in this release build.
- No overlay, no joystick/shooting/multitouch blending, no game patches.

Build from the source repository with `build_zygisk.ps1`. Output is `output/rt_shim.zip`.
Module ID `rt_shim`; it replaces the older `cf_aim_speed_zygisk` module (remove or disable that module before enabling this one).

See the repository validation notes for measured runtime results.
