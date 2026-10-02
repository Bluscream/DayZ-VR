# DayZ-VR work tracks

Living backlog. Tracks are never "done": each keeps a *State* (what exists today), *Next*
(the concrete next step) and *Open questions*, so work can resume on any track after a
detour. Update the entry when you touch the track; keep history in git, not here.

## R1. True per-frame stereo (double world render)
- State: alternate-eye only (one eye per game frame, `[stereo] alternate_eye`), so each
  eye gets half the game frame rate and the eyes are temporally offset. Ghidra shows the
  world render at DayZ+0x8E7650 (1.29.163709): prepareView(mode 1), execute+finalize,
  a 3-pass loop, then a consumed slot `*param_3 = 0`. Hook plan: call it twice per frame
  with the eye toggled between calls, restoring `param_3[0..1]`, capturing the backbuffer
  after each call (mid-frame capture callback from the host's frame source).
- Next: add `worldRenderRva` to the build profile with a byte signature, implement
  `[stereo] stereo_mode=double` behind a flag, test on the sim rig, add an eye-dump
  debug command (BMP) to inspect both captures headless.
- Open: is the backbuffer the final scene image after 0x8E7650 or only after a later
  composite? Are the 3-pass loop / arena cursor idempotent within one frame? GPU cost.

## R2. Depth-based second eye (fallback rendering mode)
- State: idea only. The probe already records depth-stencil state per draw (ALPHA dump),
  so DayZ's scene depth target can be identified. SuperDepth3D in
  `.references/repos/tools/Depth3D` is the reference for parallax reprojection.
- Next: only if R1 proves impossible or too costly; locate the depth target, write a
  reprojection pass in `dayz_frame_source.cpp`.

## R3. Headless frame-rate ceiling
- State: 14-20 fps on the sim rig (null compositor), 67 fps with WiVRn on the headset.
  Cause unknown; the aim loop was verified at this rate anyway.
- Next: compare flat (OpenXR disabled) fps via an external counter, try
  `PROTON_DIR=…/GE-Proton11-7 scripts/run-dayz-direct.sh --sim`, check whether the null
  compositor's frame pacing (`XRT_COMPOSITOR_DEFAULT_FRAMERATE`) throttles xrWaitFrame.

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
- State: DayZ Standalone mods are Enforce Script (not SQF). Research pending on file IO
  (`$profile:` files, per-frame feasibility), PBO tooling on Linux, server-side
  verifySignatures constraints for client-only mods.
- Next: native side writes `$profile:dayzvr/vr.json` (poses, buttons) each N frames and
  reads `$profile:dayzvr/game.json` (ammo, health, stance, inventory open) written by a
  sample client mod in `enforce/@DayZVR`; build script for the PBO.
- Open: which servers accept the client mod; latency of file polling; alternative
  native hook into the script VM.

## U1. Immersive UI
- State: GUI quad (world-locked menu/inventory), HUD safe-area and scale overrides,
  controller rays. No world-anchored widgets.
- Next: ammo counter quad attached to the right controller grip (needs S2 data), then
  optional HUD elements as controller/wrist-anchored quads; all behind ini flags.

## M1. Motion controls
- State: WASD/turn/jump/use/inventory/menu/hotbar on sticks and buttons; stick turn
  via the aim loop (smooth/snap); left stick click recenters; controller_aim decouples
  the weapon from the head; WMR, Touch and Index binding profiles work. Touch Pro, Touch
  Plus, Pico, Cosmos and HP profiles fail with -22 because their extensions are not
  enabled.
- Next: enable the matching extensions when available before suggesting those profiles;
  haptics on fire (needs S2 or a draw-call heuristic); gesture reload; two-handed grip.

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
  `scripts/run-dayz-direct.sh --sim`, `scripts/local-server.sh`, `scripts/
  ghidra-decompile.sh`, `scripts/dayz-vr-ctl.py` (watch shows aimerr/gain).
- Next: a `calibrate` subcommand in dayz-vr-ctl.py that runs the yaw-ratio measurement
  (currently a scratch script); automated regression run (launch, join, measure, stop).
