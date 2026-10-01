#!/usr/bin/env bash
# Build gate and deploy helper for the DayZ VR proxy on this Linux workstation.
#
#   scripts/build.sh                 build (inside the build-box container) and run tests
#   scripts/build.sh --deploy        ... then stop DayZ if it runs and copy the DLLs into the game folder
#   scripts/build.sh --deploy --start  ... then launch DayZ through Steam
#   scripts/build.sh --stop          only stop a running DayZ (game and launcher) and verify it is gone
#   scripts/build.sh --dry-run ...   print what would happen for stop/deploy/start without doing it
#
# Steps always run in this order no matter how the flags are given:
#   build -> test -> stop -> deploy -> start
# Any failure aborts the rest. Environment overrides: DAYZ_DIR, XWIN_ROOT, OPENXR_SDK,
# OPENXR_LOADER, BUILD_JOBS, BUILD_CONTAINER (default build-box).
set -euo pipefail
IFS=$'\n\t'

script_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
project_dir="$(dirname -- "$script_dir")"
build_dir="$project_dir/build"
log_dir="$build_dir/logs"

dayz_dir="${DAYZ_DIR:-/run/media/system/Data/Games/Steam/steamapps/common/DayZ}"
container="${BUILD_CONTAINER:-build-box}"
xwin_root="${XWIN_ROOT:-$HOME/.cache/dayz-vr-sdk}"
openxr_sdk="${OPENXR_SDK:-/run/media/system/Data/Projects/ArmA VR/.references/repos/runtime/OpenComposite/libs/openxr-sdk}"
openxr_loader="${OPENXR_LOADER:-$build_dir/original-install/openxr_loader.dll}"
dayz_app_id=221100

do_build=0
do_test=0
do_stop=0
do_deploy=0
do_start=0
dry_run=0

usage() {
  sed -n '2,15p' "${BASH_SOURCE[0]}" | sed 's/^# \{0,1\}//'
}

for arg in "$@"; do
  case "$arg" in
    --build) do_build=1 ;;
    --test) do_test=1 ;;
    --stop) do_stop=1 ;;
    --deploy) do_build=1; do_test=1; do_stop=1; do_deploy=1 ;;
    --start) do_start=1 ;;
    --dry-run) dry_run=1 ;;
    -h | --help) usage; exit 0 ;;
    *) printf 'unknown argument: %s\n' "$arg" >&2; usage >&2; exit 2 ;;
  esac
done
if (( do_build == 0 && do_test == 0 && do_stop == 0 && do_deploy == 0 && do_start == 0 )); then
  do_build=1
  do_test=1
fi

say() { printf '==> %s\n' "$*"; }
die() { printf 'error: %s\n' "$*" >&2; exit 1; }

in_container() { [[ -f /run/.containerenv || -n "${CONTAINER_ID:-}" ]]; }

# Runs a command inside the build container, or directly when already inside one.
# The working directory is set inside the same command string because entering a
# container resets it.
run_in_container() {
  local script="$1"
  if in_container; then
    bash -c "cd \"$project_dir\" && $script"
  else
    command -v distrobox >/dev/null || die "distrobox is required to build outside $container"
    distrobox enter "$container" -- bash -c "cd \"$project_dir\" && $script"
  fi
}

# Wine processes do not carry the exe name as their kernel process name, so match
# the full command line. The bracket keeps the pattern from matching this script.
dayz_pids() {
  pgrep -f 'DayZ_x6[4]\.exe|DayZLaunche[r]\.exe' || true
}

step_build() {
  say "build (container: $container, jobs: ${BUILD_JOBS:-4})"
  [[ -d "$xwin_root" ]] || die "XWIN_ROOT not found: $xwin_root"
  [[ -f "$openxr_sdk/include/openxr/openxr.h" ]] || die "OPENXR_SDK not found: $openxr_sdk"
  [[ -f "$openxr_loader" ]] || die "OPENXR_LOADER not found: $openxr_loader"
  mkdir -p "$log_dir"
  run_in_container "export XWIN_ROOT=\"$xwin_root\" OPENXR_SDK=\"$openxr_sdk\" OPENXR_LOADER=\"$openxr_loader\" BUILD_JOBS=\"${BUILD_JOBS:-4}\" && nice -n 10 ./scripts/build-linux.sh" 2>&1 | tee "$log_dir/build.log"
  [[ -f "$build_dir/dxgi.dll" && -f "$build_dir/dayz_openxr_debug.dll" ]] || die "build did not produce both DLLs"
}

