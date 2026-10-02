#!/usr/bin/env bash
# One-shot status report for a DayZ-VR test launch, so nothing has to be hunted by hand:
#
#   scripts/dayz-status.sh                 report now
#   scripts/dayz-status.sh --wait 240      first block (bounded) until the game is in-world,
#                                          crashed, or the timeout passes, then report
#   scripts/dayz-status.sh --since N       only count log lines after line N (default: the
#                                          offset file written by run-dayz-direct.sh)
#   scripts/dayz-status.sh --lines 10      how many recent log lines to show (default 10)
#
# Every section always runs (no opt-outs): a full-screen capture is saved to build/logs/
# on each report so dialogs and the game view can be inspected afterwards.
#
# Sections: game install (exe version/size/time), deployed artifacts vs build outputs,
# ini highlights, processes, window, OpenXR session via the debug plugin, log summary
# (errors/fatal/skips) and tail, crash dumps and DayZ's own logs, script bridge files,
# sim runtime and local server. Exit code is 0 when the game is running without a fatal
# exception since the offset, 1 otherwise. With --wait, an in-world marker is also
# required. The event-driven follower has one deadline and always reaps its children.
set -euo pipefail
IFS=$'\n\t'

script_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
project_dir="$(dirname -- "$script_dir")"
build_dir="$project_dir/build"
dayz_dir="${DAYZ_DIR:-/run/media/system/Data/Games/Steam/steamapps/common/DayZ}"
steam_library="${STEAM_LIBRARY:-/run/media/system/Data/Games/Steam}"
# $profile: (where DayZ and the script mod write) resolves to %LOCALAPPDATA%\DayZ without
# -profiles=; Documents\DayZ only holds the player profile/settings files.
local_appdata="$steam_library/steamapps/compatdata/221100/pfx/drive_c/users/steamuser/AppData/Local/DayZ"
profile_dir="${DAYZ_PROFILE_DIR:-$local_appdata}"
documents_dir="$steam_library/steamapps/compatdata/221100/pfx/drive_c/users/steamuser/Documents/DayZ"
log="$dayz_dir/dayz_openxr.log"
offset_file="$build_dir/logs/openxr-log-offset.txt"

wait_seconds=0
since=""
lines=10
while (( $# )); do
  case "$1" in
    --wait) wait_seconds="${2:?--wait requires seconds}"; shift 2 ;;
    --since) since="${2:?--since requires an offset}"; shift 2 ;;
    --lines) lines="${2:?--lines requires a count}"; shift 2 ;;
    -h|--help) sed -n '2,22p' "${BASH_SOURCE[0]}" | sed 's/^# \{0,1\}//'; exit 0 ;;
    *) echo "unknown argument: $1" >&2; exit 2 ;;
  esac
done
if [[ -z "$since" ]]; then
  since="$( [[ -f "$offset_file" ]] && cat "$offset_file" || echo 0 )"
fi

say() { printf '\n== %s\n' "$*"; }
row() { printf '  %-22s %s\n' "$1" "$2"; }
file_info() {
  # "<size> <mtime>" or "missing"
  if [[ -f "$1" ]]; then
    printf '%s  %s' "$(du -h "$1" | cut -f1)" "$(date -r "$1" '+%Y-%m-%d %H:%M:%S')"
  else
    printf 'missing'
  fi
}
same_file() { [[ -f "$1" && -f "$2" ]] && cmp -s "$1" "$2" && echo "same as build" || echo "DIFFERS from build"; }
dayz_pids() { pgrep -f 'DayZ_x6[4]\.exe' || true; }

[[ "$wait_seconds" =~ ^[0-9]+$ && "$since" =~ ^[0-9]+$ && "$lines" =~ ^[1-9][0-9]*$ ]] || {
  echo "--wait/--since must be nonnegative integers and --lines must be positive" >&2
  exit 2
}
wait_passed=1
if (( wait_seconds > 0 )); then
  say "waiting up to ${wait_seconds}s for in-world or a failure marker"
  python3 "$script_dir/dayz_log.py" wait-launch --log "$log" --since "$since" \
    --timeout "$wait_seconds" --profile "$profile_dir" --marker "$offset_file" || wait_passed=0
fi

say "game install"
exe="$dayz_dir/DayZ_x64.exe"
row "exe" "$(file_info "$exe")"
if [[ -f "$exe" ]]; then
  # PE link timestamp identifies the build (matches dayz_build_profiles.hpp peTimestamp).
  pe_stamp="$(python3 - "$exe" <<'EOF'
import struct, sys, datetime
with open(sys.argv[1], 'rb') as f:
    f.seek(0x3C); pe = struct.unpack('<I', f.read(4))[0]
    f.seek(pe + 8); ts = struct.unpack('<I', f.read(4))[0]
print(f"0x{ts:08X} ({datetime.datetime.fromtimestamp(ts, datetime.timezone.utc):%Y-%m-%d %H:%M} UTC)")
EOF
)"
  row "pe timestamp" "$pe_stamp"
