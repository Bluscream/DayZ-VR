# DayZ-VR work tracks

Living backlog. Tracks are never "done": each keeps a *State* (what exists today), *Next*
(the concrete next step) and *Open questions*, so work can resume on any track after a
detour. Update the entry when you touch the track; keep history in git, not here.

## R1. True per-frame stereo (double world render)
- State: `[stereo] stereo_mode=double` (experimental, off by default) hooks the world
  render (1.29.163709 DayZ+0x8E7650, signature verified), runs it twice per in-world
  frame with the eye toggled, re-dispatches the projection (DayZ+0x952000) per eye,
  restores the consumed descriptor slot, and captures the left image inside DayZ's
  render thread before the N-th backbuffer-sized clear (`double_capture_clear`, live
  tunable) and the right at Present. `dump-eyes` writes both captures as BMP (render
  thread). Per-frame D3D mark sequences (`Double mode frame … marks:`) are logged.
  **Findings (sim, 2026-10-02):**
  1. Both passes execute (prepare/execute/finalize events), no crash in-world, same fps.
  2. The second pass renders exactly the same view: not even a 1.2 rad yaw applied to
     the context camera before the projection re-dispatch (or in prepareView) changes
     the presented image. DayZ builds its visibility/draw lists in the mode-0 prepare
     (caller DayZ+0x8E7967, inside the frame function 0x8E77C0) before the projection
     dispatch; the world render only executes those lists, so a second eye needs the
     scene preparation re-run (the frame function 0x8E77C0 calls 0x8E7650 at 0x8E7AED
     and 0x8E7B6B, the per-frame work above it is the real per-eye unit).
  3. Camera translation written at FrameBase+0x2C is honoured neither for eye offset
     nor hmd_position_scale (60x showed no shift), rotation (+0x08..+0x20) is. So eye
     separation and positional tracking have never affected rendering; current output
     is mono with head rotation.
  4. DayZ submits D3D on a separate render thread lagging the game thread by more
     than a pass; captures from the game thread see stale frames, immediate-context
     use from the debug thread crashed d3d11. The full-size clear count per frame is
     not stable (2..18), so the clear index is not a reliable pass marker.
  5. Double mode in menus/loading hung the game once; it is now gated to frames with
     a fresh projection context and calibrated camera.
- Next: Ghidra 0x8E77C0 fully: identify the minimal per-eye unit (scene traversal
  + projection + world render) and which state must be reset between the two; find
  where the renderer takes its view translation (likely a world-origin/double
  position elsewhere in the camera or context). Keep `stereo_mode=alternate` default.
- Open: whether an engine "render twice" path exists (DayZ has PiP/scopes?); GPU cost.

## R2. Depth-based second eye (fallback rendering mode)
- State: idea only. The probe already records depth-stencil state per draw (ALPHA dump),
  so DayZ's scene depth target can be identified. SuperDepth3D in
  `.references/repos/tools/Depth3D` is the reference for parallax reprojection.
- Next: only if R1 proves impossible or too costly; locate the depth target, write a
  reprojection pass in `dayz_frame_source.cpp`.

