# DayZ-VR open work

What is not done, or only partly done. Each entry names its backlog track in
[TASKS.md](TASKS.md) (`R1`, `M1`, …) or its audit finding in [AUDIT.md](AUDIT.md)
(`C02`, `S01`, …); those files carry the history and the concrete next step. What already
works is listed in [FEATURES.md](FEATURES.md).

Status legend: **partial** = code exists and runs but the feature is incomplete or
unproven, **headset** = implemented and sim-verified, waits for a real headset,
**planned** = researched or designed, no code yet, **idea** = named requirement only.

## 1. Rendering

| Item | Status | Track | Notes |
| --- | --- | --- | --- |
| True per-frame stereo with real parallax | partial | R1, R4 | `stereo_mode=double` runs the world render twice but both passes draw the same view: DayZ builds its draw lists in the scene preparation before the projection dispatch. Needs the per-eye unit in the frame function (`0x8E77C0`) identified and re-run per eye. Current output is **mono with head rotation**. |
| Positional tracking and eye separation reaching the renderer | partial | R1 | Camera translation at FrameBase+0x2C is ignored by the engine; the renderer's real view-origin source is not found yet. `hmd_position_scale` and `camera_separation` have no visible effect. |
| Depth-based reprojected second eye (fallback) | idea | R2 | Only if R1 fails; SuperDepth3D is the reference. |
| Scopes, night vision, flashbang and other full-screen overlays in VR | idea | Q1 | Not yet examined; must look correct per eye once stereo exists. |
| Vignette look, lock_yaw/lock_pitch feel, controller_aim with real tracking | headset | V1 | Shader and logic verified on the sim only. |
| Residual render removes the hand judder on a real HMD | headset | M1, V1 | Sim shows render = native + residual; the perceptual result is unverified. |
| Aim-loop warm-up at headset frame rates | headset | T1, V1 | On the sim the gain estimate swings for 60-90 s after spawn while the world streams in at 25-33 fps. |
| GUI quad behaviour on a real primary-backbuffer downsize | headset | audit R03 | Sizing regression passes; live resize not exercised. |
| UI element sizing follows frame height | partial | – | Inherited limitation: square render resolutions make UI oversized. |

## 2. Motion controls and interaction

| Item | Status | Track | Notes |
| --- | --- | --- | --- |
| In-game hands matching the real controllers (hand models, finger curl, hand tracking) | idea | Q1, M2 | No hand-model or hand-tracking path exists. Controller aim moves the camera/weapon but the drawn weapon does not follow the hand. |
| Weapon rotation following the hand for melee and aiming | planned | M2 | Needs a bone override / IK or a native camera-relative transform patch. |
| Motion melee thresholds | headset | M2 | `light_speed`, `heavy_speed`, `heavy_hold_seconds` untuned; default off. |
| Haptics for melee hits and vehicle collisions | planned | M1, M2 | The bridge has no event for either. |
| Native fire hook for haptics | planned | M1, V2 | Current detection lags by up to one bridge interval (100 ms). |
| Gesture reload, magazine extraction/insertion, bolt/chamber gestures | idea | M3 | Needs validated DayZ actions and weapon reload state. |
| Body holsters and slots for weapons, grenades, magazines; shoulder reach for inventory | idea | M3 | Needs a body pose from HMD + controllers and calibration. |
| Two-handed weapon grip | idea | M1, M3 | – |
| Grabbing door handles, vehicle handles choosing the seat, attachment gestures | idea | M4 | Keyboard interaction remains the fallback. |
| Physical crouch/prone thresholds, seated reference, mantle/climb | headset / planned | M5 | Default off; sim only verified the negative path. Seated play needs `stance.seated` or capture-on-demand. |
| Steering wheel range and pedals | headset | V2 | `wheel_max_degrees`/`deadzone` untuned; whether `SetThrottle` beats the engine input like steering did is unproven (sim triggers idle). |
| Horn audible, headlights visible on a real client | headset | V2 | Server executes the actions (readback verified); client-side effect unseen. |
| Seated recenter / view locked to vehicle yaw | planned | V2 | `lock_view` exists; a seated height reference does not. |
| Quickbar cycle resync with physical keyboard | partial | – | Inherited: pressing a number key does not update the plugin's cycle position. |
| Hands visibly on the wheel | idea | V2 | Depends on the hand-model track. |

