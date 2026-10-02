# DayZ-VR second audit — 2026-10-02 (commits `419d7c9..02f0c10`)

## Scope and provenance

Range: the 24 commits after the first audit's last reviewed revision, `419d7c9`, up to
`02f0c10` on `fix/dayz-1.29.163709-proton`. New in this range: controller steering wheel,
trigger pedals, in-car buttons and dashboard, motion melee, physical stance, firing
haptics, `[vehicle] lock_view`, `[stereo] aim_residual_render`, the debug plugin stop
fix, `regression-run.sh`, and the FEATURES/TODO inventories. Line references are at
`02f0c10`; use `git show 02f0c10:<path>` if later fixes move them.

This file was started by the Codex Desktop review (session `01a0f3e6`, 14:37-14:42 on
2026-10-02). That review ran the full gate (pass), split the code reading over three
sub-agents (`audit_render_input`, `audit_enforce_vehicle`, `audit_tools_tests`) and
announced three integration findings plus a shot-detector reproduction, then every
agent hit the Codex usage limit before `audit2.md` was written. The sub-agent payloads
in the transcripts are encrypted, so only the parent's statements and the commands it
ran were recoverable. Claude re-derived each lead from the code and reproduced the
ones that can be reproduced off the game; findings Codex may have had beyond the ones
it announced are lost.

Verification level: static control-flow reading plus host reproductions (marked).
Nothing here was run against DayZ; the sim rig was not started for this audit.

## Findings

Priority as in AUDIT.md: P1 = invalid runtime state or input that reaches the game
unintentionally; P2 = functional defect; P3 = edge case or maintenance gap.