## R3. Headless frame-rate ceiling
- State (resolved 2026-10-02): the 14-20 fps on the sim rig was Monado's null
  compositor, which hard-codes a 20 fps frame interval (`null_compositor.c`:
  `U_TIME_1S_IN_NS / 20`) and ignores `XRT_COMPOSITOR_DEFAULT_FRAMERATE`; xrWaitFrame
  blocked ~45 ms of every 50 ms frame (host log `xr_ms wait_frame=`). `scripts/xr-sim.sh`
  now defaults to `SIM_COMPOSITOR=window` (main compositor, `XRT_COMPOSITOR_FORCE_WAYLAND`,
  paced at SIM_FRAMERATE=90): 75-85 fps in-world, wait_frame 2 ms, game thread 70 %
  CPU (now game-bound like on the headset's 67 fps). `SIM_COMPOSITOR=null` keeps the
  old headless path. Sources kept in `build/monado-src/` (not committed).
- Next: the per-frame `xr_ms` breakdown stays in the pose log line; use it on the headset
  to see whether WiVRn's wait dominates there too. If the Wayland window is unwanted on a
  truly headless box, `XRT_COMPOSITOR_FORCE_VK_DISPLAY` or a patched null compositor.
- Open: none.

## S1. Lua scripting layer
- State: research done. UEVR vendors Lua 5.4.4 (`.references/repos/injectors/UEVR/
  dependencies/lua/src`) and sol2 3.2.3 single header (`…/UEVR/dependencies/sol2/single/
  single/include/sol`); its `lua-api/lib` glue is decoupled from the host via a C ABI
  header and is the template. Script discovery: `*.lua` from a scripts dir, per-script
  enable, callbacks (`on_frame`, pre/post render, input), VR usertype (poses, actions,
  haptics, recenter, mod values), imgui bindings for UI.
- Next: vendor Lua + sol2 into `third_party/`, add `common/lua_scripting.cpp` exposing
  `vr.hmd_pose()`, `vr.controller_pose(hand)`, `vr.buttons()`, `vr.get/set_tunable`,
  `vr.recenter()`, `vr.add_yaw_offset()`, `vr.log()`, callbacks `on_frame`,
  `on_hotkey`; scripts from `<game>/dayz_openxr_scripts/*.lua`; reload hotkey.
- Open: UI drawing from Lua needs a text/quad renderer (see U1).

## S2. Enforce Script bridge (DayZ-side data)
- State: DayZ Standalone mods are Enforce Script (not SQF). Research done (sources:
  `dta/scripts.pbo` 1_Core/proto/EnSystem.c, 3_Game/DayZGame.c, 5_Mission/missionGameplay.c):
  file IO is `OpenFile/FPrint/FPrintln/FGets/CloseFile`, `JsonFileLoader<T>`, writable
  roots only `$profile:` and `$saves:`, no sockets/pipes/FFI anywhere in the script API.
  Per-frame hook: `modded class MissionGameplay { override void OnUpdate(float dt) }` or
  `g_Game.GetUpdateQueue(CALL_CATEGORY_GAMEPLAY).Insert(fn)`. Game data: weapon
  `GetMagazine(GetCurrentMuzzle()).GetAmmoCount()`, `GetHealth("","Health"/"Blood")`,
  `GetStaminaHandler().GetStamina()`, `g_Game.IsInventoryOpen()`, `GetMovementState()`
  stance, `GetTransport()` vehicles. Packaging: `@DayZVR/addons/*.pbo` with config.cpp
  CfgPatches/CfgMods script modules; no PBO tool installed anywhere on this host, the
  format is simple enough to write in Python. Servers with verifySignatures need the
  mod's .bikey; the local test server can run verifySignatures=0.
- State (2026-10-02): working end to end on the sim rig. `common/script_bridge.cpp`
  writes `$profile:dayzvr\vr.txt` (key=value) every `[bridge] interval_frames` and
  reads `game.txt`; `enforce/DayZVR` (packed by `scripts/build-pbo.py`, deployed as
  `@DayZVR`) writes weapon/ammo/chamber/health_level/bleeding/stamina/inventory/
  stance/raised/in_vehicle at 10 Hz and reads vr.txt into a map. `$profile:` is
  `%LOCALAPPDATA%\DayZ` without `-profiles=`. Gotchas: `GetHealth`/`GetHealth01`
  throw "cannot be called on client" (VM exceptions land in crash_*.log, one per
  frame); `PlayerBase.GetTransport` does not exist (compile error dialog blocks the
  launch). Client-safe: `m_HealthLevel`, `GetBleedingBits()`, `GetStaminaHandler()`.
- State (2026-10-02 08:45): vr.txt now also carries the wheel/pedals (`steer_valid
  steer pedals_valid throttle brake`) and raw controller state (`btn_x/y/a/b`,
  `stick_click_l/r`, `grab_l/r`, `trigger_l/r`), so Enforce features (U2 options menu,
  wrist dashboard) can react to buttons without the native side knowing them. game.txt
  gained `melee`, vehicle `steering speed driver gear rpm engine fuel`. World-module
  classes cannot see Mission-module classes: the bridge pushes values into statics
  (`DayZVRSteering`) for 4_World consumers.
- Next: a wrist HUD widget drawn from Enforce (HMD yaw + hud rect are in vr.txt);
  JSON via `JsonFileLoader` if parsing cost matters; `.bikey`/`.bisign` for
  signature-checking servers.
- Open: latency of 10 Hz file polling (fine for HUD data); alternative native hook
  into the script VM (would avoid files entirely).

## U1. Immersive UI
- State: GUI quad (world-locked menu/inventory), HUD safe-area and scale overrides,
  controller rays. No world-anchored widgets.
- State (2026-10-02): Enforce-side ammo label exists (`DayZVRAmmoCounter.c`, `[bridge]
  ammo_counter`). Finding: the first-person weapon is a separate hands model drawn
  relative to the camera; the weapon *entity* position (GetPosition, ModelToWorld of the
  "magazine" selection) is the body-attached third-person model and projects nowhere
  near the drawn gun, so the label uses a fixed offset from the aim point (which follows
  the native view offset) and hides while lowered.
- State (audit continuation, 2026-10-02): native right-grip ammo quad is implemented
  in `common/ammo_display.*` / OpenXR host, with `[hud] ammo_quad` and size/offset/tilt
  settings. Build, bitmap/arithmetic regressions and XR image-ownership regressions
  pass. Simulator creates its 170x48 swapchain and runs without XR errors; a tiny
  controller-adjacent label is visible in the compositor, but its exact text and
  real-headset readability still need a clearer close-up verification.
- State (2026-10-02 07:10): native quad verified readable on the sim compositor
  (`build/logs/ammo-quad-60plus1.png`: "60+1" seven-segment next to the right grip,
  M4A1 + STANAG 60). `hud.ammo_quad*` (show, width, offset xyz, tilt) are live
  tunables (`dayz-vr-ctl.py set hud.ammo_quad_width_meters 0.4`), registered by the
  host through `runtime_probe::RegisterTunables`. Regression found and fixed on the
  way: the audit's desktop-focus gate (`InputAllowed`) had also stopped controller
  *tracking* when the DayZ window was not the foreground, which hid the quad, the rays
  and the debug hand poses; `TrackingAllowed` now gates poses, `InputAllowed` only
  key/mouse injection.
- State (07:15): magazine changes and low ammo verified on the sim: `hands M4A1 - 3`
  shows amber "3+1", `hands M4A1 - 0` amber "0+1" (red only when nothing is loaded),
  text re-rasterises on change (`build/logs/ammo-quad-3plus1-amber.png`).
- State (07:17): weapon switching verified: `hands HuntingKnife` clears weapon/ammo in
  the bridge and the quad disappears.
- Next: verify the red state (no magazine, empty chamber shows nothing by design, so
  red only appears for ammo=0 without chamber); verify physical-controller
  placement/readability (sim grips sit at the
  frame edge, so the quad clips there). Keep the Enforce label available as the
  fallback. Fix bridge freshness/internal-magazine audit
  findings before treating displayed values as reliable for every weapon.

## U2. In-game settings UI (edit every mod setting at runtime)
- State: settings live in `dayz_openxr.ini`; the `[stereo]`/`[gui]`/`[comfort]`-style
  keys the render path reads per frame are already live tunables (debug plugin
  `set`, hotkey toggles), keys consumed at hook installation need a restart. No
  in-game editor. UEVR's VR-friendly overlay (imgui drawn into a world-locked quad,
  operated with the controller ray) is the reference; plain desktop imgui is not VR
  friendly.
