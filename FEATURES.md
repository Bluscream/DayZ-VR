# DayZ-VR feature inventory

Everything the mod does today, including the functionality inherited from the original
July 2026 prototype (commits `7866d17`..`e3cbdf2`) and the work added since 2026-10-01 on
`fix/dayz-1.29.163709-proton`. Unfinished or planned work is in [TODO.md](TODO.md); the
living per-track backlog with history is [TASKS.md](TASKS.md).

**Verification columns.** *Origin*: `inherited` = present in the July prototype, `new` =
added October 2026. *Verified*: `headset` = seen on a real HMD (only the inherited work
has this, via the showcase video), `sim` = verified on the headless Monado rig with the
local server, `tests` = covered by the native/Python test gate, `untested` = compiles and
deploys but no live evidence. Most new work is `sim`-verified and still waits for a
headset session (see TODO.md).

> **Rendering-only build since 2026-10-03.** Sections 4 (controller input), 5 (script
> bridge and the `@DayZVR` client mod), the ammo quad, haptics, melee, stance, steering,
> comfort vignette, hotkeys and the HMD-to-game aim path are **parked**: moved to
> [`parked/`](parked/README.md), not compiled, not deployed and removed from the ini and
> schema. The entries below describe them as they were; the HMD rotation is applied
> render-side on all axes meanwhile, so the body and weapon no longer follow the head.

Config keys refer to `dayz_openxr.ini`. "Live tunable" means the value can be changed
while the game runs through the debug plugin (`scripts/dayz-vr-ctl.py set <section.key>`)
or a hotkey toggle.

## 1. Loading and runtime

| Feature | Details | Config | Origin | Verified |
| --- | --- | --- | --- | --- |
| Local DXGI proxy | `dxgi.dll` beside `DayZ_x64.exe` forwards the factory exports to the system library and hooks swap-chain creation, `Present`, `Present1` and `ResizeBuffers`. | `[hooks]` | inherited | headset, sim |
| OpenXR session via D3D11 | Rendered frames are submitted to the active OpenXR runtime (SteamVR, WiVRn, Monado) as a two-view projection layer with the headset's native FOV. | `[openxr] enabled` (LOCAL reference space, no validation layer; the former `reference_space`/`debug_layer` keys were never read and are gone) | inherited | headset, sim |
| Build-identity check | Hooks install only when the executable's PE timestamp and `SizeOfImage` match a known profile; a mismatched DayZ build leaves rendering untouched. Profiles: `DayZ_x64.exe` 1.29.163709 and `DayZDiag_x64.exe` (signature-relocated, experimental). | `[stereo] runtime_probe` | inherited (profile `new`) | sim, tests |
| Forwarding-only mode | `[openxr] enabled=false` loads the proxy without changing rendering, for a safe first test. | `[openxr] enabled` | inherited | headset |
| Keep full frame rate when unfocused | DayZ throttles to ~20 fps when its window is in the background; `keep_focus` makes the engine see its window as always active. | `[hooks] keep_focus` | new | sim |
| Test-present passthrough | `DXGI_PRESENT_TEST` presents are forwarded without advancing an XR frame. | – | new (audit R04) | tests |
| Swapchain image ownership | Acquire/wait/release results are tracked per image; a projection layer is only submitted when both eyes rendered. | – | new (audit X03–X07) | tests |
| Tracking-loss handling | Pose validity bits are honoured, poses are published coherently, held controller input is released at focus/session loss. Controller *tracking* continues while the desktop window is unfocused; only key/mouse injection stops. | – | new (audit X01/X02) | sim, tests |
| Engine crash guard | Skips a single `executeView` call whose prepared-view pointer DayZ already cleared (crash at DayZ+0x1DDE4B after dragging the window) and logs it. | `[patches] guard_execute_without_prepared_view` | new | untested (crash not reproduced headless) |
| Crash reporter | Vectored exception handler writes the fault, a DayZ+RVA backtrace and the recent `ResizeBuffers` calls to `dayz_openxr.log`. Best-effort diagnostics. | – | new | sim (no crash yet) |
| Logging | Log file `dayz_openxr.log` beside the DLL, fresh per launch (previous kept as `.1`); the level is fixed at the build's verbosity, the former `[logging]` ini section was never read. | – | inherited | headset, sim |
| Standalone OpenXR probe | `xr_probe.exe` verifies runtime and headset with a diagnostic grid before touching the game. | – | inherited | headset |

