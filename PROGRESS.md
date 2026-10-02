# Recovery checkpoint — 2026-10-02

## Objective
Audit inherited DayZ-VR work, preserve it, fix defects in separate commits, and
continue native ammo display / simulator verification.

## Completed
- Prior project conversation retrieved through `ai log`/`ai handoff`; TASKS.md read.
- Inherited native ammo work saved unchanged in `9037255`; its build failure repaired.
- Full source/logic audit and repair ledger in `AUDIT.md`.
- Repairs through `78d24ad`: input loss handling, pose publication, XR image ownership,
  GUI runtime selection/resizing, projection freshness, aim suspension, test-present
  handling, ammo overflow, bounded log tools, PBO validation, isolated local server.
- Windows cross-build, 25 Python regressions, native ASan/UBSan checks, shellcheck,
  and executable-profile mutation/truncation checks all passed. Deployment completed.
- Render trace buffer prototype is preserved/tested but NOT wired to the runtime.

## Current state
- Branch: `fix/dayz-1.29.163709-proton`; tested code commit `78d24ad`.
- Full build/test/deploy passed. DLL backup: `build/deploy-backup/20261002-055945/`.
- Simulator/local-server cycle passed: joined world, 86–89 FPS, no new fatal/script
  errors, bridge and client raise command worked. Inventory was visually checked;
  pending mouse corrections remained zero while open and after closing. Focus loss
  cleared hand validity, and refocus restored it.
- Ammo swapchain initialized; controller-adjacent label visible but exact text and
  readability NOT verified. Screenshot: `build/logs/ammo-focused-20261002.png`.
- DayZ, task-owned local server and simulator stopped successfully. No ini changes.
- Audit agents hit their usage limits; their coherent changes were reviewed and
  tested locally. No agent work is still running. This is a progress checkpoint,
  not a claim that all audit findings or the stereo work are finished.

## Operational rules
- Read AUDIT.md, TASKS.md, git status and recent commits before editing.
- Use `scripts/build.sh`; never filter build/test output.
- Read FULL `scripts/dayz-status.sh` output; never pipe through grep/head/tail.
- Close DayZ when not actively testing. Stop task-owned server/simulator afterward.
- Use only local test server/simulator for unattended tests.
- Preserve `stereo_mode=alternate`; true stereo/translation remain unproven.
- Back up and restore any temporary game configuration changes.
- No pushes or publication requested.

## Immediate next steps
1. Read AUDIT.md's remaining findings. Prioritize debug-thread ownership C02 and
   plugin stop/restart P02; avoid direct render-state changes from TCP callbacks.
2. Fix bridge complete/fresh snapshot validation and internal magazines S01/S02.
3. Improve native ammo visual verification (close-up compositor capture or physical
   headset, then change ammunition/weapon). Keep alternate mode until R1 is resolved.
4. X09/X11/T04/P01/H01 and render trace buffer integration remain open.
5. After source changes, use `timeout 900 scripts/build.sh --deploy`, start the
   task-owned simulator/local server, launch via `run-dayz-direct.sh --sim --
   -connect=127.0.0.1 -port=2302 -mod=@DayZVR`, then inspect the FULL
   `scripts/dayz-status.sh --wait 300` output. Always stop the test processes.