- State (2026-10-02): host-owned settings can now join the probe's tunable table
  (`runtime_probe::RegisterTunables`, atomics written by the debug thread, read per
  frame); `[hud] ammo_quad*` is the first user. The ini clamps and the table bounds
  are still written twice (host config read + table row).
- State (09:15): host tunables come from one table (`LoadHostTunables`: ini key
  `section.key`, default, range, boolean flag); the ini read, the clamp and the
  protocol row share it; `static_assert` keeps `hostTunables_` sized to the rows.
  23 entries (`hud.* melee.* vehicle.* stance.* haptics.*`) listed and range-checked
  through `dayz-vr-ctl.py` on the sim. The render-path tunables (`[stereo]`/`[gui]`/
  `[comfort]`) still live in the probe's own table.
- Next: (1) make as many settings as possible runtime tunables (register every ini
  key through one table with type/range/"needs restart" flag, so the ini parser, the
  debug protocol `tunables`, hotkey toggles and the UI all share it; fold the host's
  `ReadFloat`+clamp pairs into that table); (2) UI options,
  pick one: (a) reuse DayZ's own UI through the Enforce mod (an options tab/menu built
  from `.layout` widgets, values exchanged through the S2 bridge, gets the GUI quad +
  controller ray for free); (b) native imgui into the existing GUI quad swapchain
  (`dayz_frame_source`), ray-driven like UEVR. (a) is zero new renderer code and fits
  the world-locked menu quad; start there. (3) persist changes back to the ini.
