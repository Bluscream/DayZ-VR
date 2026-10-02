#!/usr/bin/env bash
# Send a test command to the local DayZ server's @DayZVR_Server mod and print the result.
#
#   scripts/dayz-cmd.sh hands M4A1                 weapon into hands with a full magazine
#   scripts/dayz-cmd.sh give Mag_STANAG_30Rnd 3    items into the inventory
#   scripts/dayz-cmd.sh spawn OffroadHatchback     drivable vehicle 4 m ahead
#   scripts/dayz-cmd.sh tp nwaf                    teleport (presets or "tp x z" / "tp x y z")
#   scripts/dayz-cmd.sh heal | kill | time 12 | weather clear
#   scripts/dayz-cmd.sh --client raise 1          client-side hooks (@DayZVR mod): raise 0|1,
#                                                 print <text>
#
# The command is appended to build/local-server/serverprofile/dayzvr/cmd.txt (the
# server's $profile:dayzvr/, polled every 0.5 s); the mod deletes the file and appends
# "<command> -> <result>" to cmd.log, which this script waits for (bounded, no loop).
# See enforce/DayZVR_Server/scripts/5_Mission/DayZVRServerCmd.c for the command list.
set -euo pipefail
IFS=$'\n\t'
script_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
project_dir="$(dirname -- "$script_dir")"
profile_dir="$project_dir/build/local-server/serverprofile"
cmd_dir="$profile_dir/dayzvr"
timeout_seconds="${DAYZ_CMD_TIMEOUT:-10}"
client_profile="${STEAM_LIBRARY:-/run/media/system/Data/Games/Steam}/steamapps/compatdata/221100/pfx/drive_c/users/steamuser/AppData/Local/DayZ"
target=server
if [[ "${1:-}" == "--client" ]]; then
  target=client
  shift
  profile_dir="$client_profile"
  cmd_dir="$profile_dir/dayzvr"
fi

if (( $# == 0 )) || [[ "$1" == "-h" || "$1" == "--help" ]]; then
  sed -n '2,13p' "${BASH_SOURCE[0]}" | sed 's/^# \{0,1\}//'
  exit "$(( $# == 0 ? 2 : 0 ))"
fi
if [[ "$target" == server ]]; then
  [[ -d "$profile_dir" ]] || { echo "error: local server profile missing ($profile_dir); run scripts/local-server.sh setup/start" >&2; exit 1; }
  podman ps --format '{{.Names}}' 2>/dev/null | grep -q '^dayz-vr-local-server$' ||
    echo "warning: the local server container is not running" >&2
  [[ -d "$project_dir/build/local-server/@DayZVR_Server" ]] ||
    echo "warning: @DayZVR_Server is not deployed to the local server (scripts/build.sh --deploy, then local-server.sh restart)" >&2
  cmd_file="$cmd_dir/cmd.txt"
  log="$cmd_dir/cmd.log"
else
  [[ -d "$profile_dir" ]] || { echo "error: client profile missing ($profile_dir)" >&2; exit 1; }
  pgrep -f 'DayZ_x6[4]\.exe' >/dev/null || echo "warning: the game is not running" >&2
  cmd_file="$cmd_dir/client_cmd.txt"
  log="$cmd_dir/client_cmd.log"
fi

mkdir -p "$cmd_dir"
# IFS is newline/tab here, so join the words with spaces explicitly.
command_text="$(IFS=' '; printf '%s' "$*")"
touch "$log"
before="$(wc -l < "$log")"
printf '%s\n' "$command_text" >> "$cmd_file"
# Wait for the matching result line; bounded by timeout, no polling loop.
if ! timeout "$timeout_seconds" bash -c \
    "tail -n +$((before + 1)) -F '$log' 2>/dev/null | grep -m1 -F '$command_text -> '"; then
  echo "error: no result within ${timeout_seconds}s (server mod not loaded, no player connected, or server down)" >&2
  exit 1
fi
