#!/usr/bin/env bash
# Start DayZ_x64.exe directly through the Steam Linux Runtime and the Proton
# build Steam has mapped to it, skipping the DayZ Launcher. Steam must be
# running (DRM), but the Steam launch options are not used, so the game's
# environment is fully under this script's control.
#
#   scripts/run-dayz-direct.sh            flat start to the main menu (no OpenXR runtime forced)
#   scripts/run-dayz-direct.sh --sim      use the headless Monado runtime from scripts/xr-sim.sh
#   scripts/run-dayz-direct.sh -- ARGS    pass extra DayZ command-line arguments
#
# The game runs detached; stdout/stderr go to build/logs/dayz-direct.log and the
# Proton log to build/logs/steam-221100.log. Stop it with scripts/build.sh --stop.
# Before each start, old output is pruned so nothing grows without bound: DayZ's
# crash/script/RPT logs and minidumps keep the newest KEEP_RUNS (default 3) of each
# kind, dayz_openxr.log is rotated to .1 once it passes LOG_ROTATE_MB (default 64),
# build/logs screenshots keep the newest 10 and deploy backups the newest 5.
# The line number of dayz_openxr.log at launch is written to
# build/logs/openxr-log-offset.txt for scripts/dayz-status.sh.
# Environment overrides: DAYZ_DIR, STEAM_LIBRARY (library holding compatdata/221100),
# STEAM_ROOT (client install, default ~/.local/share/Steam), PROTON_DIR, SLR_DIR.
set -euo pipefail
IFS=$'\n\t'

script_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
project_dir="$(dirname -- "$script_dir")"
log_dir="$project_dir/build/logs"
app_id=221100
steam_root="${STEAM_ROOT:-$HOME/.local/share/Steam}"
steam_library="${STEAM_LIBRARY:-/run/media/system/Data/Games/Steam}"
dayz_dir="${DAYZ_DIR:-$steam_library/steamapps/common/DayZ}"
proton_dir="${PROTON_DIR:-$steam_root/compatibilitytools.d/Proton-GE Latest}"
# GE-Proton 11 requires the Steam Linux Runtime 4.0 container (toolmanifest
# require_tool_appid 4183110); Proton 8/9 use SteamLinuxRuntime_sniper instead.
slr_dir="${SLR_DIR:-$steam_library/steamapps/common/SteamLinuxRuntime_4}"
compat_data="$steam_library/steamapps/compatdata/$app_id"

say() { printf '==> %s\n' "$*"; }
die() { printf 'error: %s\n' "$*" >&2; exit 1; }

use_sim=0
game_args=(-nobe)
while (( $# )); do
  case "$1" in
    --sim) use_sim=1 ;;
    --) shift; game_args+=("$@"); break ;;
    -h | --help) sed -n '2,21p' "${BASH_SOURCE[0]}" | sed 's/^# \{0,1\}//'; exit 0 ;;
    *) die "unknown argument: $1" ;;
  esac
  shift
done

[[ -f "$dayz_dir/DayZ_x64.exe" ]] || die "DayZ not found in $dayz_dir"
[[ -x "$proton_dir/proton" ]] || die "Proton not found: $proton_dir"
[[ -x "$slr_dir/_v2-entry-point" ]] || die "Steam Linux Runtime not found: $slr_dir"
[[ -d "$compat_data/pfx" ]] || die "Proton prefix missing: $compat_data/pfx"
pgrep -x steam >/dev/null || die "the Steam client is not running; DayZ needs it for DRM"
if pgrep -f 'DayZ_x6[4]\.exe|DayZLaunche[r]\.exe' >/dev/null; then
  die "DayZ is already running; stop it first (scripts/build.sh --stop)"
fi
mkdir -p "$log_dir"

# --- prune past output ------------------------------------------------------------
keep_runs="${KEEP_RUNS:-3}"
rotate_mb="${LOG_ROTATE_MB:-64}"
appdata_dayz="$compat_data/pfx/drive_c/users/steamuser/AppData/Local/DayZ"
prune_newest() {
  # prune_newest <keep> <dir> <glob>: delete all but the newest <keep> matches.
  local keep="$1" dir="$2" glob="$3" removed=0 file
  [[ -d "$dir" ]] || return 0
  while IFS= read -r file; do
    rm -f -- "$file"; removed=$(( removed + 1 ))
  done < <(find "$dir" -maxdepth 1 -name "$glob" -printf '%T@ %p\n' | sort -rn | tail -n +"$((keep + 1))" | cut -d' ' -f2-)
  (( removed )) && say "pruned $removed old $glob from $(basename -- "$dir")"
  return 0
}
for glob in 'crash_*.log' 'script_*.log' 'DayZ_x64_*.RPT' 'DayZ_x64_*.mdmp' 'info_*.log' 'warning_*.log' 'error_*.log'; do
  prune_newest "$keep_runs" "$appdata_dayz" "$glob"
done
prune_newest 10 "$log_dir" 'screen-*.png'
if [[ -d "$project_dir/build/deploy-backup" ]]; then
  while IFS= read -r dir; do rm -rf -- "$dir"; say "pruned deploy backup $(basename -- "$dir")"; done \
    < <(find "$project_dir/build/deploy-backup" -mindepth 1 -maxdepth 1 -type d | sort -r | tail -n +6)
fi
openxr_log="$dayz_dir/dayz_openxr.log"
if [[ -f "$openxr_log" ]] && (( $(stat -c %s "$openxr_log") > rotate_mb * 1024 * 1024 )); then
  mv -f -- "$openxr_log" "$openxr_log.1"
  say "rotated dayz_openxr.log (> ${rotate_mb} MB) to dayz_openxr.log.1"
fi
rm -f -- "$dayz_dir"/dayz_openxr_eye*.bmp
{ [[ -f "$openxr_log" ]] && wc -l < "$openxr_log" || echo 0; } > "$log_dir/openxr-log-offset.txt"
# ----------------------------------------------------------------------------------

env_list=(
  "STEAM_COMPAT_APP_ID=$app_id"
  "SteamAppId=$app_id"
  "SteamGameId=$app_id"
  "STEAM_COMPAT_CLIENT_INSTALL_PATH=$steam_root"
  "STEAM_COMPAT_DATA_PATH=$compat_data"
  "STEAM_COMPAT_INSTALL_PATH=$dayz_dir"
  "STEAM_COMPAT_LIBRARY_PATHS=$steam_library:$steam_root"
  "STEAM_COMPAT_TOOL_PATHS=$proton_dir:$slr_dir"
  "STEAM_COMPAT_MOUNTS=$project_dir"
  "PROTON_LOG=1"
  "PROTON_LOG_DIR=$log_dir"
)
if (( use_sim )); then
  [[ -S "${XDG_RUNTIME_DIR:?}/monado_comp_ipc" ]] ||
    die "the simulated runtime is not running; start it with scripts/xr-sim.sh start"
  mapfile -t sim_env < <("$script_dir/xr-sim.sh" env)
  env_list+=("${sim_env[@]}")
  say "using the headless Monado runtime"
fi

say "starting DayZ_x64.exe ${game_args[*]} via $(basename -- "$proton_dir") (log: $log_dir/dayz-direct.log)"
cd "$dayz_dir"
setsid env "${env_list[@]}" "$slr_dir/_v2-entry-point" --verb=waitforexitandrun -- \
  "$proton_dir/proton" waitforexitandrun "$dayz_dir/DayZ_x64.exe" "${game_args[@]}" \
  > "$log_dir/dayz-direct.log" 2>&1 < /dev/null &
say "launch requested (pid $!); use scripts/build.sh --stop to close the game"