- Open: Enforce cannot write outside `$profile:`, so persisting from (a) goes through
  the bridge; menu open/close bound to a controller chord.

## M1. Motion controls
- State: WASD/turn/jump/use/inventory/menu/hotbar on sticks and buttons; stick turn
  via the aim loop (smooth/snap); left stick click recenters; controller_aim decouples
  the weapon from the head; WMR, Touch and Index binding profiles work. Touch Pro, Touch
  Plus, Pico, Cosmos and HP profiles fail with -22 because their extensions are not
  enabled.
- State (2026-10-02 08:32): the five profile extensions are enabled when the runtime
  exposes them (named as strings, the vendored SDK predates most) and their bindings
  are suggested only then; Monado exposes all five and all eight profiles bind (Vive
  Cosmos only has `squeeze/click`, so its binding set uses that; no -22 left).
- State (09:05): haptics on fire: a vibration action bound on every profile
  (`output/haptic`, both hands), `common/shot_detector.*` turns the bridge's
  magazine+chamber readback into shot events (drop of exactly one, same weapon,
  no bridge gap), `[haptics] fire/fire_seconds/fire_amplitude` live tunables,
  `dayz-vr-ctl.py haptic` test pulse. Verified on the sim: client `fire` (new test
  verb, WeaponManager.Fire) twice -> two "haptic pulse: shot from M4A1" lines.
  Detection lags the shot by up to one bridge interval (100 ms); a native hook on the
  weapon fire event would remove that.
- Next: gesture reload; two-handed grip; haptics for melee hits and vehicle
  collisions (bridge has no event for either yet).

## G1. Game and server control for testing (spawn, teleport, vehicles)
- State: `enforce/DayZVR_Server` and `scripts/dayz-cmd.sh` implement local-server
  give/hands/spawn/teleport/heal/time/weather commands and result logs. Client
  `--client raise 1` was exercised successfully during the 2026-10-02 simulator run.
  Test-server ports are now published only on loopback, and setup preserves the
  deployed mod/profile and refuses to overwrite a running server.
- State (2026-10-02 08:30): `hands <class> [mag|-] [rounds]` (drop then create on the
  next tick, internal magazines filled), `tpto [dx dy dz]`, `enter` (driver seat via
  CommandHandler), per-model wheel classes; client hooks `raise`, `enter`, `steer`.
  Used to verify the ammo quad (60+1, 3+1, 0+1, Mosin 4+1, knife) and the vehicle
  steering/pedal pipeline. Spawned cars are not CE-registered and vanish on reconnect.
- State (08:32): `give` capped at 50, `info` reports position/direction/alive/vehicle/
  held item.
- State (08:45): `exit` (client + server, `HumanCommandVehicle.GetOutVehicle` from the
  CommandHandler tick) added.
- State (09:25): limits on both channels: lines over 200 characters are dropped
  (server logs a truncated prefix + "rejected"), more than 8 words is refused, and
  `give`/`hands`/`spawn` check `ConfigIsExisting("CfgVehicles <class>")` first
  ("unknown class"). Verified on the sim. Note: `dayz-cmd.sh` waits for the full
  command echo, so a rejected over-long line times out in the wrapper.
- Next: `give` into a specific slot. This command channel is for the isolated
  local test rig; public-server administration/authentication is not implemented.

## M2. Motion-controlled melee
- State: idea. DayZ melee is a key press with animation (`MeleeCombat`, `DayZPlayerMeleeFightLogic_LightHeavy`), hit detection server-side from the animation. Controller
  pose and velocity are available natively (grip pose per frame).