| ID | Priority | Evidence and consequence | Required correction |
| --- | --- | --- | --- |
| V01 | P1 | **Pedals and buttons freeze at their last value when DayZ loses focus.** `openxr_host.cpp:1428-1433`: with `injectInput == false` (window not foreground) `SyncControllerInput` returns after `ReleaseInjectedInput()`, which resets keys, mouse, melee and stance (`:1296-1345`) but **not** the bridge values. `SetVehiclePedals` (`:1510`) and `SetControllerButtons` (`:1542`) sit after that return, so `g_throttle`, `g_brake`, `g_pedalsValid` and the button flags keep their last state. `script_bridge.cpp:124-136` rewrites them into `vr.txt` every interval with a fresh `frame=`, so the Enforce freshness check (`DayZVRBridge.c:143-147`, `DayZVRVehicle.c:35-38`) stays true and `CarScript.OnUpdate` keeps applying throttle/brake (`DayZVRVehicle.c:178-184`). Alt-tab with the right trigger pulled = the car keeps accelerating until focus returns. Only `ReleaseControllerKeys` (`:1107-1117`), used on tracking loss and shutdown, clears them. | Clear steer, pedals and buttons in `ReleaseInjectedInput` (or call the three bridge setters with `valid=false` on the `!injectInput` path) and add the case to the focus regression (`tests/` has a focus/tracking test from the first audit's X01 fix to extend). |
| V02 | P2 | **Steering is published regardless of focus.** `PublishVehicleSteering()` (`:1423`) runs before the focus gate on purpose (poses stay live while unfocused, commit `5292775`), so hands on the wheel steer the car while the desktop has another window in front. Defensible for a headset user (the game is still visible in the HMD), but it contradicts the rule stated at `:1430-1431` ("nothing may reach the game while its window is not the desktop foreground") and differs from keyboard/mouse injection. | Decide and document: either gate steering on `injectInput` like the pedals, or state in the comment and README that bridge inputs are exempt from the focus rule. |
| H01 | P2 | **Full-auto fire loses about half of its haptic pulses.** `shot_detector.cpp:28-29` counts a shot only when the total drops by exactly one between consecutive bridge samples; a drop of two is treated as an unload. The bridge samples at 10 Hz (`DayZVRBridge.c:10`, `DAYZVR_INTERVAL = 0.1`), and an M4A1 fires ~13 rounds/s, so every third sample drops by two. **Reproduced** (host build, `scratchpad/shot_repro.cpp`): 20 rounds fired in a burst → 10 shots counted. The unit test (`tests/shot_detector_test.cpp:33`) encodes the two-round drop as "unload", which is the wrong model for automatic weapons. | Treat drops of 1..N (N ≈ max rounds per bridge interval, 2-3) on the same weapon as shots and pulse once per round or once per sample; keep "reload adds rounds" and the weapon-swap reset. Magazine removal already reports `ammo=-1` (`DayZVRBridge.c:88`), so a drop to 0 with the chamber kept is not a removal and the existing "unload" guard protects nothing real. |
| H02 | P3 | **Signed overflow in the shot detector on hostile bridge input.** `shot_detector.cpp:9`: `ammo + 1` with `ammo = INT_MAX`. `game.txt` is parsed with `atoi` (`script_bridge.cpp:173-175`) and written by the mod, so the value is trusted today, but the first audit's S01 (snapshot validation) is still open. **Reproduced** with UBSan: `signed integer overflow: 1 + 2147483647`. | Clamp `ammo` to a sane range at the parse boundary (0..10000) and in `Total`. |
| H03 | P3 | **`hapticPulses_` and the haptic action are touched from two threads.** `TestHaptic` (`:1234`) runs on the debug plugin's TCP thread (`debug_bridge.cpp:188`); `UpdateFireHaptics` (`:1242`) runs on the render thread. Both call `PulseHaptic`, which increments the plain `unsigned hapticPulses_` (`:1230`). OpenXR calls are thread-safe per spec; the counter is a data race (benign in practice, logged value only). | Fold into the first audit's C02 (debug-thread ownership): make the counter atomic or route `TestHaptic` through the frame-thread command handoff. |
| T01 | P2 | **A regression step reports PASS when its log cannot be written.** `regression-run.sh:54`: `"$@" 2>&1 \| tee "$run_dir/$name.log" \|\| status=${PIPESTATUS[0]}`. When `tee` fails (directory missing, disk full) the pipeline fails and the fallback takes the *command's* status, which is 0, so the step is PASS with no log file. **Reproduced** with `run_dir=/proc/nonexistent-dir`: `tee: No such file or directory`, summary `PASS demo`, `failed=0`. | Check `${PIPESTATUS[1]}` too (or `status=$(( ${PIPESTATUS[0]} \|\| ${PIPESTATUS[1]} ))` semantics), and fail the step when the log is missing; verify `$run_dir` is writable before the first step. |
| T02 | P2 | **`calibrate` can pass with the aim loop inactive.** `dayz-vr-ctl.py:148-157`: the result is `settled = abs(yaw_error) < 3 and abs(pitch_error) < 3`; with `stereo.controller_aim=1` (the regression rig's configuration, `regression-run.sh:110-112`) that alone is the verdict. `dayz_runtime_probe.cpp:2540-2548` zeroes both errors whenever the game window is not foreground or the GUI cursor is showing, and the snapshot's `hmd_valid`, `camera_directions_valid`, `window_focused` and `hooks_active` fields (`protocol.hpp:187-220`) are only printed, never required. A client that lost focus, has no HMD pose, or never calibrated its camera basis therefore calibrates "camera follows the controller". | Require `window_focused`, `hmd_valid` and `camera_directions_valid` in the `after` snapshot (and `right_hand` validity when controller aim is on) before declaring success; print which precondition failed. |
| C01 | P3 | **`lock_view` unlock recenters yaw only.** `dayz_runtime_probe.cpp:2443-2456`: leaving a vehicle (`g_aimYawWasLocked && !lockYaw`) recaptures the yaw centres and resets the loop; the pitch target is absolute, so no pitch centre is needed, but the lock state is read from the bridge (`GetGameState().inVehicle`, up to one bridge interval plus `interval_frames` late). For up to ~0.2 s after entering a car the loop still chases the world-space head direction while the camera is already vehicle-relative. Sim-verified as "error 0.0 in the car" only in steady state. | Acceptable as is; note the latency in TASKS V2 and re-check on a real headset when turning while entering. |
| C02 | P3 | **Residual render uses last frame's error by construction.** `dayz_runtime_probe.cpp:1149-1152` adds `g_aimYawError`/`g_aimPitchError`, written by `UpdateClosedLoopAim` (`:2494`) from the camera basis of the frame being rendered, so the residual matches the camera that this frame draws. Correct for the normal path; while the loop is suspended (`:2540-2548`) the error is zeroed, so the render falls back to native. No defect found; recorded because Codex flagged "render timing" as open and the sim evidence (`render − native == reported error`) does not distinguish same-frame from one-frame-late. | None required. A headset test of the hand judder (TODO §1) remains the real check. |
| M01 | P3 | **Motion melee hold times are not derived from DayZ's input.** `openxr_host.cpp:1276-1284`: light swing holds the attack button for 60 ms, heavy for `melee.heavy_hold_seconds` (default in ini). DayZ's `UAMeleeAttack...` distinguishes tap vs hold through the input manager's hold threshold, which the Enforce side could read (`UAInput`-based) instead of a tuned constant. Codex's "check against DayZ's scripts" was not completed. | Default-off feature; keep, but note in TASKS M2 that `heavy_hold_seconds` must be ≥ the engine's hold threshold and verify with a headset. |

## What the gate does not cover

- No regression test exercises the focus gate together with the bridge values (V01); the
  existing focus test only checks keys and mouse.
- `shot_detector_test.cpp` fixes the 1-round-per-sample model into a test (H01), so the
  gate passes while full-auto haptics are wrong.
- `regression-run.sh` has no self-test; T01 and T02 are both "PASS by omission" paths that
  a green summary hides.

## Reproduction record

| Finding | How | Result |
| --- | --- | --- |
| H01, H02 | build-box `g++ -std=c++20 -Wall -Wextra -Wpedantic -Werror -Wshadow -Wconversion -Wsign-conversion -fsanitize=undefined` of `shot_repro.cpp` + `common/shot_detector.cpp` | `full auto: fired 20 rounds, detector counted 10 shots`; `ammo 1->0 with chamber kept: 1 shot(s)`; UBSan `signed integer overflow: 1 + 2147483647` |
| T01 | `step()` from `regression-run.sh:49-62` sourced with `run_dir=/proc/nonexistent-dir`, `step demo true` | `tee: ... No such file or directory` then `PASS  demo`, `failed=0` |
| V01, V02, T02, C01, C02, H03, M01 | code reading only | not run against DayZ |

## Recommended order

1. V01 (one function, clear safety issue) with a focus regression case.
2. T01 + T02 (tooling trust; both cheap).
3. H01 + H02 (detector model and clamp; update the unit test's expectations).
4. V02 decision, H03 as part of C02, C01/M01 notes into TASKS.md.