## 3. UI

| Item | Status | Track | Notes |
| --- | --- | --- | --- |
| In-game settings menu for every plugin setting | planned | U2 | Decision made: reuse DayZ's own UI from the Enforce mod (options built from `.layout` widgets, values through the bridge). Not started. |
| Every ini key as a runtime tunable with a "needs restart" flag | partial | U2 | 23 host keys and the render-path keys are live; keys consumed at hook installation (`runtime_probe`, HUD safe area, resolution override) still need a restart, and the render-path table is separate from the host table. |
| Persisting runtime changes back to the ini | planned | U2 | Values set live are lost on exit. The desktop config editor (`tools/config-editor`) covers edit + apply live + save outside the game; in-game persistence is still open. |
| Wrist-anchored dashboard | planned | V2, S2 | Data path and screen-space line exist; a native quad or left-grip projection is next. |
| Ammo quad placement with real controllers | headset | U1 | Sim grips sit at the frame edge, so the quad clips there. |
| Ammo quad red state | partial | U1 | Red is only reachable with ammo=0 and no chamber; unverified. |
| Immersive HUD elements beyond ammo and dashboard | idea | U1 | Health, stamina, bleeding are already in game.txt. |
| Mod Settings tab and keybind rows inside DayZ's own Options/Controls menus | planned | L1 | Needs native widget/input-action injection (reverse engineering) or a host-drawn overlay styled after the game menu; overlay first. |
| Hotkey press-to-bind editing in the overlay, persisted to `dayz_pluginloader.ini` | planned | L1 | Bindings are ini-only today. |
| In-game drop-down console (default `^`) with mod-registered commands and variables | planned | L1 | Settings become console variables, hotkey actions become commands; shares the command table with the debug plugin protocol. |

## 4. Scripting and bridge

