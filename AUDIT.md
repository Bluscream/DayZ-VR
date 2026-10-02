# DayZ-VR code and logic audit — 2026-10-02

## Scope and baseline

Baseline: `9037255` on `fix/dayz-1.29.163709-proton`. This checkpoint preserves the
inherited, unfinished native ammo quad exactly as found. Its parent is `0509ce5`.
The baseline Linux cross-build **fails**: `common/ammo_display.hpp:32` declares
`std::string` without including `<string>`. No failed build was deployed.

The audit covers the first-party C++ runtime, DXGI proxy, OpenXR host, debug plugin,
native tests, Enforce client/server mods, build descriptions, and scripts. MinHook
and external SDK/runtime implementations were treated as dependency boundaries,
not re-audited. This is a source/logic audit plus targeted executable checks; it is
not proof that every engine offset, driver, headset or server combination works.
Line references below identify the baseline, so use `git show 9037255:<path>` if
later fixes move them.

Prior work was retrieved with the `ai` CLI: the continued Claude session
`db7455a7-b5b3-4bca-918e-92a58ad9feb9` and its predecessor
`46b7bda7-44f4-403c-8468-87db0eb9bd4d`. The requested “Project overview via MCP
tools” title was not present in the CLI index; the workspace-scoped continued
transcript contains the matching project history, including the simulator, script
bridge, and unfinished native ammo display. `TASKS.md` was read and compared with
the implementation. Some G1/U1 status text predates already implemented work.

## Findings

Priority: P1 = crash, invalid runtime state, significant input or execution risk;
P2 = functional defect/reliability issue; P3 = edge case or maintenance gap.
Unless marked reproduced, these are static control-flow findings.