## 2. Stereo and camera

| Feature | Details | Config | Origin | Verified |
| --- | --- | --- | --- | --- |
| Alternate-eye presentation | One eye per game frame, each eye at half the frame rate. This is the default and the only mode that shows a usable image. **The image is mono with head rotation**: DayZ ignores the camera translation the mod writes, so eye separation and positional tracking never reach the renderer. | `[stereo] stereo_mode=alternate`, `alternate_eye`, `camera_separation`, `image_shift` | inherited | headset, sim |
| Double world render (experimental) | Calls DayZ's world render twice per frame with the eye toggled and re-dispatches the projection per eye. Executes without crashing but both passes draw the same view (see TODO R1). Off by default. | `[stereo] stereo_mode=double`, `double_capture_clear` | new | sim (negative result) |
| HMD rotation on the camera | Head yaw/pitch/roll applied to DayZ's camera during view preparation. | `[stereo] hmd_rotation` | inherited | headset, sim |
| Native HMD aim | Head yaw/pitch are routed through DayZ's mouse camera so the aim ray, shots, body and weapon match the rendered direction; roll stays render-only. | `[stereo] hmd_native_aim`, `hmd_mouse_yaw_scale`, `hmd_mouse_pitch_scale` | inherited | headset, sim |
| Closed-loop head aim | Each frame the native camera direction is compared with the HMD and only the counts needed to close the gap are injected, learning counts-per-radian online (DayZ scales mouse deltas by frame time, so a fixed scale only fits one frame rate). Damping and per-frame cap. Pending corrections are suspended while game input is blocked (inventory). | `[stereo] hmd_aim_closed_loop`, `hmd_aim_loop_damping`, `hmd_aim_loop_max_counts` | new | sim (0.1° steady error), tests |
| Residual render | The loop's remaining yaw/pitch error is applied on the render side, so the eyes sit exactly on the head while DayZ's camera catches up a frame later. Fixes the "hands shake while the game is focused" judder. Live tunable. | `[stereo] aim_residual_render` | new | sim (render−native = error) |
| Axis locks | `lock_yaw` / `lock_pitch` keep an axis render-only (view follows the head, aim does not). Live tunables, default hotkeys F10/F11. | `[stereo] lock_yaw`, `lock_pitch` | new | sim |
| Controller aim | DayZ's camera and weapon follow the right controller's aim pose while the eyes show the head direction; falls back to head aim when untracked. Live tunable, default hotkey F12. | `[stereo] controller_aim` | new | sim |
| Positional tracking input | Physical HMD translation is written to the camera with a scale. **Has no visible effect** on this DayZ build (see above). | `[stereo] hmd_position_scale` | inherited | sim (no effect) |
| Gameplay FOV override | Absolute FOV in radians replaces the loaded profile `fov` in memory only; `.DayZProfile` is never written. | `[stereo] game_fov` | inherited | headset, sim |
| Image fitting | `contain` / `stretch` / `cover` plus independent scale factors for the final eye image. | `[stereo] fit_mode`, `scale_x`, `scale_y` | inherited | headset |
| Square render-resolution override | Optional backbuffer override to a square resolution. | `[stereo] override_game_resolution`, `render_width`, `render_height` | inherited | headset |
| Recenter | Recaptures the HMD yaw and position centre. Hotkey, left stick click, debug command. | `[hotkeys] recenter` (F9), `[controls] recenter_stick_click` | new | sim |
| Vehicle view lock | While the bridge reports the player in a vehicle, both head axes are render-only so the cabin view follows the head without the aim loop fighting the car's rotation. Live tunable. | `[vehicle] lock_view` | new | sim (error exactly 0 in car) |
| Locomotion vignette | Darkens the periphery while moving or smooth-turning with the sticks; snap turns never vignette. Strength, radius and fade time configurable. | `[comfort] vignette*` | new | sim (shader path), untested look |
| Eye capture dump | `dayz-vr-ctl.py dump-eyes` writes both eye images as BMP from the render thread. | – | new | sim |

## 3. GUI, HUD and cursor

