# Parked: everything that is not rendering

Since 2026-10-03 the build is **rendering only**: the dxgi proxy, the OpenXR host (session,
eye swapchains, GUI quad, frame records, pose submission) and the runtime probe's camera
and render hooks. Everything that drives the game or the player from VR hardware was moved
here and is **not compiled, not deployed and not read from the ini**, so that stereo
rendering can be measured and tuned on its own. Nothing here is deleted; git history has
the full files before the cut (commit "refactor: park non-rendering features").

| Parked | Was | Why it is not rendering |
| --- | --- | --- |
| `common/dayz_input_hooks.*`, `common/input_actions.*` | engine input-action getter hooks, controller to action mapping | input |
| `common/hmd_aim_loop.*` and `common/dayz_runtime_probe_aim.cpp` (excerpt) | HMD yaw/pitch to game aim: synthetic mouse, closed loop, direct aim, axis locks, vehicle view lock | input; the HMD rotation is now applied render-side on all three axes |
| `common/openxr_host_controls.cpp` (excerpt) | OpenXR action set, controller poses, buttons, sticks, haptics, axis/ray layers, ammo quad, motion melee, physical stance, two-hand steering | controls and HUD |
| `common/ammo_display.*`, `common/shot_detector.*`, `common/melee_swing.*`, `common/physical_stance.*`, `common/vehicle_steering.*` | the pure logic behind the above | controls and HUD |
| `common/script_bridge.*`, `enforce/DayZVR/` | Enforce client mod and the file bridge (poses, ammo, vehicle, dashboard) | gameplay |
| `common/comfort.*` | movement vignette | comfort (needs controller movement state) |
| `common/dayz_hotkeys.*` | keyboard hotkeys (recenter, tunable toggles) | input; the debug plugin's `recenter` command covers the need |
| `scripts/ini-preset.sh` | flipped the now removed ini keys | obsolete while parked |
| `tests/*_test.cpp` | unit tests of the parked logic | follow their modules |
| `dayz_openxr.parked.ini`, `dayz_openxr.schema.parked.json` | the removed ini sections/keys and their schema entries | restore together with the modules |

What stayed on purpose: the server test-command mod (`enforce/DayZVR_Server`, teleport and
spawn for the rig), the debug plugin and bridge (state, tunables, `recenter`, `dump_eyes`),
the GUI quad and its cursor remap in the probe (rendering of menus), `[hooks] keep_focus`
and the crash guard patches. The debug API struct keeps its layout and version; hand,
aim-loop and direct-input fields read zero.

## Restoring

1. `git mv` the module pair(s) back to `common/` and their tests to `tests/`, add them to
   `CMakeLists.txt` (`vr_common`) and `scripts/build.sh` (`step_test`).
2. Re-insert the excerpts: `openxr_host_controls.cpp` ranges into `common/openxr_host.cpp`
   (members and declarations into the header), `dayz_runtime_probe_aim.cpp` ranges into
   `common/dayz_runtime_probe.cpp`. Each excerpt is labelled with the function it came from.
3. Merge `dayz_openxr.parked.ini` sections back into `dayz_openxr.ini` and the schema
   entries into `dayz_openxr.schema.json` (the schema test requires both to agree), raise
   the row threshold in `tests/test_config_schema.py` again.
4. `enforce/DayZVR` back to `enforce/`, restore the PBO build/deploy lines in
   `scripts/build.sh` and `-mod=@DayZVR` plus the client-cmd/haptic/calibrate steps in
   `scripts/regression-run.sh`.