| ID | Priority | Evidence and consequence | Required correction |
| --- | --- | --- | --- |
| B01 | P1 | `ammo_display.hpp:32`: missing `<string>`; reproduced cross-build failure. | Make the header self-contained and run the full gate. |
| B02 | P1 | Follow-up: `vr_common` is a static library without `_WINDLL`; `openxr_host.cpp` uses that macro to select the standalone path, compiling out game controller injection and VR GUI for the proxy too. Confirmed in generated `flags.make`. | Select game behavior from the attached game swapchain at runtime, and validate the compiled path in the simulator. |
| X01 | P1 | `openxr_host.cpp:1354-1368,1056-1063,958-966`: held controller input is not released on no-render frames, locate/sync failure, or loss of focus/session. | Separate input cleanup from rendering and release held state at every loss boundary. |
| X02 | P1 | `openxr_host.cpp:1345-1365`: view validity bits are ignored, yet pose fields are published as valid. | Require valid orientation and position before reading/publishing poses; clear stale tracking. |
| X03 | P1 | `openxr_host.cpp:1425-1458,1643`: a failed eye acquire leaves a zeroed view in a submitted two-view layer. | Submit projection only after both eyes finish successfully. |
| X04 | P1 | `openxr_host.cpp:753-754`: ammo acquire failure after an earlier success returns true before filling the output quad. | Never return a successful layer before constructing it. |
| X05 | P2 | `openxr_host.cpp:757-766,1431-1450,1470-1481,1520-1529`: wait failures still release images; release results are ignored. | Encode acquire/wait/release ownership and treat only `XR_SUCCESS` as image-ready. |
| X06 | P2 | `openxr_host.cpp:824-827`: writes every enumerated axis texture without acquire/wait. | Initialize only owned images; remove the unnecessary eager writes. |
| X07 | P2 | `openxr_host.cpp:608,793`: disabling axes also prevents GUI/direction ray swapchain creation. | Create the shared texture when any of its consumers is enabled. |
| X08 | P2 | `openxr_host.cpp:1119-1147`: comfort delta uses a clock advanced only by closed-loop aim. | Advance the input clock independently of aiming mode. |
| X09 | P2 | `comfort.cpp:39-50`: NaN/Inf ini values survive parsing/clamping into shader constants. | Reject nonfinite and malformed values at the boundary. |
| X10 | P2 | `openxr_host.cpp:1474-1481`: a prior GUI image validates a later acquired image whose rendering failed. | Track success of the actual most recently released image. |
| X11 | P2 | `openxr_host.cpp:920-936,1687-1733`: partial initialization leaks state; shutdown leaves exit/GUI/tracking state behind. | Shared cleanup under the host lock and complete state reset. |
| C01 | P1 | `openxr_host.cpp:1669-1673,178`: debug reads use a different mutex from lifecycle writes; eye-dump request reads an unprotected frame-source pointer. | Publish synchronized snapshots and queue dump requests without dereferencing render-owned state. |
| C02 | P1 | `dayz_runtime_probe.cpp:3634-3638,3679-3687`: debug-thread tunable writes/recenter mutate ordinary game/render globals. | Establish thread ownership/command handoff; do not claim a mutex in the TCP layer protects engine hooks. |
| R01 | P1 | `dayz_runtime_probe.cpp:2508-2527`: closed-loop error accumulates in pending mouse counts while inventory prevents consumption. | Suspend/reset the controller while input is unavailable; regression-test long inventory dwell. |
| R02 | P1 | `dayz_runtime_probe.cpp:1571,1590,1605,1628`: double-render replay republishes its own freshness flag. | Only genuine engine projection dispatch may authorize a double-render pass. |
| R03 | P2 | `dayz_runtime_probe.cpp:2049`: GUI target sizing rejects smaller dimensions after a real resolution reduction. | Invalidate size/resources on primary swapchain resize; retain the auxiliary-target filter. |
| R04 | P2 | `dxgi/swapchain_hooks.cpp:254,268`: `DXGI_PRESENT_TEST` still advances and waits for an XR frame. | Forward visibility tests without frame submission. |
| T01 | P1 | `scripts/dayz-cmd.sh:58-59`: command text is interpolated into shell source. Apostrophes can break out of quoting. | Pass data as arguments; no command-text shell evaluation. |
| T02 | P2 | `scripts/dayz-status.sh:238`: absent/unreadable mod log can produce success; `grep -q` + pipefail misclassifies a large fatal log after producer SIGPIPE. | Fail closed on missing evidence and consume the complete current-run log. |
| T03 | P2 | `dayz-status.sh:73-82`, `dayz-cmd.sh:58-59`: a matching line does not guarantee the pipeline finishes until another write or timeout. | Event-driven bounded log watching with cleanup and explicit result. |
| T04 | P2 | `.vcxproj` source lists omit newly required implementations; README still advertises them. | Restore source parity or clearly designate/maintain the supported build path. |
| S01 | P2 | `script_bridge.cpp:128-168`: empty/partial input is valid, malformed numbers silently become zero, and stale state persists indefinitely. | Validate complete snapshots, reject malformed data, expire stale samples. |
| S02 | P2 | Enforce bridge only queries detachable magazine ammo. | Read internal-magazine ammo for weapons that use it. |
| S03 | P2 | Local test server uses host networking and blank password; observed bound to `0.0.0.0:2302/2305`. | Bind the test server to loopback by default; preserve explicit opt-in override. |
| P01 | P2 | `debug_plugin/protocol.hpp:139`: string loop dereferences before its bounds check; JSON emits NaN/Inf and unescaped control characters. | Bounded strings and valid JSON for every value; strict parser tests. |
| P02 | P2 | `dayz_openxr_debug.cpp:70-111,157-170`: Stop closes listener but not an accepted socket blocked in recv; repeated Start overwrites a joinable thread. | Serialize lifecycle, wake the active connection, make starts/stops idempotent and release Winsock on failed start. |
| A01 | P3 | `ammo_display.cpp:141`: `INT_MAX + 1` signed overflow if chambered. | Compare/clamp before arithmetic; UBSan extremes test. |
| H01 | P3 | `dayz_hotkeys.cpp:53-63`: `_wtoi` accepts `F1x` as F1 despite the parser's strict-name contract. | Parse the complete numeric suffix; add portable parser coverage. |

## Architectural and verification debt

- Real eye separation and positional tracking are **not established**. Prior simulator
  experiments recorded identical eye captures even with exaggerated separation.
  `stereo_mode=double` replays already prepared draw lists; identifying the correct
  per-eye scene preparation boundary remains R1 in `TASKS.md`. Keep alternate mode
  the default and do not label double mode true stereo.
- The window-drag crash workaround is a guard, not a proven root-cause repair.
  Simulator sessions did not reproduce the original WiVRn crash. Preserve diagnostics.
- The runtime probe is 3,689 lines and the OpenXR host 1,734 lines at baseline;
  several functions exceed 100 lines. Extraction should follow ownership boundaries,
  especially GUI capture, controller input, and runtime control. Broad formatting
  or arbitrary splitting would obscure this correctness audit.
- The gate has a Windows cross-build and a few native logic tests, but lacks CTest
  registration, comprehensive sanitizer/race/fault-injection coverage, enforced size
  limits, and strict Python typing/formatting. The mapped-PE fixture is local-only.
  A simulator run cannot prove physical headset/controller comfort or stereo depth.
- The crash reporter takes logging locks and allocates from a vectored exception
  handler, and suppresses faults inside the proxy. It is best-effort diagnostics,
  not a reliable crash-safe telemetry mechanism; avoid claiming every fault logs.
- Optional controller profiles are suggested without enabling their required
  extensions. This is already tracked in M1, and needs extension-aware binding.