| Feature | Details | Config | Origin | Verified |
| --- | --- | --- | --- | --- |
| World-locked GUI quad | Main menu and inventory are captured separately and submitted as an OpenXR quad anchored in front of the HMD when cursor mode opens; resolution, width, distance and vertical offset configurable. | `[gui] quad_*` | inherited | headset, sim |
| Virtual GUI cursor | System cursor locked to the window centre; mouse and controller ray move a virtual cursor in DayZ GUI space, drawn into the VR-visible layer, aligned across resizes. | `[stereo] gui_mouse_remap`, `gui_cursor` | inherited | headset, sim |
| GUI capture resize | Capture resources are refreshed when the primary window shrinks or grows. | – | new (audit R03) | tests |
| Inventory HMD look | Head look stays active while DayZ owns the mouse for the inventory. | `[gui] inventory_hmd_look` | inherited | headset |
| Inventory character preview | Preview rotation compensation, preview show/hide, optional suppression of the Gauss blur behind the inventory. | `[gui] inventory_preview_rotation_scale`, `inventory_player_preview_visible`, `inventory_blur_enabled` | inherited | headset |
| HUD scale and safe area | Native IGUIScale override, safe-area width/height, experimental centred composite viewport and per-eye offsets. | `[stereo] override_hud_scale`, `hud_scale`, `hud_safe_*`, `hud_composite_*`, `hud_*_offset_x` | inherited | headset |
| Controller-anchored ammo quad | Native world-space seven-segment display ("30+1") above the right grip, fed by the bridge; amber below six rounds, red when empty; width, offsets, tilt and pixel height are live tunables. Internal-magazine weapons show the cartridge count. | `[hud] ammo_quad*` | new | sim ("60+1", "3+1" amber, Mosin "4+1"), tests |
| Screen-space ammo label | Enforce-drawn magazine count next to the aim point, hidden when lowered, in menus or with the HUD off. Fallback for the quad. | `[bridge] ammo_counter` | new | sim |
| Vehicle dashboard line | Enforce-drawn text at the bottom of the HUD while driving: speed, gear (R/N/n), rpm or "engine off" (amber), fuel %, lights. Hidden in menus and the inventory. | `[bridge] dashboard` | new | sim (eye dump) |
| Diagnostic rays and axes | Controller XYZ axes, GUI pointer ray (shown only while menu/inventory is visible), gameplay direction rays (HMD white, aim blue, native camera yellow, render camera magenta). | `[controls] show_*`, `*_ray_length`, `*_ray_thickness` | inherited (direction rays `new`) | headset, sim |

## 4. Controller input

