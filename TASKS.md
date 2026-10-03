# DayZ-VR work tracks

Living backlog. Tracks are never "done": each keeps a *State* (what exists today), *Next*
(the concrete next step) and *Open questions*, so work can resume on any track after a
detour. Update the entry when you touch the track; keep history in git, not here.
The flat views are `FEATURES.md` (what works, with verification state) and `TODO.md` (what
does not yet); update them when a track's State changes.

> **Checkpoint 2026-10-03: rendering-only build.** Every non-rendering feature (controls,
> input hooks, aim loop, bridge + `@DayZVR` client mod, ammo quad, haptics, melee, stance,
> steering, comfort, hotkeys) is parked under `parked/` (see its README for the restore
> steps). Tracks about those features are frozen at their last State; R1/R5 continue. The
> regression run has no client-cmd, haptic or calibrate steps while parked.

## L1. Plugin loader: dxgi.dll hosts plugins, the VR plugin is one of them
- Goal (2026-10-03, user): split the proxy into a generic plugin loader and a separate VR mod
  DLL so rendering work stays isolated, with a host API for mods to declare settings
  and hotkeys. First hotkeys: toggle VR mode (default F12) and recenter (default F11).
- Design:
  - `dxgi.dll` (target `dayz_pluginloader`, sources `loader/`) keeps only what every plugin
    needs: system-dxgi forwarding, the factory/swapchain/present/resize detours
    (MinHook), its own log `dayz_pluginloader.log`, config `dayz_pluginloader.ini`, mod
    discovery (`plugins/*.dll`, `[loader] plugins_dir`), the settings and hotkey registries.
    No OpenXR, no engine hooks, no DayZ knowledge.
  - `plugins/dayzvr.dll` (target `dayzvr`, sources `vrplugin/` + `vr_common`): everything
    that is VR today, unchanged behaviour: OpenXR host, runtime probe (engine hooks),
    frame sources, patches, crash reporter, debug plugin bridge, `dayz_openxr.ini`.
  - C ABI `include/dayz_plugin_api.h` (`DAYZ_PLUGIN_API_VERSION`): a plugin exports
    `DayzPluginDescribe(DayzPluginInfo*)` (name, version, config file) and
    `DayzPluginStart(const DayzPluginHost*, DayzPluginCallbacks*)` / `DayzPluginStop()`. Host
    callbacks: `log`, `game_dir`, `request_backbuffer_size` (the resolution override
    becomes a host service), `setting_register/get/set`, `hotkey_register`. Mod
    callbacks: `on_swapchain_created`, `on_present` (non-TEST presents, before the
    real Present), `on_resize_buffers`, `on_hotkey`, `on_setting_changed`.
  - Settings: a plugin declares typed keys (`section.key`, title, description, bool/int/
    float/enum/string, default, range, live|restart). The host stores values in the
    mod's own config file (`dayz_openxr.ini` for the VR plugin, so every script keeps
    working), notifies the plugin on change and is the single source for any UI.
  - Hotkeys: a plugin declares actions with a default key; bindings live in
    `dayz_pluginloader.ini [hotkeys] <plugin>.<action>=<key>` (same key grammar as the parked
    `dayz_hotkeys`: `F12`, `ctrl+shift+r`, `numpad5`, `0x7B`). The host polls on the
    present thread with edge detection and dispatches to the owning plugin.
  - Toggle VR mode v1 = the VR plugin's `stereo.vr_enabled` tunable: off stops the HMD
    camera rotation and eye alternation (flat game image in the headset), keeps the
    menus on the game window with the stock cursor (no GUI quad, no mouse remap, no
    virtual cursor) and pauses the HUD safe-area override; the OpenXR session keeps
    running and the square backbuffer override cannot change live (headset session
    2026-10-03: "jitter stopped, UI crushed in the centre, inventory only on the
    quad" were the three gaps). Ending/restarting the session at runtime is later.
- Phase 1 (this change): loader + API + VR plugin moved, same behaviour, regression
  passes, both hotkeys working, `build.sh --deploy` copies `plugins/dayzvr.dll` and
  installs `dayz_pluginloader.ini`, `dayz-status.sh` lists the new artifact.
- Next:
  1. Register every `dayz_openxr.schema.json` key through `setting_register` so the
     ini parser, the debug protocol `tunables`, the config editor and the in-game UI
     share one table (replaces the two native tunable tables; see U2).
  2. Settings UI inside the game, two steps: (a) host-drawn overlay panel on the GUI
     quad and the desktop, styled after DayZ's Options screen, opened from a hotkey and
     from an injected "Mod Settings" button; (b) research native injection of a real
     tab into the Options menu and keybind rows into the Controls menu (engine widget
     and input-action registries; no server involvement, no script mod). (b) is a
     multi-week reverse-engineering task and is scheduled after R1.
  3. Hotkey editing in that UI (press-to-bind), written back to `dayz_pluginloader.ini`.
  4. Move the debug plugin bridge into the host so every plugin's settings are reachable
     through `dayz-vr-ctl.py`.
  5. Toggle VR v2: end and re-create the OpenXR session, restore the plain present.
  6. In-game console (user, 2026-10-03): the loader draws a drop-down console (default
     key `^`, the key left of `1` on a German QWERTZ layout, scan code 0x29 / `VK_OEM_5`
     on that layout; bind by scan code so the layout does not matter) with command
     history and completion. Plugins register console commands (`name`, help, callback
     with argv) and console variables through the host API; every registered setting
     is automatically a console variable (`<mod>.<section>.<key> [value]`), every
     hotkey action a command. Built-ins: `help`, `mods`, `set`, `get`, `bind`,
     `unbind`, `exec <file>`, `log`. Drawn on the GUI quad in the headset and on the
     desktop; the debug plugin's line protocol becomes a remote for the same command
     table so `dayz-vr-ctl.py` and the console share one implementation.
- Open: hotkeys poll `GetAsyncKeyState`, and `[hooks] keep_focus` makes the game look
  focused even when it is not, so a bound key pressed in another window still fires;
  the Visual Studio project files (`vr_mod.slnx`, `loader/dxgi.vcxproj`) are stale since the
  CMake build became the only tested build and are not updated for the split.

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
  3a. (2026-10-03) The engine's own second-view path (RenderTargetWidget / SetWidgetWorld /
     SetCameraEx) is verified dead: `Landscape` leaves the world render-with-camera virtual
     empty (`ret`), see rendering.md "Render-target widgets and indexed cameras". No shortcut
     around re-running the scene preparation. Adopt the stricter acceptance bar from the Codex
     survey: same world timestamp per eye pair, near-object disocclusion, no eye mixing in
     screen-space effects, CPU/GPU times vs mono at equal per-eye resolution; audit occlusion
     queries, reflections, exposure, temporal history and first-person geometry per eye.
  3b. (2026-10-03) Survey of eight proprietary-engine VR mods (docs/research/
     proprietary-engine-vr-mods.md): BladeVR and Racer PCVR get correct per-eye culling,
     shadows and portals by calling the engine's whole scene render twice (draw duplication
     alone left artefacts), with the frame counter advanced and once-per-frame side effects
     gated to the last pass: same conclusion as finding 2. Queued experiments: ring-occupancy
     log / pop-per-Present eye tags; layer orientation from the head pose with per-eye
     position only; pair lock (one pose per eye pair; no world-time clamp, DayZ is
     server-authoritative) for alternate-eye; scene-preparation re-run
     with culling widened; constant-buffer matrix patch only as a fallback.
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