| Item | Status | Track | Notes |
| --- | --- | --- | --- |
| Lua scripting layer | planned | S1 | Research done (UEVR's Lua 5.4 + sol2 template). Nothing vendored or exposed. |
| Bridge snapshot validation and freshness | partial | audit S01 | Partial or malformed `game.txt` is still accepted and stale values persist. |
| Quoted `-profiles=` argument parsing on the native side | partial | audit | Fully quoted arguments are mis-parsed. |
| Signed mods (`.bikey`/`.bisign`) | planned | S2 | Required for servers with `verifySignatures`; the local server runs without. |
| Native hook into the script VM instead of files | idea | S2 | Would remove the 10 Hz file polling. |
| `give` into a specific inventory slot | planned | G1 | – |
| Spawned vehicles survive reconnect | partial | G1 | Not CE-registered, deleted on reconnect. |

## 5. Code health and audit debt

| Item | Status | Track | Notes |
| --- | --- | --- | --- |
| Split `common/dayz_runtime_probe.cpp` (3780 lines) | planned | H1 | GUI cursor module first, HUD layout block second; both need a GUI-cursor path (menu open) as the test gate. |
| Debug-thread ownership of game/render globals | partial | audit C02 | Tunable writes and recenter from the TCP thread mutate hook globals; needs a command handoff design. |
| Partial-init leaks and incomplete shutdown reset | open | audit X11 | – |
| Nonfinite values in comfort shader constants | open | audit X09 | Ini parsing was hardened (`4c1a6f9`); the finding is not re-verified closed. |
| Visual Studio project source parity | open | audit T04 | `.vcxproj` lists lag the CMake build; the Linux cross-build is the maintained path. |
| Protocol string bounds and JSON validity | open | audit P01 | Partly addressed in `5908799`; not closed. |
| Hotkey parser accepts `F1x` as F1 | open | audit H01 | – |
| Render trace buffer integration | partial | audit | Synchronized buffer prototype tested (`be6c351`) but not wired into the runtime; the current diagnostic arrays can race. |
| Sim script ownership of `monado-service` | partial | audit | `xr-sim.sh stop` finds all Monado processes instead of its own PID. |
| Mapped-PE fixture freshness check | open | audit | A passing old fixture is no evidence for a new game version. |
| CTest registration, enforced size limits, stricter Python typing | open | audit | Gate runs tests by script, not through CTest. |
| Window-drag crash root cause | partial | C1 | Guard patch deployed; the crash never reproduced headless. |
| Offset update workflow for the next DayZ build | planned | – | Profiles and signatures are per build; a new patch breaks the hooks until offsets are refreshed. |
| Every `dayz_openxr.schema.json` key registered through the loader's settings API (one table for ini, debug protocol, editor, UI) | planned | L1, U2 | Phase 1 of the loader ships the API; the VR plugin still reads its ini directly. |
| Debug plugin bridge hosted by the loader instead of the VR plugin | planned | L1 | So every plugin's settings reach `dayz-vr-ctl.py`. |
| Toggle VR v2: end/re-create the OpenXR session at runtime | planned | L1 | v1 only stops HMD camera rotation and eye alternation. |

## 6. Test rig

| Item | Status | Track | Notes |
| --- | --- | --- | --- |
| `regression-run.sh --no-sim` on the real headset path | headset | T1 | Flag exists, never run. |
| Timer-driven nightly regression | planned | T1 | After the headset path works. |
| Scripted controller motion on the sim | idea | T1, M2 | Static sim controllers only verify negative paths for melee, stance, pedals and wheel magnitude. |
| GUI-cursor path on the sim (menu open) | planned | H1, T1 | Needed as the gate for the probe carve-outs. |

## 7. Headset verification backlog (everything since 2026-10-01)

First headset session done 2026-10-02 (TASKS.md V3): verified alternate-eye stereo, GUI
quad, haptics per shot, ammo label, steering, pedals, physical crouch. Open from it: input
only with window focus (TASKS I1 direct input), closed-loop aim unusable on the HMD (open
loop -180/-135 smooth but the view trails), stick turn dead with lock_yaw, WiVRn needs
negative eye separation, direction rays default on, GUI quad not mirrored to the desktop,
inventory icons missing on the quad, inventory pitch inverted, car: auto-recenter on enter,
ShiftTo from N, right-stick shifting, horn/lights no visible effect, arms following the
controllers instead of the body animation, B/A jump mapping.

Listed once here; every row above marked **headset** belongs to it. Also: GE-Proton 11-7
compatibility, hotkeys F9-F12 on the real keyboard, stick-click recenter, drag-crash guard
under WiVRn, the per-frame `xr_ms` wait breakdown on WiVRn.

## 8. VR feature checklist (what native VR games and good VR mods have)

The common feature set of native VR games and of the three tiers of VR plugin (injector:
stereo + head tracking + gamepad; conversion: motion-controller aim, HUD placement, camera
fixes, comfort; full conversion: tracked hands, physical weapons, manual interactions,
VR UI), checked against this mod. **have** = in FEATURES.md, **partial** = exists with a
gap listed above, **–** = not started. DayZ's engine limits (Enforce has no bone access,
no sockets, no FFI; the renderer builds its draw lists before our projection hook) decide
which rows are realistic; "idea" rows above are the ones we have already decided to try.

### Rendering and tracking (injector tier)

| Feature | Status | Where |
| --- | --- | --- |
| 6DoF head tracking (rotation) | have | closed-loop head aim, lock axes |
| 6DoF head tracking (position) | partial | translation ignored by the engine, R1 |
| Stereo rendering with real parallax | partial | mono + head rotation, R1/R4 |
| OpenXR runtime support (Quest, Index, Vive, WMR, WiVRn) | have | all interaction profiles bound |
| Reprojection / motion smoothing compatibility | – | untested, depends on R1 |
| Adjustable world scale | – | would follow positional tracking |
| Camera offsets (forward/back/up/down) | partial | `hmd_position_scale` has no effect, R1 |
| FOV fix | have | `game_fov`, fit modes, hud safe area |
| Camera-effect removal (head bob, shake, forced zoom) | – | DayZ's camera shake and ADS zoom untouched |
| Scopes, NVG, overlays per eye | – | Q1 |
| VR render settings (supersampling, resolution scale) | partial | `override_game_resolution`, no per-eye scaling |
| Performance: render once per eye only when needed | partial | `stereo_mode=double` renders twice for one view |
| Optional gamepad/keyboard play with head tracking | have | controller input can be disabled |