| Feature | Details | Config | Origin | Verified |
| --- | --- | --- | --- | --- |
| Direct action input | The proxy hooks DayZ's `Input` interface getters (value/press/release/hold/hold-begin by id and by record, the aim axis pair, the player input controller update; 1.29.163709 RVAs build-checked) and combines the controller state with the engine's reading: analogue `UAMove*`, `UAStance`, `UAReloadMagazine`, `UADefaultAction`, `UATurbo`, `UAGetOver`, `UAFire`, `UATempRaiseWeapon`, `UAItem0..9`. Works without desktop focus, keyboard stays usable, no server mod. Optional direct head/stick aim through the axis pair (exact per-frame angles, no closed loop). | `[input] direct_actions`, `direct_aim`, `aim_yaw_sign`, `aim_pitch_sign` | new | tests (override table), build, sim (walks unfocused through `action UAMoveForward 1`); headset pending |
| Button and stick mapping | Left stick WASD, right stick turn, X=C, Y=R, LGRAB+X=Esc, LGRAB+Y=Tab, LGRAB+A/B=previous/next quickbar slot (mod-maintained cycle), RGRAB=F (not while driving: F is "Get out"), A=Shift, B=Space, triggers=right/left mouse. Emulated input mirrors the controller state and needs window focus (TASKS I1). | `[controls] enabled`, `deadzone` | inherited | headset, sim |
| Interaction profiles | Simple, Oculus Touch, Valve Index, Windows Mixed Reality, Vive Cosmos, Touch Pro, Touch Plus, Pico, HP Reverb bindings; vendor extensions are enabled only when the runtime exposes them. | – | inherited (Touch/Index), new (others) | sim (all eight bind on Monado) |
| Stick turning through the aim loop | Smooth turn in degrees per second or snap turn in degrees per flick rotates the loop's yaw target; open-loop `turn_scale` fallback. | `[controls] turn_rate`, `snap_turn`, `turn_scale` | new | sim |
| Hotkeys | Keyboard hotkeys with modifier chains, polled while DayZ is foreground: recenter plus up to eight boolean-tunable toggles. | `[hotkeys]` | new | sim |
| Haptics on fire | Vibration action bound on every profile; the bridge's magazine+chamber readback is turned into shot events (drop of exactly one, same weapon, no bridge gap) and pulses the right controller. Live tunables, `dayz-vr-ctl.py haptic` test pulse. Detection lags the shot by up to one bridge interval (100 ms). | `[haptics] fire`, `fire_seconds`, `fire_amplitude` | new | headset (pulse per single shot, M4A1), sim, tests |
| Motion melee (default off) | Right-controller swings tap the attack button (light) or hold it (heavy) while fists or a melee weapon are in hands; one event per swing with cooldown. | `[melee] motion_swing`, `light_speed`, `heavy_speed`, `cooldown_seconds`, `heavy_hold_seconds` | new | sim (negative path only), tests |
| Physical crouch/prone (default off) | Head drop below the standing height taps crouch/prone toggles until the bridge reports the matching stance; hysteresis; off in GUI, inventory, vehicle. Standing height recaptured on recenter. | `[stance] physical`, `crouch_drop`, `prone_drop`, `hysteresis` | new | headset (crouch), sim, tests |
| Two-hand steering wheel | The line between the grips is the rim; its tilt becomes `Car.SetSteering` through the bridge while the local player drives; deadzone, max angle, invert, optional both-grips requirement. Keyboard steering keeps working when hands are not both tracked. | `[vehicle] steering`, `wheel_max_degrees`, `deadzone`, `invert`, `require_grip` | new | headset (steers, janky), sim, tests |
| Trigger pedals | While in a vehicle the triggers publish throttle/brake instead of mouse buttons; the mod applies them only while pressed so W/S keep working. | (part of `[vehicle] steering`) | new | headset (in 1st gear; in N it rolls backwards), sim |
| In-car buttons | A = engine start/stop, left grip + A = headlights, right stick click = horn, through the vanilla action manager (server executes). B stays handbrake. Native A/hotbar chords are suppressed while driving. | – | new | sim (engine, lights readback) |
| Raw controller state for mods | vr.txt carries `btn_a/b/x/y`, stick clicks, grips, triggers so Enforce features can react to buttons. | `[bridge]` | new | sim |

## 5. Script bridge and Enforce mods

| Feature | Details | Config | Origin | Verified |
| --- | --- | --- | --- | --- |
| Native ↔ Enforce file bridge | The proxy writes `$profile:dayzvr\vr.txt` (HMD pose, right aim pose, aim errors, GUI cursor state, HUD rectangle, steer/pedals, buttons, dashboard flag) every N frames and reads `game.txt`. | `[bridge] enabled`, `interval_frames` | new | sim |
| `@DayZVR` client mod | Writes `game.txt` at 10 Hz: weapon class, ammo, chamber, melee flag, health level, bleeding bits, stamina, inventory open, stance, raised, vehicle state (steering, speed, driver, gear, rpm, engine, fuel, lights), dashboard text. Reads vr.txt into a map and pushes values to statics for 4_World consumers. | `-mod=@DayZVR` | new | sim |
| Client command hook | `$profile:dayzvr/client_cmd.txt` verbs: `raise`, `fire`, `enter`, `exit`, `engine`, `lights`, `horn`, `steer <v>|off`, `print`. Limits: 200 characters, 8 words. | – | new | sim |
| `@DayZVR_Server` test mod | Unauthenticated command channel for the isolated local server: `hands <class> [mag|-] [rounds]`, `give <class> [n≤50]`, `spawn <vehicle>`, `tp <preset|x z>`, `tpto [dx dy dz]`, `enter`, `exit`, `info`, `heal`, `time`, `weather`, `kill`. Class names checked against CfgVehicles/CfgWeapons/CfgMagazines/CfgAmmo; `spawn` creates with `ECE_SETUP` so vehicles have collision. | – | new | headset (hands, give, spawn), sim |
| PBO tooling | `scripts/build-pbo.py` packs both mods (path validation, LZSS), `scripts/unpack-pbo.py` unpacks any PBO. | – | new | tests |