fi
manifest="$steam_library/steamapps/appmanifest_221100.acf"
if [[ -f "$manifest" ]]; then
  row "steam buildid" "$(grep -m1 'buildid' "$manifest" | grep -o '[0-9]\+' | tail -1)"
fi
row "game dir" "$dayz_dir"

say "deployed artifacts (game dir vs build/)"
for name in dxgi.dll dayz_openxr_debug.dll; do
  row "$name" "$(file_info "$dayz_dir/$name")  $(same_file "$build_dir/$name" "$dayz_dir/$name")"
done
row "@DayZVR pbo" "$(file_info "$dayz_dir/@DayZVR/addons/DayZVR.pbo")  $(same_file "$build_dir/@DayZVR/addons/DayZVR.pbo" "$dayz_dir/@DayZVR/addons/DayZVR.pbo")"
row "ini" "$(file_info "$dayz_dir/dayz_openxr.ini")"
if [[ -f "$dayz_dir/dayz_openxr.ini" ]]; then
  row "ini highlights" "$(grep -E '^(stereo_mode|alternate_eye|hmd_native_aim|hmd_aim_closed_loop|controller_aim|lock_yaw|lock_pitch|keep_focus|enabled|vignette)=' "$dayz_dir/dayz_openxr.ini" | tr '\n' ' ')"
fi
row "git" "$(git -C "$project_dir" log --oneline -1 2>/dev/null) $(git -C "$project_dir" status --short 2>/dev/null | wc -l | sed 's/^/(dirty files: /; s/$/)/')"

say "processes"
pids="$(dayz_pids)"
if [[ -z "$pids" ]]; then
  row "game" "not running"
else
  for pid in $pids; do
    row "pid $pid" "$(ps -o stat=,%cpu=,%mem=,etime= -p "$pid" | sed 's/^ *//')"
  done
fi
if command -v kdotool >/dev/null; then
  # Every window of the game process: the game itself plus any Enforce compile error or
  # crash dialog, which would otherwise block the launch silently.
  wins="$(kdotool search --name '' 2>/dev/null || true)"
  found=0
  for win in $wins; do
    win_pid="$(kdotool getwindowpid "$win" 2>/dev/null || true)"
    for pid in $pids; do
      if [[ "$win_pid" == "$pid" ]]; then
        row "window $win" "$(kdotool getwindowname "$win" 2>/dev/null | cut -c1-80) $(kdotool getwindowgeometry "$win" 2>/dev/null | tr '\n' ' ' | sed 's/Window [^ ]* //')"
        found=1
      fi
    done
  done
  (( found )) || row "window" "none owned by the game"
  active="$(kdotool getactivewindow 2>/dev/null || true)"
  [[ -n "$active" ]] && row "active window" "$active $(kdotool getwindowname "$active" 2>/dev/null | cut -c1-60)"
fi
if command -v spectacle >/dev/null; then
  mkdir -p "$build_dir/logs"
  shot="$build_dir/logs/screen-$(date '+%Y%m%d-%H%M%S').png"
  if timeout 20 spectacle -b -n -f -o "$shot" >/dev/null 2>&1 && [[ -s "$shot" ]]; then
    row "screenshot" "$shot ($(du -h "$shot" | cut -f1))"
  else
    row "screenshot" "failed"
  fi
fi
row "monado-service" "$(pgrep -x monado-service >/dev/null && echo running || echo stopped)"
row "local server" "$(podman ps --format '{{.Names}} {{.Status}}' 2>/dev/null | grep dayz || echo stopped)"

say "openxr session (debug plugin)"
if [[ -n "$pids" ]] && timeout 8 python3 "$script_dir/dayz-vr-ctl.py" ping >/dev/null 2>&1; then
  timeout 8 python3 "$script_dir/dayz-vr-ctl.py" get 2>/dev/null | python3 -c "$(cat <<'PY'
import sys, json, math
d = json.load(sys.stdin)
print("  fps=%.1f state=%s focus=%s in_world=%s gui=%s eye=%s presents=%s hmd_yaw=%.1f aimerr=(%+.1f,%+.1f) aimR=%s" % (
    d["host_fps"], d["session_state"], d["window_focused"], d["camera_directions_valid"], d["gui_cursor_mode"],
    d["rendered_eye"], d["present_count"], math.degrees(d["hmd_yaw"]),
    math.degrees(d.get("aim_yaw_error", 0)), math.degrees(d.get("aim_pitch_error", 0)), d["right_hand"]["aim_valid"]))
PY
)" || row "debug plugin" "state query failed"
else
  row "debug plugin" "not reachable"
fi