## U2. In-game settings UI (edit every plugin setting at runtime)
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
- State (15:20): outside the game the whole ini is editable through
  `tools/config-editor` (single binary, Linux + Windows, schema-driven from
  `dayz_openxr.schema.json`, applies live tunables through the debug plugin). The
  schema is the first complete per-key metadata table (type, range, enum, live/
  restart/unused) and `tests/test_config_schema.py` keeps it in step with the ini and
  the two native tunable tables; the in-game menu can be generated from it.
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
- State (09:57, answers "why are my hands spazzing out when the game has focus"):
  with native aim the render kept only roll on the render side, so the eyes showed
  DayZ's mouse camera, which trails the head by the closed loop's residual (one frame
  of latency, whole mouse counts, per-frame turn cap). Unfocused, no counts are
  injected and the world is still; focused, the world judders by that residual and
  the hands (drawn in head space) appear to shake against it. It is a side effect of
  the mouse control, not of tracking. Fix: `[stereo] aim_residual_render` (default
  on, live tunable) adds the residual yaw/pitch on the render side, so the eyes sit
  exactly on the head while DayZ catches up. Sim: render-native equals the reported
  error in both axes with the option on, 0.00 with it off. Not applied while
  controller_aim drives the camera (that branch already undoes the camera rotation).