### Controls and comfort (conversion tier)

| Feature | Status | Where |
| --- | --- | --- |
| Decoupled aiming (head = camera, right hand = gun) | partial | `controller_aim` moves camera+weapon together, M2 |
| Independent head and weapon aim | – | needs the weapon bone, M2 |
| Smooth locomotion (left stick) | have | WASD injection |
| Smooth turning / snap turning with angle | have | `turn_rate`, `snap_turn` |
| Head- vs hand-relative movement direction | – | always camera-relative (engine) |
| Teleport locomotion | – | not planned (DayZ is not a teleport game) |
| Room-scale walking | – | positional tracking first, R1 |
| Physical crouch / prone | have (default off) | `stance.*`, M5 |
| Leaning | – | Q/E keys not mapped to head lean |
| Sprint, jump, swim, climb from gestures | – | buttons only |
| Seated mode, height calibration | partial | `recenter`, no seated reference, M5 |
| Movement vignette / tunnelling | have | `comfort.*` |
| Dominant-hand selection | – | right hand hard-coded |
| Recenter view (hotkey, stick click, CLI) | have | – |
| Disable artificial head movement (vehicle-relative camera) | have | `vehicle.lock_view` |
| Controller remapping | – | fixed mapping, keyboard remains |
| Weapon stabilisation / smoothing | – | aim-loop damping only |
| Grab toggle instead of hold | – | wheel grip is hold-only |

### Hands, weapons and interactions (full-conversion tier)

| Feature | Status | Where |
| --- | --- | --- |
| Floating hands / hand models / finger tracking | – | Q1, M2 |
| Tracked weapon following the hand | – | M2 |
| Two-handed weapon grip | – | M1/M3 |
| Guns with manual reloading, magazine gestures | – | M3 |
| Melee from swings | have (default off) | `melee.*`, M2 |
| Blocking / parrying | – | – |
| Throwing (grenades, items) | – | – |
| Grab, pick up, switch hands, inspect | – | M4 |
| Doors, drawers, handles, vehicle entry by grab | – | M4 |
| Physical buttons / knobs (vehicle controls) | have (buttons), – (knobs) | A/grip+A/stick click in cars |
| Steering wheel and pedals | have | `vehicle.*`, V2 |
| Hip / shoulder / back holsters, virtual pockets | – | M3 |
| Wrist or chest inventory, physical backpack | – | M3 |
| Flashlight / watch / map in hand | – | – |
| Eating / drinking gestures | – | – |
| Items highlight when grabbable, distance grab | – | – |

### UI and feedback

| Feature | Status | Where |
| --- | --- | --- |
| Menus on a world-locked screen with laser pointer | have | GUI quad + GUI ray |
| HUD repositioning into 3D (head, wrist, world) | partial | HUD safe area; ammo quad on the controller; dashboard screen-space, S2/V2 |
| Wrist-mounted menu / dashboard | – | V2, S2 |
| Direct-touch buttons | – | – |
| Depth-aware or controller-attached crosshair | – | – |
| Cutscenes / loading screens on a virtual screen | partial | GUI quad covers menus; loading not examined |
| Haptics on fire | have | `haptics.*`, lags one bridge interval, M1 |
| Haptics on hits, damage, vehicle collisions, interactions | – | M1/M2 |
| Impact-dependent vibration | – | – |
| Spatial audio, directional footsteps | engine | DayZ's own; headset audio through the runtime |
| In-game settings menu for the plugin | – | U2 (config editor exists outside the game) |
| Subtitle / text size options | – | – |
| Accessibility: one-handed mode, button remap, height offset | – | – |

### Multiplayer

| Feature | Status | Where |
| --- | --- | --- |
| Head and hand movement visible to other players | – | needs the hand/weapon bone path, M2 |
| Voice chat with spatial audio | engine | DayZ VON |
| Avatars, gestures | – | – |
| Signed mods for public servers | – | S2 |
