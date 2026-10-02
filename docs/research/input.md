# Input system (DayZ 1.29.163709)

Goal of this research: drive the game's input from outside (VR controllers, head tracking)
without `SendInput`, which needs the window to be the OS foreground window and only produces
digital keys. Everything below is decompiled from `DayZ_x64.exe` with Ghidra
(`scripts/ghidra-decompile.sh`, see tooling.md) unless marked verified.

## Layers

```
OS  --WM_INPUT/RawInput--> Input system (class Input)  --events--> action registry  --records--> HumanInputController
    --XInput 1.3 poll---->   device modules                          (UAInput records)         (InputAccess tables)
```

1. **Device layer**: the `Input` object (RTTI name `Input::vftable`) owns device modules
   (`InputModuleKeyboard::vftable` instances at `+0x75A0` and `+0xA480`), raw key states,
   accumulated mouse deltas and an event queue.
2. **Action registry**: a global (`DayZ+0x1009408`, 0x1A0-byte object made by
   `FUN_14052fc20`, constructed lazily by `FUN_140532cd0`) holding one **record** per named
   action (`UAMoveForward`, `UAFire`, ...). Enforce's `UAInput` class *is* a pointer to such a
   record.
3. **Consumers**: gameplay code (HumanInputController, vehicles, GUI) reads records through
   `InputAccess` handles resolved by name hash.

## Imports actually used

| API | IAT slot | Call site | Role |
| --- | --- | --- | --- |
| `GetRawInputData` | `0xC02BE8` | `0x350B60` (window message handler, `WM_INPUT` = `param_3 == 0xFF`) | Keyboard (type 1 → `0x351610`), mouse (type 0 → `0x351740`), HID (type 2 → `0x3511F0`). Only processed when `Input+0xC0 == 1` and `Input+0x8194 != 0`. |
| `RegisterRawInputDevices` | `0xC02BD8` | `0x34FCE0`, `0x34FD40`, `0x34FD90`, `0x351D20` | Device registration. |
| `GetKeyboardState` | `0xC02C18` | `0x34FDF0` | Polled keyboard module: maps 256 VK slots through a table at `Input+0x85D0` into a 256-byte state array (1 = down, 2 = up). |
| `GetAsyncKeyState` | `0xC02C28` | `0x350B60` (VK_RCONTROL), `0x43C700`, `0x712A50` | Modifier checks. |
| `xinput1_3.dll` ordinals 2 (`XInputGetState`), 4 (`XInputGetCapabilities`), 5 (`XInputEnable`) | `0xC02EF0`, `0xC02EE0`, `0xC02EE8` | `0x352010` | Gamepad poll, runs only when `Input.IsEnabledGamepad()` (byte at `Input+0x18418` region, getter `0x5F6CB0`). Connection re-check every 200 ms per slot (`+0x81A0` timer, `+0x81A4` slot cursor, `+0x8195..` connected flags, `+0x81C8` active pad). Button bits are built into `+0x81A8` through `FUN_1403509d0(buttonId, &XINPUT_STATE)`. |
| `hid.dll` `HidP_*` | `0xC02110..` | HID module | Non-XInput pads. |

`DirectInput` is **not** imported.

## Device layer (`Input` object, allocated 0x184D8 bytes by `0x5F5160`, ctor `0x5F4320`)

| Offset | Meaning |
| --- | --- |
| `+0xC0` | Input mode; raw input processing requires `== 1`. |
| `+0xD0` | Pointer to a GUI/event sink whose vtable slot `+0x18` can swallow char events. |
| `+0xE0` | Last input device type (`EInputDeviceType`: 1 = mouse and keyboard, set by both raw handlers; a `LastInputDeviceChangeEvent` is queued on change). |
| `+0x75A0`, `+0xA480` | Two `InputModuleKeyboard` objects (256 x 0x20 entries follow each). |
| `+0x85D0` | 256 `int` VK remap table for the polled keyboard module. |
| `+0x8194` | Raw input enabled flag. |
| `+0x89D0`, `+0x89E4` | Mouse button sequence counters (`0x80000000` bit = pressed). |
| `+0x89F8`, `+0x89FC` | Accumulated raw mouse delta X/Y for the frame; copied to `+0x8A00/+0x8A04` and zeroed in the device update `0x351DE0`. |
| `+0x8A10` | Accumulated wheel (float, scaled by `0x140C50B0C`). |
| `+0x8A30` | 256-byte raw key state (`0` up, `1` pressed this frame, `2` held); key 0x0F (Tab) special-cased with `+0x8A68`. |
| `+0x183D4`, `+0x183D8..+0x183F0` | Game-focus counter and a hash set of focused input ids: `HasGameFocus(id)` (`0x5F6900`) is false while the counter is > 0 unless the id is in the set. `DisableKey` (`0x5F5260`) adds to a set at `+0x183F8`. |
| `+0x18410/+0x18414` | Profile index (current/previous). |
| `+0x18419` | `IsEnabledMouseAndKeyboard`. |