- Next: gesture reload; two-handed grip; haptics for melee hits and vehicle
  collisions (bridge has no event for either yet); headset check that the residual
  render removes the judder.

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
- Open (still): seated recenter (`[comfort]` head height).
- State (09:50): `[vehicle] lock_view` (default on, live tunable `vehicle.lock_view`):
  while the bridge reports `in_vehicle` both head axes are treated like `lock_yaw` +
  `lock_pitch` (render-only), so the cabin view follows the head and the aim loop no
  longer drags DayZ's vehicle-relative camera after a world-space HMD direction.
  Sim: in the car the error reads exactly 0.0 with frozen pending counts; switching
  the tunable off re-engages the loop (error 0.1, counts moving). Headset: check
  that keyboard/stick free-look in the cabin still feels right with the lock.
- State (2026-10-02 07:55, test rig): `scripts/dayz-cmd.sh spawn OffroadHatchback`
  (wheels mapped per pluginel: HatchbackWheel, CivSedanWheel, Truck_01_Wheel, <type>_Wheel),
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
  mouse buttons, the plugin applies `SetThrottle`/`SetBrake` only while a pedal is pressed
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
  LGRAB+A / hotbar chords while `in_vehicle` and the plugin reads `btn_a`, `grab_l`,
  `stick_click_r` from vr.txt: A = engine start/stop (client-side `EngineStart`/
  `EngineStop` as the vanilla actions do for physics vehicles; stop refused above
  8 km/h), grip+A = `ActionSwitchLights`, right stick click = `ActionCarHornShort`
  (both through `ActionManagerClient.PerformActionStart`, so the server executes
  them). B stays the handbrake. Client test verbs `engine|lights|horn`. Sim: engine
  1 -> rpm 800 -> 0 verified; lights/horn actions accepted (`lights=` added to
  game.txt for the readback). 09:16: `lights=` verified 0 -> 1 -> 0 on the sim, so
  the server executes the action-manager path; the horn uses the same path.