step_test() {
  say "test"
  mkdir -p "$log_dir"
  local gpp_flags='-std=c++20 -Wall -Wextra -Wpedantic -Werror -Wshadow -Wconversion -Wsign-conversion'
  run_in_container "g++ $gpp_flags tests/debug_protocol_test.cpp -o build/debug_protocol_test && ./build/debug_protocol_test" 2>&1 | tee "$log_dir/test-protocol.log"
  run_in_container "g++ $gpp_flags tests/hmd_aim_loop_test.cpp common/hmd_aim_loop.cpp -o build/hmd_aim_loop_test && ./build/hmd_aim_loop_test" 2>&1 | tee "$log_dir/test-aim-loop.log"
  if [[ -f "$build_dir/dayz-image.bin" ]]; then
    run_in_container "g++ $gpp_flags -Icommon tests/build_checks.cpp -o build/build_checks && ./build/build_checks build/dayz-image.bin" 2>&1 | tee "$log_dir/test-build-checks.log"
  else
    die "build/dayz-image.bin (mapped DayZ_x64.exe image) is missing; the build-profile test cannot run"
  fi
  if command -v shellcheck >/dev/null; then
    shellcheck "$script_dir"/*.sh | tee "$log_dir/shellcheck.log"
  else
    run_in_container "command -v shellcheck >/dev/null && shellcheck scripts/*.sh" 2>&1 | tee "$log_dir/shellcheck.log" || die "shellcheck is not installed on the host or in $container"
  fi
}

# Stops DayZ and its launcher by asking Wine to close them, then waits for the
# processes to leave. tail --pid is a real blocking wait, not a poll loop.
step_stop() {
  say "stop DayZ"
  local pids
  pids="$(dayz_pids)"
  if [[ -z "$pids" ]]; then
    say "DayZ is not running"
    return
  fi
  if (( dry_run )); then
    say "dry-run: would stop pids: ${pids//$'\n'/ }"
    return
  fi
  local pid
  for pid in $pids; do
    kill -TERM "$pid" 2>/dev/null || true
  done
  for pid in $pids; do
    timeout 60 tail --pid="$pid" -f /dev/null || die "pid $pid did not exit within 60 s"
  done
  pids="$(dayz_pids)"
  [[ -z "$pids" ]] || die "DayZ processes still present after stop: ${pids//$'\n'/ }"
  say "DayZ exited"
}

step_deploy() {
  say "deploy to $dayz_dir"
  [[ -f "$dayz_dir/DayZ_x64.exe" ]] || die "not a DayZ folder: $dayz_dir"
  local pids
  pids="$(dayz_pids)"
  [[ -z "$pids" ]] || die "refusing to overwrite DLLs while DayZ runs (pids: ${pids//$'\n'/ })"
  local stamp backup
  stamp="$(date +%Y%m%d-%H%M%S)"
  backup="$build_dir/deploy-backup/$stamp"
  if (( dry_run )); then
    say "dry-run: would back up to $backup and copy dxgi.dll, dayz_openxr_debug.dll"
    return
  fi
  mkdir -p "$backup"
  local name
  for name in dxgi.dll dayz_openxr_debug.dll; do
    [[ -f "$dayz_dir/$name" ]] && cp -p "$dayz_dir/$name" "$backup/$name"
    cp -f "$build_dir/$name" "$dayz_dir/$name"
    cmp "$build_dir/$name" "$dayz_dir/$name"
  done
  if [[ ! -f "$dayz_dir/dayz_openxr.ini" ]]; then
    cp "$project_dir/dayz_openxr.ini" "$dayz_dir/dayz_openxr.ini"
    say "installed the reference dayz_openxr.ini (none was present)"
  else
    say "kept the existing dayz_openxr.ini"
  fi
  say "deployed; previous DLLs saved in $backup"
}

step_start() {
  say "start DayZ (Steam app $dayz_app_id)"
  command -v steamcli >/dev/null || die "steamcli is required to start the game"
  if (( dry_run )); then
    say "dry-run: would run: steamcli client run $dayz_app_id"
    return
  fi
  steamcli client run "$dayz_app_id"
  say "launch requested; the Steam launch options and the DayZ Launcher take it from here"
}

(( do_build )) && step_build
(( do_test )) && step_test
(( do_stop )) && step_stop
(( do_deploy )) && step_deploy
(( do_start )) && step_start

say "summary: build=$do_build test=$do_test stop=$do_stop deploy=$do_deploy start=$do_start dry_run=$dry_run"
