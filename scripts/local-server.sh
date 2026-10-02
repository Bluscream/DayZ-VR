#!/usr/bin/env bash
# Vanilla DayZ dedicated server on this machine for testing the VR proxy without
# touching the user's real server. Runs inside the same container image the
# Pterodactyl egg uses (ghcr.io/parkervcp/games:dayz) with host networking, from a
# private copy of Steam's "DayZ Server" (app 223350) install, with BattlEye patched
# out by pterodactyl-eggs/dayz-standalone/patch_be.pl so a -nobe client can join.
#
#   scripts/local-server.sh setup    copy the Steam install, patch BattlEye, write serverDZ.cfg
#   scripts/local-server.sh start    start the server container (port 2302, query 2305)
#   scripts/local-server.sh stop     stop it
#   scripts/local-server.sh status   container state and the last log lines
#   scripts/local-server.sh logs     follow the server log (Ctrl-C to stop following)
#
# Join from the client with: scripts/run-dayz-direct.sh --sim -- -connect=127.0.0.1 -port=2302
# Environment overrides: DAYZ_SERVER_SRC, EGGS_DIR, SERVER_IMAGE, SERVER_PORT.
set -euo pipefail
IFS=$'\n\t'

script_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
project_dir="$(dirname -- "$script_dir")"
server_dir="$project_dir/build/local-server"
server_src="${DAYZ_SERVER_SRC:-/run/media/system/Data/Games/Steam/steamapps/common/DayZServer}"
eggs_dir="${EGGS_DIR:-/run/media/system/Data/Projects/pterodactyl-eggs/dayz-standalone}"
image="${SERVER_IMAGE:-ghcr.io/parkervcp/games:dayz}"
port="${SERVER_PORT:-2302}"
container_name="dayz-vr-local-server"

say() { printf '==> %s\n' "$*"; }
die() { printf 'error: %s\n' "$*" >&2; exit 1; }

cmd_setup() {
  [[ -f "$server_src/DayZServer" ]] || die "DayZ Server (app 223350) is not installed at $server_src"
  [[ -f "$eggs_dir/patch_be.pl" ]] || die "patch_be.pl not found in $eggs_dir"
  command -v podman >/dev/null || die "podman is required"
  mkdir -p "$server_dir"
  say "copying the server install to $server_dir (first run copies ~3.8 GB)"
  rsync -a --delete --exclude serverprofile --exclude serverDZ.cfg "$server_src/" "$server_dir/"
  mkdir -p "$server_dir/serverprofile" "$server_dir/.steam/sdk64"
  cp -f "$server_dir/steamclient.so" "$server_dir/.steam/sdk64/steamclient.so"
  say "patching BattlEye out of the server binary"
  perl "$eggs_dir/patch_be.pl" "$server_dir/DayZServer"
  cat > "$server_dir/serverDZ.cfg" <<CFG
hostname = "DayZ-VR local test server";
password = "";
passwordAdmin = "";
enableWhitelist = 0;
maxPlayers = 4;
verifySignatures = 0;
forceSameBuild = 0;
disableVoN = 1;
disable3rdPerson = 0;
disableCrosshair = 0;
serverTime = "SystemTime";
serverTimeAcceleration = 1;
serverNightTimeAcceleration = 1;
serverTimePersistent = 0;
guaranteedUpdates = 1;
loginQueueConcurrentPlayers = 5;
loginQueueMaxPlayers = 500;
instanceId = 1;
storageAutoFix = 1;
lootHistory = 1;
storeHouseStateDisabled = false;
allowFilePatching = 1;
steamQueryPort = 2305;
enableDebugMonitor = 1;
logAverageFps = 60;
logMemory = 60;
logPlayers = 60;
logFile = "server_console.log";
BattlEye = 0;
class Missions
{
    class DayZ
    {
        template = "dayzOffline.chernarusplus";
    };
};
CFG
  say "pulling $image if needed"
  podman image exists "$image" || podman pull "$image"
  say "setup complete"
}

cmd_start() {
  [[ -f "$server_dir/DayZServer" && -f "$server_dir/serverDZ.cfg" ]] || die "run 'setup' first"
  if podman container exists "$container_name"; then
    podman rm -f "$container_name" >/dev/null
  fi
  # The test-command mod (enforce/DayZVR_Server, deployed by build.sh --deploy) lets
  # scripts/dayz-cmd.sh spawn gear/vehicles and teleport the connected player.
  local server_mods=""
  if [[ -d "$server_dir/@DayZVR_Server" ]]; then
    server_mods="-serverMod=@DayZVR_Server"
    say "loading server mod @DayZVR_Server"
  fi
  say "starting $container_name on UDP $port (log: scripts/local-server.sh logs)"
  # --userns=keep-id keeps the host uid so the image's 'container' user (uid 1000)
  # owns the mounted files; host networking so the client reaches 127.0.0.1:$port.
  podman run -d --name "$container_name" --network host --userns=keep-id \
    -v "$server_dir:/home/container:Z" -w /home/container --entrypoint /bin/bash "$image" \
    -c "./DayZServer -config=serverDZ.cfg -port=$port -profiles=serverprofile -BEpath=battleye -dologs -adminlog -limitFPS=60 $server_mods" \
    >/dev/null
  cmd_status
}

cmd_stop() {
  if podman container exists "$container_name"; then
    podman stop -t 20 "$container_name" >/dev/null && say "stopped"
    podman rm -f "$container_name" >/dev/null
  else
    say "server container is not running"
  fi
}

cmd_status() {
  if podman container exists "$container_name"; then
    podman ps -a --filter "name=$container_name" --format 'status: {{.Status}}'
    podman logs --tail 8 "$container_name" 2>&1 | cut -c1-200
  else
    say "server container does not exist"
  fi
}

case "${1:-}" in
  setup) cmd_setup ;;
  start) cmd_start ;;
  stop) cmd_stop ;;
  restart) cmd_stop; cmd_start ;;
  status) cmd_status ;;
  logs) podman logs -f "$container_name" ;;
  -h | --help | "") sed -n '2,16p' "${BASH_SOURCE[0]}" | sed 's/^# \{0,1\}//'; [[ -n "${1:-}" ]] || exit 2 ;;
  *) die "unknown command: $1" ;;
esac