- Device recreation, hook teardown, and game/render-thread shared state need a
  broader ownership design. Fixes to individual debug races do not prove all engine
  hook concurrency safe.

## Contract references

- [OpenXR view validity](https://registry.khronos.org/OpenXR/specs/1.1/man/html/XrViewStateFlagBits.html)
- [OpenXR image wait](https://registry.khronos.org/OpenXR/specs/1.1/man/html/xrWaitSwapchainImage.html)
- [OpenXR image release](https://registry.khronos.org/OpenXR/specs/1.1/man/html/xrReleaseSwapchainImage.html)

## Repair and validation record

Initial audit committed before repairs. Subsequent entries below and `PROGRESS.md`
record specific commits, test evidence, and remaining limitations. A finding is not
closed merely because its source was edited.

### Repairs landed and checked

- `1ee3eda`, `170d778`: B01/A01; self-contained ammo header and bounded ammo arithmetic.
- `d29f1ba`: R01; blocked game input suspends pending aim corrections.
- `f21d9ac`: B02; game controller/GUI behavior selected at runtime instead of `_WINDLL`.
- `62a5018`: R02; only engine-issued projections authorize replay.
- `6395c5c`: coherent pose publication and explicit tracking invalidation; concurrent
  pose regression passed ThreadSanitizer in the audit pass.
- `c5f22f7`: X03–X07/X10; explicit image ownership, complete projection submission,
  and image-specific success. Injected acquire/wait/release failures pass regressions.
- `a44679a`: R04; visibility-test presents do not advance XR frames.
- `793b40f`: X01/X02/X08 and the specific C01 lifecycle/dump-request races; tests cover
  focus, view validity, and time progression. Broader C02 ownership remains open.
- `4e2a1e2`: R03; GUI capture selection resets on observed primary resize. Portable
  sizing regression passes; visual resize behavior still needs live verification.
- `8d14201`: T01–T03; no shell evaluation of command text, bounded/reaped log followers,
  and missing/stale/fatal logs fail launch classification.
- `cd6e290`: additional PBO findings repaired: traversal/symlink destinations, namespace
  collisions, zero-distance LZSS references, and corrupt/unknown packing fallback.
- `9263f38`: S03; loopback-published test-server ports. Setup also refuses a running
  container and preserves deployed mods/profile data. Mock CLI regressions pass.
- `78d24ad`: gate now runs 25 Python regressions and native AddressSanitizer and
  UndefinedBehaviorSanitizer tests, including frame policy and GUI sizing.

Full Windows cross-build and the expanded native/tooling gate passed on 2026-10-02.
The mapped executable was accepted; 208 instruction mutations, 13 truncations and
empty input were rejected. These checks do not establish headset visuals or true stereo.

### Remaining follow-up findings

- Render diagnostic arrays publish reservation counts before records are complete;
  snapshots/reset can race producers. `be6c351` preserves a tested synchronized
  buffer prototype, **not integrated into the runtime**. This defect remains open.
- The simulator script discovers all `monado-service` processes rather than proving
  ownership from its saved PID. Do not use its stop action against an unrelated runtime.
- The mapped-PE test fixture needs explicit freshness/hash verification against the
  installed executable. A passing old fixture is not evidence for a new game version.
- Native bridge parsing of fully quoted `-profiles=...` arguments needs correction.
  Client/server command validation, internal magazine counts and snapshot freshness
  remain part of S01/S02 follow-up.
- X09, X11, C02, T04, S01, S02, P01, P02 and H01 remain open. No claim is made that
  the project-wide audit findings are all repaired.

### Live simulator verification — 06:00–06:03 local time

The deployed build joined the loopback-only local server with simulated WMR hands.
The full `dayz-status.sh --wait 300` report passed, as did subsequent full reports:
86–89 FPS after loading, valid HMD/controller poses, no new error/fatal/guard-skip
entries, and no new script errors. The script bridge reported M4A1, ammo=10,
chamber=1. The client `raise 1` command returned promptly and raised the weapon.

Inventory was visually present in the compositor; pending mouse x/y were both zero
while open and immediately after closing. Aim error settled near 0.1 degrees.
Losing desktop focus cleared hand validity; focusing DayZ restored it. This is
live evidence for the repaired game-path selection and focus/aim suspension, not
proof for every physical controller or XR loss condition.

The ammo swapchain initialized at 170x48; no ammo/image/end-frame errors appeared.
A small controller-adjacent label is visible in `build/logs/ammo-focused-20261002.png`,
but the screenshot is too small to validate its exact content/readability. Native
ammo visual verification remains partial. The inventory capture was inspected;
actual primary-backbuffer downsize was not exercised. Game ini was not modified.