Raw handlers queue typed events through `FUN_140337ac0(Input)`: `enf::RawKeyEvent`
(`{vftable, key, time, state 1=down/2=up}`), `enf::RawMouseEvent`, `enf::MouseMoveEvent`
(`{dx, dy}`), `enf::MouseWheelEvent`, `enf::CharEvent`, `enf::LastInputDeviceChangeEvent`.
The scan code is translated by `FUN_140352640` for E0-prefixed keys (`flags & 4`) and offset
by 0x80 for `flags & 2`.

Device update `0x351DE0(Input, flags)`: bit 2 = poll gamepads (`0x352010`), bit 0/1 =
re-enumerate raw devices (`0x34FF80(1|0)`) and fire plug/unplug callbacks, then
`FUN_140337780` and the mouse delta hand-over. It is a virtual method (vtable slots at
`0xE6D6EC`, `0xE6D72C`; `0xC50908`).

## Action registry and records

Registry (`DayZ+0x1009408`):

| Offset | Meaning |
| --- | --- |
| `+0x00`, `+0x01` | Two bytes checked by the record constructor (`0x53A430`): non-zero `+0x01` sets record flag `0x20`. |
| `+0xC8` | Array of `InputAccess*` (count `+0xD4`, capacity `+0xD0`), appended by `0x538B20`. |
| `+0xF0` | Hash map `hash → record*`: buckets of 0x18 bytes `{?, state(1 = used), hash, ?, record*}` at `+0xF0`, bucket count `+0xF8`, used `+0x100`; probe `(h + (2h+1) % n) % n`. Lookup: `FUN_14051b850(registry, hash)` (`0x51B850`). |
| `+0x110` | Default record returned for unknown names (its `+0x74 == -0xFFFF`). |

Name hash (`0x51B920`): `h = h * 37 + c` over the ASCII name with upper-case letters folded to
lower case (`c + 0x20` when `'A'..'Z'`), 32-bit wrap.

**Record** (the native `UAInput`):

| Offset | Meaning |
| --- | --- |
| `+0x28` | Pointer to the **active state block** (`+0x30` by default; set in the constructor `0x53A430`). |
| `+0x30` | State block A (0x20 bytes). `+0x34` flags: bit 1 `LocalPress`, bit 2 `LocalRelease` (assumed), bit 3 `LocalHold`, bit 4 `LocalHoldBegin` (assumed), bit 10 suppressed; `ForceEnable/Disable/Supress` clear with mask `0xFFFFFEA1`. `+0x3C` and `+0x40` are two floats whose **sum is `LocalValue()`** (`0x536AE0`). `+0x44` u16, `+0x48` 8 bytes, cleared by the force functions. |
| `+0x50` | State block B, same shape (`+0x54` flags, `+0x5C/+0x60` values, `+0x64`, `+0x68`). |
| `+0x70` | Control flags: `0x08` locked (`Lock` = `0x536AF0`, `IsLocked` tests `0x0C`), `0x20` set from registry byte `+0x01`, `0x40` force-enabled, `0x80` force-disabled (`0x533C00` / `0x533BC0`), bit 10 `0x400` suppress-until-release (`Supress` = `0x53A710`). |
| `+0x74` | Id; `-0xFFFF` marks the "missing input" default record. |

Enforce native getters on a record: `LocalValue 0x536AE0`, `LocalPress 0x536AC0`,
`LocalRelease 0x536AD0`, `LocalHold 0x536AA0`, `LocalHoldBegin 0x536AB0`,
`LocalDoubleClick 0x536A90`, `LocalClick 0x536A80`, `IsLocked 0x5360C0`. The `Input` class
natives `LocalValue_ID` / `LocalValue` (`0x5F5DF0` / `0x5F5E00`) are virtual calls through the
`Input` object (`+0x28` → vtable slots 0 and 1), gated by `HasGameFocus` when `check_focus`.