say "dayz_openxr.log (since line $since)"
if [[ -f "$log" ]]; then
  total="$(wc -l < "$log")"
  row "lines" "$total total, $((total - since)) new; last write $(date -r "$log" '+%H:%M:%S')"
  new="$(tail -n +"$((since + 1))" "$log")"
  row "errors" "$(printf '%s\n' "$new" | grep -c '\[ERROR\]' || true)"
  row "fatal" "$(printf '%s\n' "$new" | grep -c '\] Fatal exception' || true)"
  row "guard skips" "$(printf '%s\n' "$new" | grep -c 'Skipped executeView' || true)"
  row "profile" "$(printf '%s\n' "$new" | grep -o 'runtime probe active: [^;]*' | sed -n '1p')"
  { printf '%s\n' "$new" | grep '\] Fatal exception\|Crash context' || true; } | sed -n '1,3p' | cut -c1-200 | sed 's/^/  ! /'
  { printf '%s\n' "$new" | grep -v '^\s*#\|D3D #\|ALPHA #\|^\s*event #\|^\s*$' || true; } | tail -n "$lines" | cut -c12-160 | sed 's/^/  /'
else
  row "log" "missing"
fi

say "crash dumps and DayZ logs"
for dir in "$profile_dir" "$documents_dir"; do
  [[ -d "$dir" ]] || continue
  { find "$dir" -maxdepth 1 \( -name '*.mdmp' -o -name '*.RPT' -o -name 'crash_*.log' \) -mmin -180 -printf '  %TY-%Tm-%Td %TH:%TM  %s  %p\n' 2>/dev/null || true; } | sort | tail -5
done
# The newest crash log: distinct reasons with counts (script VM exceptions repeat per frame).
newest() { [[ -d "$1" ]] || return 0; find "$1" -maxdepth 1 -name "$2" -printf '%T@ %p\n' 2>/dev/null | sort -rn | sed -n '1p' | cut -d' ' -f2-; }
latest_crash="$(newest "$profile_dir" 'crash_*.log')"
if [[ -n "$latest_crash" ]]; then
  row "latest crash log" "$(file_info "$latest_crash")  $(basename "$latest_crash")"
  { grep -h '^Reason:' "$latest_crash" || true; } | sort | uniq -c | sort -rn | sed -n '1,3p' | cut -c1-160 | sed 's/^/  ! /'
  { grep -h -m1 -A4 '^Stack trace' "$latest_crash" || true; } | tail -n +2 | sed -n '1,3p' | cut -c1-160 | sed 's/^/    /'
fi
latest_script="$(newest "$profile_dir" 'script_*.log')"
if [[ -n "$latest_script" ]]; then
  row "latest script log" "$(file_info "$latest_script")  $(basename "$latest_script")"
  { grep -i 'DayZVR\|SCRIPT.*ERROR\|Can.t compile' "$latest_script" || true; } | tail -3 | cut -c1-160 | sed 's/^/  /'
fi

say "local server (build/local-server/serverprofile)"
server_profile="$project_dir/build/local-server/serverprofile"
if [[ -d "$server_profile" ]]; then
  row "server mod" "$( [[ -d "$project_dir/build/local-server/@DayZVR_Server" ]] && echo deployed || echo "not deployed" )"
  server_rpt="$(newest "$server_profile" '*.RPT')"
  if [[ -n "$server_rpt" ]]; then
    row "server rpt" "$(file_info "$server_rpt")  $(basename "$server_rpt")"
    { grep -E 'DayZVR|Player connected|disconnected|SCRIPT.*ERROR|Can.t compile' "$server_rpt" || true; } | tail -4 | cut -c1-160 | sed 's/^/  /'
  fi
  if [[ -f "$server_profile/dayzvr/cmd.log" ]]; then
    row "cmd.log" "$(file_info "$server_profile/dayzvr/cmd.log")"
    tail -n 4 "$server_profile/dayzvr/cmd.log" | cut -c1-160 | sed 's/^/    /'
  fi
  [[ -f "$server_profile/dayzvr/cmd.txt" ]] && row "cmd.txt" "PENDING (server has not consumed it)"
fi

say "script bridge files"
for name in game.txt vr.txt; do
  path="$profile_dir/dayzvr/$name"
  if [[ -f "$path" ]]; then
    row "$name" "$(file_info "$path")"
    tr '\n' ' ' < "$path" | cut -c1-200 | sed 's/^/    /'; echo
  else
    row "$name" "missing"
  fi
done

say "result"
check_args=(--log "$log" --since "$since" --profile "$profile_dir" --marker "$offset_file")
if (( wait_seconds > 0 )); then check_args+=(--require-world); fi
log_passed=1
python3 "$script_dir/dayz_log.py" check "${check_args[@]}" || log_passed=0
if [[ -z "$pids" ]]; then row "RESULT" "DayZ is not running"; exit 1; fi
if (( wait_passed == 0 || log_passed == 0 )); then
  row "RESULT" "FAILED (see log classification above)"
  exit 1
fi
row "RESULT" "PASSED the requested log checks (headset output is not verified)"
exit 0
