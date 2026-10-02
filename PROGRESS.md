# Recovery checkpoint — 2026-10-02

## Objective

Audit the inherited project, preserve its work, fix concrete defects in separate
commits, then continue the native ammo display and simulator verification.

## Current state

- Branch: `fix/dayz-1.29.163709-proton`.
- Inherited work checkpointed as `9037255`; baseline build fails on missing `<string>`
  in `common/ammo_display.hpp`. No failed build deployed.
- Source audit recorded in `AUDIT.md`; repairs pending.
- Prior continued session inspected through `ai log`/`ai handoff`; `TASKS.md` read.
- DayZ client was already stopped. The inherited local test server and Monado
  simulator have also been stopped while source work proceeds.

## Operational rules for resuming

- Read `AUDIT.md`, `TASKS.md`, the current git status and recent commits first.
- Use `scripts/build.sh` as the build/test gate; do not hide diagnostics.
- Always read the **full** output of `scripts/dayz-status.sh`; never pipe it through
  grep/head/tail or another filter. This is an explicit user preference from the
  prior session. A complete report can be teed to `build/logs/`.
- Keep DayZ closed except during an active test. Stop the local server/simulator
  after test work. Do not target unrelated Wine/desktop processes.
- Use only the local test server and the simulator for unattended end-to-end work.
- Keep `stereo_mode=alternate` as default; true stereo/translation remain unproven.
- Preserve game configuration in a timestamped backup before temporary test edits,
  then restore it. Do not mix test settings into the user's permanent config.

## Next actions

Fix audit findings with regressions, run the complete gate, commit each concern
separately, then deploy and run the simulator/local-server cycle. Inspect screenshot,
debug state, bridge files and complete status report before reporting success.