- State (09:43): dashboard widget (`enforce/DayZVR/scripts/5_Mission/DayZVRDashboard.c`,
  `[bridge] dashboard`, vr.txt `dashboard=1`): one centred text line at the bottom of
  the HUD rectangle while the local player drives ("0 km/h   gear N   800 rpm
  fuel 100%  lights", amber when the engine is off), updated by the bridge tick;
  game.txt reports `dashboard=<visible> <text>`. Verified in the dumped eye image
  (`build/logs/dashboard-eye0.png`, under the hotbar; the 640x360 compositor is too
  small to read it). Vanilla gauges still show, so this is mainly the data path for a
  wrist-anchored version.
- Next: (5) hear the horn on a real client; (5b) move the dashboard to a wrist/dash
  anchor (native quad like the ammo display, or project the left grip pose);
  (6) seated recenter / view lock to vehicle yaw; (7) a native fire hook to remove
  the 100 ms haptic latency.

## V3. Headset session 2026-10-02 (Quest 3 over WiVRn, DayZ 1.29.163709)
First real-headset run of the new work. Launch recipe (the WiVRn flatpak's
`active_runtime.json` has a relative `library_path` that pressure-vessel cannot resolve, so
a copy with absolute paths under `build/wivrn/` is imported):
`XR_RUNTIME_JSON=$PWD/build/wivrn/active_runtime.json PRESSURE_VESSEL_IMPORT_OPENXR_1_RUNTIMES=1
PRESSURE_VESSEL_FILESYSTEMS_RW=/run/user/1000/wivrn scripts/run-dayz-direct.sh -- -connect=127.0.0.1
-port=2302 "-mod=@DayZVR"`. In-headset screenshots: `adb exec-out screencap -p` on the
mDNS serial (`adb devices`), 4128x2208 side by side. Evidence in `build/logs/headset-*`,
`quest-shot-*`, `quest-jitter.mp4`.

Verified on the headset (FEATURES.md updated): alternate-eye stereo with the GUI quad,
ESC menu usable from the headset (far/small), stick movement, haptic pulse per shot plus
the ammo label counting down (M4A1, single shots), two-hand steering (janky but steers),
trigger pedals (in 1st gear), physical crouch, `hands`/`give`/`spawn` server commands.

Findings, each with its fix state:
- Tracking and injected input only work while the DayZ window has focus (audit2 V01/V02);
  the user wants both without focus. Open; see I1 below (direct script control is the fix,
  SendInput is the cause).
- Closed-loop aim is unusable on the headset: the gain estimator diverges (-36..-1533) and
  the errors wrap to ±180° (`headset-loop-watch-closed.txt`). Open loop with
  `hmd_mouse_yaw_scale=-180` / `pitch=-135` is smooth but "the view distorts with head
  movement" (DayZ's camera trails the render camera). Render-only lock is smooth and also
  distorts. Open: clamp/anti-windup or default to open loop at those scales; investigate the
  latency between render pose and the game camera (reprojection from the previous frame).
- Stick turn is dead while `lock_yaw` (render-only yaw swallows the turn). Open.
- Eyes needed `camera_separation=-0.064`, `image_shift=-0.128` on WiVRn; these are the
  repo defaults since 2026-10-03. Direction rays ("light cones") confuse users (parked).
- GUI quad: menu and inventory show in the headset only (the user wants the desktop mirror
  too); the controller ray only clicks while focused; inventory item icons do not render on
  the quad; HMD-look pitch in the inventory is inverted. Open.
- Entering the car needed the keyboard (hold F); in the car a manual mouse recenter was
  needed; throttle in N drives backwards, gear up is Shift+E (not W), right stick does
  nothing; horn/lights showed no visible effect although the action ran 8x. Open:
  auto-recenter on enter, `ShiftTo` on throttle in N, right-stick shifting.
- Holding a grip while driving ejected the driver: RGRAB is bound to F, which is "Get out"
  on a car with physics. Fixed (3be95bf: no F while driving).
- Spawned cars had no collision: `spawn` lacked `ECE_SETUP`. Fixed (14e5161, flags as the
  game's ObjectSpawner). `hands M4A1` was refused: `KnownClass` only looked at CfgVehicles.
  Fixed (4e39ab2).
- Melee animation plays only with the arms raised (left trigger); the user wants arms and
  hands following the controllers (salute, wave) with only the legs animated. Open: needs an
  upper-body IK/animation layer (M3/M4 territory), not a flag.
- B = jump (user asks why not A); left trigger toggle/hold = raise. Open: mapping review
  once I1 lands.

## I1. Direct input: HumanInputController and ActionManager instead of SendInput
- Why: SendInput needs window focus (V01), gives only digital keys (no analog walk speed),
  and mixes badly with DayZ's own mouse camera. DayZ reads XInput natively but its PC pad
  layout is weak and flips the UI into controller mode; a virtual pad (uinput/SDL into
  Wine's xinput) adds a device layer for no gain. In-process `XInputGetState` hooking stays
  an option for what scripting cannot reach (menus, inventory navigation).
- Design: the host publishes the wanted input per frame in vr.txt (`move_speed` 0..3,
  `move_angle` rad, `aim_dx/dy` rad, `jump`, `use`, `raise`, `evade`, `freelook`,
  `inventory`, `menu`), the `@DayZVR` mod applies them from `PlayerBase.CommandHandler`
  through `HumanInputController.OverrideMovementSpeed/Angle`, `OverrideAimChangeX/Y`,
  `OverrideRaise`, `OverrideMeleeEvade`, `OverrideFreeLook` (ONE_FRAME each tick, so a dead
  bridge releases control), `OverrideJump` (DayZPlayerImplement), and the use/interact
  button through `ActionManagerClient.PerformActionStart` on the current target
  (`FindActionTarget`) or `InjectAction`. The server mod mirrors the overrides per consumed
  move the way dayz-mcp PR 191 does; without it the server may rubber-band, so the keyboard
  path stays as fallback (`[controls] inject_keys=true`).
- Bridge latency: vr.txt is written every 6 frames and read at 10 Hz; movement and aim
  need every-frame delivery, so the plugin must read a small `input.txt` each CommandHandler
  tick (or vr.txt itself at frame rate) and the host write it per frame.
- Order: movement+aim+jump+raise first (fixes focus for walking and looking), then use via
  the action manager, then menus via XInput hook or UIManager, then remove the key path.
- State (2026-10-02 17:40, supersedes the design above): the user ruled out server-dependent
  paths and asked for the engine's input manager directly. Ghidra (docs/research/input.md):
  gameplay reads named actions through the `Input` interface getters (`Input+0x28`, vtable
  `0xCC5920`), by id or by record, each gated by `HasGameFocus`; the player input controller
  update `0x4F5D40` consumes `UAMove*` values analogue and the aim through the axis-pair getter
  `0x5F5E10` as an angular rate. Implemented `common/dayz_input_hooks.cpp` (MinHook on the
  thirteen getters, the axis pair and the update; `kInputBuildChecks`) with the host-testable
  override table `common/input_actions.cpp` (press/release/hold-begin latched once per game
  frame; `tests/input_actions_test.cpp`). The host (`SyncControllerInput`) writes
  `UAMoveForward/Back/Left/Right` analogue, `UAStance`, `UAReloadMagazine`, `UADefaultAction`,
  `UATurbo`, `UAGetOver`, `UAFire`, `UATempRaiseWeapon`, `UAItem0..9`; the engine's own reading
  is combined (max / OR), so the keyboard keeps working, and the focus gate no longer blocks
  gameplay input. `[input] direct_actions` (default on), `direct_aim` (default off: head and
  stick yaw/pitch as exact per-frame angles via the axis pair, replacing the closed loop) with
  `aim_yaw_sign`/`aim_pitch_sign`. No server mod involved. Verified: host test; the DLL build.
- Verified in the sim (2026-10-02 18:00): `Direct input active`, all 11 names resolved on the
  first in-world frame, consumer answered ten times per frame (`direct_input_*` counters in the
  debug `get`), and `dayz-vr-ctl.py action UAMoveForward 1` walked the player 23 m in 6 s with
  the DayZ window unfocused, stopping on release. Two fixes on the way: the engine's
  `checkFocus` is the menu gate, not window focus, so overrides no longer consult it (the host
  clears actions while a menu is open); and the host now writes actions only on change and
  clears them on release, because rewriting the stick's zero each frame cancelled any other
  producer's value within a frame.
- Headset (2026-10-02 18:10..18:30, WiVRn): hooks active, `action UAMoveForward 1` walks
  15 m unfocused; the user reports the left stick walks in all directions, the right stick
  was dead (stick turn only went through the engine with `direct_aim`; now it uses the aim
  axis whenever the hooks are active, closed loop excepted). Closed-loop head aim produced
  the "earthquake" again; switched the deployed ini to open loop (`hmd_aim_closed_loop=false`,
  yaw -180 / pitch -135). Death screen: the proxy does not capture it, the quad is off, the
  trigger only wrote `UAFire`, so "Continue" could not be clicked; fixed by reading the
  engine's game-focus counter (`Input+0x183D4`, `MenuOwnsInput()`) and sending a real click
  while a menu owns the input and the window is focused. Menus still need desktop focus.
- Research: docs/research/vr-mod-techniques.md (UEVR, REFramework, R.E.A.L., F.E.A.R. VR,
  uuvr, vorpX, geo-11, Depth3D). Conclusion for the jitter: apply the HMD where the renderer
  builds the view matrix (post-correct), submit the pose actually rendered, feed the game
  only flattened yaw. Tracked as R5 below.
- Next: headset check of the right stick and open-loop view; `direct_aim=true` signs; then
  GUI actions (`UAUI*`) for menus without the mouse, vehicle `UACar*` shifting, and removing
  the SendInput leftovers.

## R5. Stable stereo: HMD at the renderer's view matrix, not the gameplay camera
- Why: every stable mod for a closed engine post-corrects the view where the renderer asks
  for it and submits that exact pose (docs/research/vr-mod-techniques.md). DayZ-VR writes
  the FrameBase rotation on the game thread, the render thread reads it a frame later, the
  mouse aim re-rotates the same camera, and `xrEndFrame` submits a newer pose than rendered:
  all four known jitter causes at once.
- Plan: (1) Ghidra: the view/projection matrix build in the projection dispatch `0x952000`
  (rendering.md) and the constant-buffer upload on the render thread; (2) hook it, call the
  original, compose `eye * hmd` onto the output per eye (real eye separation included);
  (3) per-frame `{frame id, predicted time, views}` record, submit those views; (4) game
  gets flattened yaw only through the direct aim axis once per frame; (5) camera-freeze and
  engine-rotation-lerp diagnostics. Keep the FrameBase path behind a flag until R5 is proven (builds on the R1 findings: scene preparation is the per-eye unit).
- State (2026-10-02 19:30, sim only from here on): Ghidra findings: the FrameBase refresh
  `0x7A0330` builds the view matrix from the camera's vtable slot 9 getter `0x9235A0`,
  which inverts the camera's own 3x4 at `camera+0x08..+0x37` (rotation and the `+0x2C`
  translation) via `0xA4D200`; the frustum planes (`+0x1B0..+0x22C`) use `+0x2C` too. So
  the translation does enter the view matrix. The in-world frame function `0x8E77C0`
  runs the scene functions `0x953240`, `0x957BF0`, `0x957100`, `0x958150`, `0x958D40`
  with the active camera (`0x4B6BE0`) BEFORE the preparation `0x85FD20` (which holds the
  mode-0 prepare and the projection dispatch where the proxy wrote the translation).
  Hypothesis: object placement is fixed relative to the camera position in those earlier
  passes, so a translation written in the dispatch never reached the image. Experiment:
  `[stereo] prepare_translation` (new, live) also applies the eye/head translation in the
  prepare hook (mode 0 and 1). If that is not enough, hook `0x8E77C0` entry and apply the
  whole HMD pose to the active camera there (one consistent camera per frame; the pose
  record for that frame is taken at the same point).
- Result (2026-10-02 20:10): **eye separation reaches the image.** With the pose written
  in the early FrameBase refresh (`0x7A0330` called from `0x4B7DE8` at frame start, before
  the scene passes), `dump-eyes` shows a horizontal shift of the near bands that is linear
  in the separation (0 m: 0 px, 0.3 m: 64 px, 1.0 m: 228 px) and zero for the far band.
  The direction says the capture labelled eye 1 was rendered with eye 0's camera: the
  eye toggled at Present while the render thread presents the frame prepared two frames
  earlier. Implemented frame records (`stereo_state::BeginGameFrame` at the early refresh
  freezes one HMD sample per game frame with the eye and both view poses; Present looks
  the record up `frame_lag` frames back for the capture's eye and the host submits that
  record's view pose with the image: `[stereo] frame_records`, `frame_lag`,
  `submit_rendered_pose`). Camera code now reads the frozen sample, so both refreshes of a
  frame see one pose. `frame_lag` measured by the parallax sign at 1 m separation
  (`scratchpad/lagtest.sh 0 1 2 3`): lags 0/2 give the swapped sign, 1/3 the right one, so the
  depth is 1 (default now 1; the log's "head yaw moved since render" stays under 0.2 deg).
  Verified after the rebuild: lag 1 at 1 m gives +80/+148 px (middle/bottom bands), at the
  real 0.064 m +16/+24 px, far band 0. Since 2026-10-03 the build itself is rendering-only
  (non-rendering code parked under `parked/`, the preset script with it), so no ini preset
  is needed. Next: headset check of smoothness with this build, then head position scale;
  pitch/roll/yaw are all render-only now; later native double render (R1).
- Mouse look: `[input] mouse_look=false` makes the aim axis report only the VR rate
  (both pair orders handled); deployed ini runs with `hmd_native_aim=false`,
  `hmd_aim_closed_loop=false`, so only the headset path rotates the view (user request).
- Verification recipe (sim): `camera_separation=1.0`, `dump-eyes`, then
  `scratchpad/eyeshift.py` (normalised cross-correlation per band) on the two BMPs; a
  real eye offset shows as opposite horizontal shifts that grow for near geometry.
- Open: the axis-pair clamp constants (`DAT_140C8AB80/64`) may cap large per-frame head
  turns at high rates; the registry's own per-frame evaluation is still unlocated.

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
- State (2026-10-02 10:00): probe is 3780 lines. Assessment: the HUD layout/safe-area
  globals (`g_hud*`) are referenced from ~100 lines spread over hooks, composite,
  capture sizing and config; the GUI cursor block (`MapGuiClientPoint` ..
  `InstallGuiMouseApiHook`, ~400 lines) shares `g_gameWindow`, the cursor API
  trampolines and `IsGuiCursorModeActive`. Both carve-outs need a session with the
  headset (or at least a GUI-cursor path on the sim, which needs a menu open) as the
  gate; the sim regression only proves in-world rendering. New code keeps going to
  separate files (today: shot_detector, regression-run, DayZVRDashboard).
- Next: carve the GUI cursor code first (cleanest boundary: a `gui_cursor` module
  owning the virtual cursor state and the GetCursorPos/Info hooks, with the probe
  passing the window handle and the mode predicate), then the HUD layout block.

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
- State (10:00): `--no-sim` added (launches without the Monado runtime flag; untested
  until the headset session).
- Next: nightly-style run from a timer once the headset path exists.


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

nothing else we can improve? especially in regards to rendering, if thats really true, look into how we can have the plugin load and run sqf files or lua files (there might be examples, i know uevr has some kind of sdk for lua, but i thini for dayz sqf would make more sense, giving access to vr related stuff directly in the sqf, also look into how we can maybe imrprove ui and motion controks even more (everything optionally like ammo counter next to gun mag or making the hud/menus more immersive
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