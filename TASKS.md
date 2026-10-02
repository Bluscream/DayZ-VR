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
- Next: expose vr.txt data to a sample Enforce feature (U1 ammo counter needs the
  native side, but a wrist HUD widget can be drawn from Enforce directly with the HMD
  yaw); add buttons/hotkey events to vr.txt; JSON via `JsonFileLoader` if parsing
  cost matters; `.bikey`/`.bisign` for signature-checking servers.
- Open: latency of 10 Hz file polling (fine for HUD data); alternative native hook
  into the script VM (would avoid files entirely).

## U1. Immersive UI
- State: GUI quad (world-locked menu/inventory), HUD safe-area and scale overrides,
  controller rays. No world-anchored widgets.
- Next: ammo counter quad attached to the right controller grip (needs S2 data), then
  optional HUD elements as controller/wrist-anchored quads; all behind ini flags.

## U2. In-game settings UI (edit every mod setting at runtime)
- State: settings live in `dayz_openxr.ini`; the `[stereo]`/`[gui]`/`[comfort]`-style
  keys the render path reads per frame are already live tunables (debug plugin
  `set`, hotkey toggles), keys consumed at hook installation need a restart. No
  in-game editor. UEVR's VR-friendly overlay (imgui drawn into a world-locked quad,
  operated with the controller ray) is the reference; plain desktop imgui is not VR
  friendly.
- Next: (1) make as many settings as possible runtime tunables (register every ini
  key through one table with type/range/"needs restart" flag, so the ini parser, the
  debug protocol `tunables`, hotkey toggles and the UI all share it); (2) UI options,
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
- Next: enable the matching extensions when available before suggesting those profiles;
  haptics on fire (needs S2 or a draw-call heuristic); gesture reload; two-handed grip.

## G1. Game and server control for testing (spawn, teleport, vehicles)
- State: the local server (`scripts/local-server.sh`, podman, verifySignatures=0) has no
  command channel beyond BattlEye RCON (kick/ban/say only). The client bridge (S2) can
  only read the player. Fresh characters spawn with no weapon, so weapon/hand pose,
  ammo counter and melee features cannot be inspected without spawning gear.
- Next: a server-side Enforce mod `enforce/DayZVR_Server` (loaded with `-serverMod=`)
  that polls `$profile:dayzvr/cmd.txt` in the server's profile volume and executes
  lines: `give <class> [count]` (CreateInInventory on the first player, magazines via
  `SpawnAttachedMagazine`/`CreateInInventory`), `hands <class>` (weapon into hands with
  a full magazine), `spawn <class> [x y z]` (CreateObject near the player, vehicles with
  wheels/battery/spark plug/fuel), `tp <x> <y> <z>` / `tp <preset>` (SetPosition), `heal`,
  `time <h>`, `weather clear`; host wrapper `scripts/dayz-cmd.sh give M4A1` writing into
  the podman volume; results logged to `$profile:dayzvr/cmd.log` and shown by
  `dayz-status.sh`. Then: `scripts/dayz-status.sh` screenshot + `dayz-vr-ctl.py dump-eyes`
  to inspect weapon/hand model rotations under controller aim.
- Open: whether the client mod can request this itself (RPC to the server mod) so one
  `@DayZVR` on both sides suffices; admin-only gating for public use.

## M2. Motion-controlled melee
- State: idea. DayZ melee is a key press with animation (`MeleeCombat`, `DayZPlayerMeleeFightLogic_LightHeavy`), hit detection server-side from the animation. Controller
  pose and velocity are available natively (grip pose per frame).
- Next: (1) native: swing detection from right-controller velocity (speed threshold,
  direction) → emit the melee key (light tap, heavy on fast swing) only while a melee
  weapon or fists are in hands (game.txt weapon class / `weapon=` empty + no item);
  (2) hand-model alignment: controller aim already drives the camera, melee needs the
  weapon rotation to follow the hand (Enforce: `player.GetItemInHands()` has no public
  transform override; investigate `DayZPlayerImplement` bone override / `Human` IK or
  native camera-relative transform patch); (3) haptics on hit (server → client RPC via
  the bridge, or draw-call heuristic). Gate everything behind `[melee]` ini keys.
- Open: anti-cheat/serverside acceptance of rapid melee; fists vs knife vs hammer
  animations differ in timing.

## V2. Vehicles: controller steering and grips
- State: idea. Steering is keyboard (A/D) through the host's controller→key mapping;
  no analog wheel. `IsInVehicle` reaches the native side via game.txt.
- Next: (1) spawn a vehicle with G1 and sit in it; (2) native: when `in_vehicle=1`,
  map controller grip position (both hands on a virtual wheel: angle between the
  two grip positions, or single-hand angle about the wheel centre) to left/right key
  pulses proportional to angle (DayZ has no analog steering input; pulse-width the
  keys per frame like the aim loop does with mouse counts); throttle on trigger,
  brake on grip; (3) seated recenter (`[comfort]` head height) and view lock to the
  vehicle yaw option; (4) Enforce side: `CarScript.GetSpeedometer()`, gear, fuel,
  engine state into game.txt for a wrist dashboard.
- Open: whether `Car.SetSteering`-style script APIs exist client-side (grep
  `proto native` in Car/CarScript); analog steering via a virtual gamepad instead.

## C1. Window-drag crash
- State: guard patch (`[patches] guard_execute_without_prepared_view`) deployed; crash
  not reproducible headless; crash reporter logs an event trail on the next real crash.
- Next: when the headset session reproduces it, read `dayz_openxr.log` for
  `Skipped executeView` or a `Fatal exception` block and fix the root cause.

## V1. Headset verification backlog
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
- Next: a `calibrate` subcommand in dayz-vr-ctl.py that runs the yaw-ratio measurement
  (currently a scratch script); automated regression run (launch, join, measure, stop).