## 6. Runtime control and diagnostics

| Feature | Details | Config | Origin | Verified |
| --- | --- | --- | --- | --- |
| Debug plugin | Optional `dayz_openxr_debug.dll`, loopback TCP, one command per line / one JSON line back: `get`, `tunables`, `set`, `recenter`, `haptic`, `dump-eyes`, `ping`. Serialised start/stop lifecycle. | `[debug] enabled`, `port`, `plugin` | new | sim, tests (real DLL under Proton) |
| Live tunables | Render-path keys (`stereo.*`, `gui.*`) plus 23 host keys (`hud.*`, `melee.*`, `vehicle.*`, `stance.*`, `haptics.*`) from one table with default, range and type; `static_assert` keeps table and storage in step. | – | new | sim (listed and range-checked) |
| Host CLI | `scripts/dayz-vr-ctl.py` `watch`, `get`, `tunables`, `set`, `recenter`, `haptic`, `dump-eyes`, `snapshot`, `compare`, `calibrate`. | – | new | sim |
| Ghidra helper | `scripts/ghidra-decompile.sh` headless decompile of RVAs from `DayZ_x64.exe`; decompilations kept under `build/ghidra/`. | – | new | used |

## 7. Build, test rig and tooling (Linux host)

| Feature | Details | Origin | Verified |
| --- | --- | --- | --- |
| Cross-build gate | `scripts/build.sh` builds the Windows targets with the cross toolchain, runs the native tests (incl. AddressSanitizer/UndefinedBehaviorSanitizer builds), 25+ Python regressions, shellcheck, the debug-plugin lifecycle test under Proton and executable-profile mutation checks; `--deploy` copies DLLs, ini and both mods into the game (with a backup), `--stop`/`--start` control the game. Visual Studio solution retained for Windows. | new | every commit |
| Native test suite | ammo display, build checks, config numbers, debug protocol and plugin lifecycle, dxgi smoke, GUI capture sizing, aim loop, melee swing, physical stance, present frame, projection replay, render trace, shot detector, stereo state, vehicle steering, XR frame policy, XR swapchain image. | new | gate |
| Headless OpenXR runtime | `scripts/xr-sim.sh` runs Monado with simulated HMD (`SIM_ROTATE`) and controllers (`SIM_CONTROLLERS=simple|wmr|ml2`), window compositor at 75-85 fps. | new | daily |
| Isolated local server | `scripts/local-server.sh` runs a dedicated server in podman, loopback ports only, loads `@DayZVR_Server`, no signature checks. | new | daily |
| Direct Proton launch | `scripts/run-dayz-direct.sh [--sim] -- <args>` launches without the Steam launcher and prunes old crash dumps, logs, screenshots and deploy backups. | new | daily |
| Launch report | `scripts/dayz-status.sh --wait N` blocks until in-world or crashed and prints one non-optional report: exe version, deployed vs built artifacts, ini highlights, processes and windows, screenshot, session state, log summary, crash reasons, script log, bridge files. | new | daily |
| Command wrapper | `scripts/dayz-cmd.sh [--client] <verb> …` sends a command and waits for its echo in the result log. | new | daily |
| One-shot regression | `scripts/regression-run.sh [--skip-build] [--keep] [--no-sim]` runs the whole cycle with per-step PASS/FAIL and full logs under `build/logs/regression-<stamp>/`. | new | 11/11 PASS 2026-10-02 |
| Log tools | `scripts/dayz_log.py` bounded log following and summaries. | new | tests |
| Config editor | `tools/config-editor` (Rust/egui, single binary for Linux and Windows): schema-driven form for every ini key from `dayz_openxr.schema.json` (display names, help, types, ranges, enums, unused/restart/live tags), per-key reset, comment-preserving save, live apply and recenter through the debug plugin. `scripts/build-config-editor.sh` runs fmt/clippy/test/doc and both release builds. | new | Linux desktop, Windows exe under Proton, 14 unit tests + schema parity test |
| Offline modding reference | `.references/docs/dayz-modding/` (symlink, not in the repo): vanilla script sources, GUI layouts, wiki pages, sample repos. | new | used |