- State (2026-10-02 07:20): step (1) implemented, default off. `common/melee_swing.*`
  (pure `SwingDetector`: smoothed grip speed, swing starts above `light_speed`, peak
  classified light/heavy, one event per swing, cooldown + rearm below `rearm_speed`;
  `tests/melee_swing_test.cpp` in the gate). Host `UpdateMotionMelee` taps the attack
  button (left mouse) for light, holds it `heavy_hold_seconds` for heavy, only while
  the bridge reports `melee=1` (fists or `IsMeleeWeapon()` in hands), no inventory, no
  GUI quad, window focused. `[melee]` ini keys are live tunables (`melee.*`). Sim
  controllers are static, so only the negative path is verified in-game (enabled live,
  no spurious events, bridge flag toggles with knife vs rifle).
- Next: real-headset tuning of the speed thresholds and the heavy hold time
  (DayZ's heavy attack needs the button held through the wind-up);
  (2) hand-model alignment: controller aim already drives the camera, melee needs the
  weapon rotation to follow the hand (Enforce: `player.GetItemInHands()` has no public
  transform override; investigate `DayZPlayerImplement` bone override / `Human` IK or
  native camera-relative transform patch); (3) haptics on hit (server → client RPC via
  the bridge, or draw-call heuristic). Gate everything behind `[melee]` ini keys.
- Open: anti-cheat/serverside acceptance of rapid melee; fists vs knife vs hammer
  animations differ in timing.

## V2. Vehicles: controller steering and grips
- State (superseded below): started as keyboard A/D only; the key-pulse idea and the
  virtual gamepad were never needed because `Car.SetSteering`/`SetThrottle`/`SetBrake`
  are script natives that beat the engine's input when called from `CarScript.OnUpdate`.
- Open (still): seated recenter (`[comfort]` head height) and a view-lock-to-vehicle-yaw
  option; wrist dashboard from game.txt vehicle data.
- State (2026-10-02 07:55, test rig): `scripts/dayz-cmd.sh spawn OffroadHatchback`
  (wheels mapped per model: HatchbackWheel, CivSedanWheel, Truck_01_Wheel, <type>_Wheel),
  `tpto [dx dy dz]` stands the player at the driver's door, server `enter` runs
  `StartCommand_Vehicle(vehicle, 0, seat)` like the vanilla action's Start(), client
  `--client enter` requests `ActionGetInTransport` (needs a cursor hit position at the
  seat selection for `CCTCursorNoRuinCheck`; all sub-conditions pass) and falls back to
  the local `StartCommand_Vehicle`. Result so far: the server seats the player (a second
  `enter` reports the seat taken) but the client never reports `in_vehicle=1`; the
  action request path also starts nothing visible. Spawned cars are deleted when the
  client reconnects (not CE-registered), so spawn+enter must happen in one session.
  Steering experiment is wired: `--client steer <v>|off` sets `DayZVRSteering.s_Override`
  and `modded class CarScript.OnUpdate` calls `SetSteering` while the local player
  drives; game.txt gets `steering=`, `speed=`, `driver=` once in a car. Untested until
  entry works.
- State (08:06): **entry and script steering work.** Vehicle commands only take effect
  when `StartCommand_Vehicle` is called from the player's `CommandHandler` tick
  (vanilla's DEVELOPER-only `TryGetInVehicleDebug` does the same), so both mods now
  park the transport in a modded `PlayerBase` field and start the command on the next
  tick; `--client enter` + server `enter` seat the player (`in_vehicle=1 driver=1`).
  `--client steer 0.6` holds `GetSteering()=0.6` for as long as the override is set
  (speed went negative: the car rolled, wheels present) and `steer off` returns it to 0,
  so `CarScript.OnUpdate` → `SetSteering` beats the engine's keyboard steering.
  Analogue VR steering can therefore be done in script, fed from vr.txt.
- State (08:17): two-hand wheel implemented end to end. `common/vehicle_steering.*`
  (rim = line between the grips, tilt → steer, deadzone, invert; host test) → host
  `PublishVehicleSteering` → vr.txt `steer_valid=`/`steer=` → `DayZVRBridge.ReadVr`
  copies into `DayZVRSteering` statics (World module cannot see Mission classes) →
  `CarScript.OnUpdate` `SetSteering` while the local player drives and the value is
  fresh (<1 s). `[vehicle]` ini keys are live tunables (`vehicle.*`). Verified on the
  sim: `invert` flips the sign and the car reports `steering=-0`, `vehicle.steering=0`
  drops `steer_valid` and the override; the static sim hands are level so the
  magnitude cannot be exercised headless.
- State (08:23): trigger pedals wired (right = throttle, left = brake): while
  `in_vehicle` the host publishes `pedals_valid=1 throttle= brake=` instead of pressing
  mouse buttons, the mod applies `SetThrottle`/`SetBrake` only while a pedal is pressed
  (>0.02) so W/S keep working. Sim: `pedals_valid` flips 0→1 on entering the car,
  values stay 0 (idle sim triggers), no errors; pressed values need real hands.
- State (08:32): `vehicle.require_grip` (default on) makes the wheel valid only while
  both squeezes are held (sim: steer_valid 0 with idle grips, 1 when the option is off);
  game.txt adds `gear= rpm= engine= fuel=` for a dashboard widget.
- Next: (1) check `SetThrottle` actually beats the engine's own input like steering
  did (real headset or a sim with scripted trigger values); (2) real-headset tuning of
  `wheel_max_degrees`/`deadzone`; (3) hands visibly on the wheel (hand models) once the
  hand-model track exists; (4) wrist dashboard (Enforce widget or native quad) from the
  new game.txt keys.
- State (09:12): in-car buttons through the bridge: the host stops injecting A /
  LGRAB+A / hotbar chords while `in_vehicle` and the mod reads `btn_a`, `grab_l`,
  `stick_click_r` from vr.txt: A = engine start/stop (client-side `EngineStart`/
  `EngineStop` as the vanilla actions do for physics vehicles; stop refused above
  8 km/h), grip+A = `ActionSwitchLights`, right stick click = `ActionCarHornShort`
  (both through `ActionManagerClient.PerformActionStart`, so the server executes
  them). B stays the handbrake. Client test verbs `engine|lights|horn`. Sim: engine
  1 -> rpm 800 -> 0 verified; lights/horn actions accepted (`lights=` added to
  game.txt for the readback). 09:16: `lights=` verified 0 -> 1 -> 0 on the sim, so
  the server executes the action-manager path; the horn uses the same path.
- Next: (5) hear the horn on a real client;
  (6) seated recenter / view lock to vehicle yaw; (7) a native fire hook to remove
  the 100 ms haptic latency.

## C1. Window-drag crash
- State: guard patch (`[patches] guard_execute_without_prepared_view`) deployed; crash
  not reproducible headless; crash reporter logs an event trail on the next real crash.
- Next: when the headset session reproduces it, read `dayz_openxr.log` for
  `Skipped executeView` or a `Fatal exception` block and fix the root cause.

## V1. Headset verification backlog
- Aim-loop warm-up: on the sim the first in-world minute (25-33 fps) has the yaw gain
  estimate swinging before it settles (see T1); confirm or rule out at headset frame rates.
- Closed-loop aim at 67 fps (gain learning engages above 3 counts/frame), lock_yaw/pitch
  feel, vignette look, controller_aim with real tracking, hotkeys F9-F12, stick-click
  recenter, GE-Proton11-7.

## H1. Code health
- State: `common/dayz_runtime_probe.cpp` is ~3400 lines (limit is far lower); new work
  goes into separate files (`dayz_patches`, `dayz_hotkeys`, `hmd_aim_loop`, `comfort`).
- Next: carve the GUI cursor/quad code and the HUD experiments out of the probe.

## T1. Test rig and tooling
- State: `scripts/xr-sim.sh` (Monado sim: SIM_ROTATE, SIM_CONTROLLERS=simple|wmr|ml2),
  `scripts/run-dayz-direct.sh --sim` (prunes old dumps/logs/backups, records the log
  offset), `scripts/dayz-status.sh --wait N` (one-shot full report: install, artifacts,
  windows incl. dialogs, screenshot, session, log summary, crash reasons, script log,
  bridge files; nothing optional), `scripts/local-server.sh`, `scripts/
  ghidra-decompile.sh`, `scripts/dayz-vr-ctl.py` (watch shows aimerr/gain).
  Standard cycle: `build.sh --stop --deploy` → `run-dayz-direct.sh --sim -- -connect=
  127.0.0.1 -port=2302 -mod=@DayZVR` → `dayz-status.sh --wait 300`.
- State (09:25): `dayz-vr-ctl.py calibrate [--seconds --min-degrees]` takes a
  reference, waits for the head to turn, prints the snapshot comparison (camera/HMD
  yaw ratio), the aim-loop error and gain, saves both snapshots and exits 0 when the
  camera settled within 3 deg. With `stereo.controller_aim=1` it says so: the camera
  then follows the right controller and the head ratio is meaningless (sim run: head
  +8.4 deg, camera -10 deg chasing the static sim controller, error 10.8 deg at 45
  fps; the loop converged to 0.0 deg once the head stopped).
- State (09:38): `scripts/regression-run.sh [--skip-build] [--keep]`: gate+deploy (or
  stop only), server restart, blocking wait for the server mod's start-up line in the
  container log (`podman logs -f | grep -m1`, SIGPIPE-tolerant), sim launch,
  `dayz-status.sh --wait 300`, server `info`, client `print`, haptic pulse, 40 s of
  `watch` samples, `calibrate`, stop. Per-step PASS/FAIL, full logs under
  `build/logs/regression-<stamp>/`. First green run 09:33 (11/11).
- Finding (sim, 45 fps): the aim loop needs ~60-90 s after the spawn to converge; the
  first in-world minute runs at 25-33 fps while the world streams in and the learned
  yaw gain swings between -200 and -770 counts/rad before settling near -165 (pitch
  -161). After that both aim modes hold the error at 0.1 deg. The headset session
  should check whether the same warm-up shows at 67+ fps.
- Next: nightly-style run from a timer once the headset path exists; a `--no-sim`
  mode for the real headset that skips the Monado launch.


## Q1. Native VR quality target and interaction roadmap
- State: user names Titanfall2VR as the quality benchmark. The interactions below
  are requirements for this project, not a verified feature inventory of another mod.
  All tracks remain revisitable; improve their State/Next/Open entries as evidence changes.
- Next: prioritize correct native stereo and 6DoF rendering (R1), reliable tracked
  hands/input ownership, then weapon handling and world interactions. Every feature
  needs an optional setting, loss-of-tracking cancellation, calibration, and local
  simulator tests where feasible plus physical-controller verification.
- Acceptance: two eye views from the same simulation tick with actual world parallax;
  no double gameplay updates; no accumulated input on focus loss; honest reporting
  of headset-only gaps. Depth reprojection remains a separately labelled fallback.

## M3. Body inventory, holsters and physical reloads
- State: desired; no body-slot or manual-reload implementation yet.
- Next: body-relative calibrated slots for weapons, grenades and magazines; shoulder
  reach opens backpack/inventory; grip/release with explicit object ownership and
  haptic feedback. Add magazine extraction/insertion, chamber/bolt gestures and
  two-handed weapon support through validated DayZ actions, respecting inventory,
  server authority and each weapon's reload state.
- Open: reliable body pose from HMD + controllers; seated/left-handed accessibility;
  avoiding accidental grabs, duplicate items and animation/gameplay desynchronization.

## M4. Physical world and attachment interactions
- State: desired; keyboard interaction is the existing fallback.
- Next: locate a nearby door handle and bind grip + drag to open/close; vehicle
  handle chooses the correct seat; attachment gestures select/toggle/modify only
  compatible attachments. Steering/grips remain V2; gesture melee remains M2.
- Open: script/native interaction targets, handle transforms, server validation,
  reach constraints and compatibility with locked doors/occupied seats.

## M5. Physical stance and traversal
- State (2026-10-02 08:40): physical crouch/prone implemented, default off.
  `common/physical_stance.*` (head drop below standing → Erect/Crouch/Prone with
  hysteresis, host test) and host `UpdatePhysicalStance`: standing height captured at
  the first tracked frame and after every recenter (`runtime_probe::RecenterGeneration`),
  taps C (crouch) / Z (prone) toggles one at a time and waits 0.7 s for the bridge's
  stance readback (raised variants 3..5 fold onto 0..2); off while GUI, inventory or
  vehicle. `[stance]` keys are live tunables (`stance.*`). Sim HMD is static, so only
  the negative path (no taps, tunables present) is verified.
- Next: real-headset calibration of the drops (seated play needs a seated reference:
  add `stance.seated` or capture-on-demand); mantle/climb through existing DayZ
  actions; render-only head offset vs body collision agreement.
- Open: safe threshold calibration, accessibility, accidental transitions, whether
  engine mantle supports reliable hand-driven input without replaying gameplay ticks.

## R4. Reference-driven stereo investigation
- State: existing Ghidra project and decompilations in `build/ghidra/`; frame function
  `0x8E77C0`, preparation `0x85FD20`, renderer prepare `0x44F5A0`, camera refresh
  `0x7A0330` already available. Reuse them before importing the binary again.
- Next: compare locally cloned UEVR/REFramework stereo view setup and target ownership;
  inspect additional public VR-mod source if needed, saving new clones under
  `.references/` on Data. Follow camera copies and command allocation from scene
  preparation to D3D submission before choosing the per-eye hook.
- Open: cached camera matrices versus live camera fields; duplicate-pass reset rules;
  pass-completion marker on the actual D3D render thread.



additional prompts by user (might be implemented):

nothing else we can improve? especially in regards to rendering, if thats really true, look into how we can have the mod load and run sqf files or lua files (there might be examples, i know uevr has some kind of sdk for lua, but i thini for dayz sqf would make more sense, giving access to vr related stuff directly in the sqf, also look into how we can maybe imrprove ui and motion controks even more (everything optionally like ammo counter next to gun mag or making the hud/menus more immersive
split all the work we discussed so far into tasks that never get marked as complete so you can always get back to something later after working on something else and dont forget regular commits
mayge features like the floating ammo count could be enfirce scripts then if we give that bridge the necessary capabilities
Continue until you have absolutely nothing more to do and cant even come up with anything yourself that can improve the project in anyway, in which case close yourself
make sure our build script or maybe carve out to a seperate launch script that does wait some time for the process then gives info about the process and tails last logs lines and maybe existence of crash files, etc and also reports mod size/time and game size/time/version and other useful info so you dont have to go hunting every time? also read afk skill again
make everything non-optional so agents dont try to bypass anything and miss crucial info
make either the build or the start script (you chose where its more useful) cleanup all past crashdumps, logs, etc, etc so we dont grow infinite log files etc
also when you really got some spare time, figure out why our emulator only has 20fps (might require online research), maybe add to tasks you written earlier
you built in stuff that helps you control the game+server right? because you will need to spawn weapons in your hand, look at them with screenshots, etc, to see weapon model/hand model rotations, also i want motion controlled melee combat (fists, knifes, hammers, etc, etc), maybe spawn yourself a vehicle and teleport in to add and test features like controller position based steering wheel control etc also write this all down to the tasks file
if theres any documentiation on enforce script and dayz sa modding in general, save/scrape it to .references/ so we have easy offline lookup
if vppadmintools was running on the server, would that mean we can use our mods internal access to use it to spawn things/heal/etc from our client while developing without the need for ui interaction?
why are my hands spazzing out so much when the game has focus, is that a side effect of mouse control or emulation? or a rea lbug
add to tasks list: either find a way to reuse existing ui for our vr mod options, or add something similar to imgui (popular option but i dont think very vr friendly; uevr has a vr friendly overlay for its settings) where all mod settings can be edited directly ingame (try to make as many settings possible to change at runtime)
remember that i told the other agent that titanfall2vr is the gold-standard for vr mods (full native stereo rendering, full motioncontrols with many gestures, holsters on body for weapons, grenades, mags (manual reload), backpack over shoulder for opening inventory or smth, controller held/controlled steering wheels, gesture based weapon attachment toggle/modify, dragging door handles to enter vehicle at that seat, grabbing building door handles to open/close doors, crouch, prone, (mantle?), and so much more, this should become the best vr mod it can be
if you get stuck getting stereo to work, maybe look at some existing vr mod code that is locally cloned, online, or clone yourself to .references/
i think the other agent also started decompiling some parts of the dayz binary if that helps at all uwu
hand tracking also means finger curling and hand tracking support as much as possible, and obviously ingame hands must match irl hands/controller placement/rotation
also dont forget about things like vignettes, weapon scopes and overlays like night vision, flashbang, etc they all need to look good in vr

last agent response:
Deployed. Adding the in-game settings UI track to TASKS.md and relaunching in the same step.

Camera steady at 0.1° error: the judder is gone with a static headset. Now raising the weapon through the client hook and refreshing the magazine to check the label placement.

Rebuilt. Deploying both mods, then the server needs a restart for its half and the game a relaunch.

Rebuilt. Killing the dialog-blocked game and deploying, then relaunching.

Committed. Now checking the launch and the label position numbers from the bridge.

In-world. Raising the weapon and capturing the label.