**InputAccess** (0x20 bytes, built by `0x51B920(access, "UAName")`): `+0x10` name hash,
`+0x18` record pointer (resolved at construction, default record if the name is unknown,
logged as `!!! [Inputs] Improper InputAccess declaration`). Accesses are registered in the
registry array so they can be re-resolved when presets change.

### Action tables consumed by gameplay

`0x535AC0(table)` builds the on-foot/weapon table: 63 accesses, 0x20 apart, in this order:
`UAMoveForward +0x00, UAMoveBack +0x20, UAMoveLeft +0x40, UAMoveRight +0x60, UAMoveUp +0x80,
UAMoveDown +0xA0, UAHeavyMeleeAttack +0xC0, UAMeleeAttackModifier +0xE0, UAWeaponMeleeAttack
+0x100, UATurbo +0x120, UAToggleTurbo +0x140, UAWalkRunToggle +0x160, UAWalkRunTemp +0x180,
UAWalkRunForced +0x1A0, UAWalkForward +0x1C0, UAWalkBack +0x1E0, UAWalkRight +0x200,
UAWalkLeft +0x220, UAAimUp +0x240, UAAimDown +0x260, UAAimLeft +0x280, UAAimRight +0x2A0,
UAGetOver +0x2C0, UAStance +0x2E0, UAStand +0x300, UACrouch +0x320, UAProne +0x340,
UAStandGetOver +0x360, UAPersonView +0x380, UATrackLeft/Right/Up/Down +0x3A0..+0x400,
UALookAround +0x420, UALookAroundToggle +0x440, UAPersonCamSwitchSide +0x460, UALeanLeft
+0x480, UALeanRight +0x4A0, UALeanLeftGamepad +0x4C0, UALeanRightGamepad +0x4E0,
UAEvasiveLeft +0x500, UAEvasiveRight +0x520, UAToggleRaiseWeapon +0x540, UATempRaiseWeapon
+0x560, UATempRaiseWeaponGamepad +0x580, UADefaultAction +0x5A0, UAFire +0x5C0, UAAction
+0x5E0, UAADSToggle +0x600, UADropItem +0x620, UAThrowitem +0x640, UAReloadMagazine +0x660,
UAToggleWeapons +0x680, UAZeroingUp +0x6A0, UAZeroingDown +0x6C0, UAHoldBreath +0x6E0,
UAHoldBreathToggle +0x700, UAZoomInOptics +0x720, UAZoomOutOptics +0x740, UAZoomIn +0x760,
UAZoomInToggle +0x780, UAVoiceOverNet +0x7A0, UAVoiceOverNetToggle +0x7C0`.
A second table (`0x534EF0`, 53 accesses, called from `0x535320`) covers the other group.
Vehicle actions are `UACarForward/Back/Left/Right`, `UACarHandbrake`, `UACarShiftGearUp/Down`,
`UAVehicleSlow/Turbo` (full name list: 324 `UA*` strings in the binary).

## Enforce-side facts that matter

- `HumanInputController.Override*` (movement speed/angle, aim change, raise, melee evade,
  free look) apply on the local client only. In multiplayer 1.29 the owner's overrides alone do
  **not** move the player; the server must apply the same overrides per consumed move
  (measured by the dayz-mcp project, PR 191). `OverrideRaise` works client-side.
- `UAInput.ForceEnable/ForceDisable/Supress/Lock` exist but there is no script setter for the
  value: the natives above read `+0x3C/+0x40`, nothing writes them from script.
- `Input.LocalValue(action, check_focus)` is the script read path; `GetGame().GetInput()`
  returns the `Input` object.

## What can be driven from outside the engine

| Path | Server mod needed | Focus independent | Analog | Status |
| --- | --- | --- | --- | --- |
| `SendInput` keys/mouse (current proxy) | no | **no** | no | verified, the problem |
| Write record values `+0x3C/+0x40` and flags `+0x34` after the registry update, before gameplay reads them | no | yes | yes | layout decompiled; update hook pending |
| Call the raw handlers `0x351610/0x351740` with synthetic `RAWINPUT` | no | yes | mouse only | decompiled, untested |
| Hook `XInputGetState` (ordinal 2 through `0xC02EF0`) and return a virtual pad | no | yes (polled) | yes | fallback; flips UI to controller mode |
| `HumanInputController.Override*` from a client mod | **yes** for movement | yes | yes | measured by dayz-mcp |

Open: the registry's per-frame evaluation function (which turns device events into record
flags and values) and the exact meaning of the two value floats (`+0x3C` vs `+0x40`; likely
per-device or per-alternative contributions).
