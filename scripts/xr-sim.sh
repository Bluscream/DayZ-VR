#!/usr/bin/env bash
# Headless OpenXR runtime for testing the DayZ VR proxy without a headset.
#
# Runs Monado (Debian package inside the build-box container) with its simulated
# HMD driver and null compositor, and exports the client library plus a runtime
# manifest to build/monado-sim/ so a Proton game can load it through
# wineopenxr exactly like WiVRn.
#
#   scripts/xr-sim.sh start    install (once), start monado-service, export the client library
#   scripts/xr-sim.sh stop     stop the service
#   scripts/xr-sim.sh status   show whether the service and its socket are up
#   scripts/xr-sim.sh env      print the environment a game needs to use this runtime
#
# Environment overrides: BUILD_CONTAINER (default build-box), SIM_FRAMERATE (default 90).
set -euo pipefail
IFS=$'\n\t'

script_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
project_dir="$(dirname -- "$script_dir")"
sim_dir="$project_dir/build/monado-sim"
log_dir="$project_dir/build/logs"
container="${BUILD_CONTAINER:-build-box}"
runtime_json="$sim_dir/openxr_monado_sim.json"
client_lib="$sim_dir/libopenxr_monado.so"
socket_path="${XDG_RUNTIME_DIR:?}/monado_comp_ipc"
pid_file="$sim_dir/monado-service.pid"
packages="monado-service monado-cli libopenxr1-monado mesa-vulkan-drivers"

say() { printf '==> %s\n' "$*"; }
die() { printf 'error: %s\n' "$*" >&2; exit 1; }

in_container_sh() {
  command -v distrobox >/dev/null || die "distrobox is required"
  distrobox enter "$container" -- bash -c "$1"
}

# Only the service itself: its distrobox/podman wrappers exit on their own and
# signalling the interactive podman exec can take the calling shell down with it.
service_pid() {
  pgrep -x monado-service || true
}

install_runtime() {
  if ! in_container_sh "dpkg -s $packages >/dev/null 2>&1"; then
    say "installing $packages in $container"
    in_container_sh "sudo apt-get install -y $packages"
  fi
  mkdir -p "$sim_dir" "$log_dir"
  in_container_sh "cp /usr/lib/x86_64-linux-gnu/libopenxr_monado.so \"$client_lib\""
  cat > "$runtime_json" <<JSON
{
    "file_format_version": "1.0.0",
    "runtime": {
        "name": "Monado simulated HMD (headless)",
        "library_path": "$client_lib"
    }
}
JSON
}

cmd_start() {
  if [[ -n "$(service_pid)" ]]; then
    say "monado-service already running (pid $(service_pid | tr '\n' ' '))"
    return
  fi
  install_runtime
  say "starting monado-service (simulated HMD, null compositor); log: $log_dir/monado-sim.log"
  # XRT_NO_STDIN keeps the service from reading the terminal; XRT_COMPOSITOR_NULL
  # avoids opening any window; SIMULATED_ENABLE forces the simulated builder.
  # XRT_COMPOSITOR_DEFAULT_FRAMERATE paces the null compositor like a real HMD.
  distrobox enter "$container" -- env XRT_NO_STDIN=1 XRT_COMPOSITOR_NULL=1 \
    XRT_COMPOSITOR_DEFAULT_FRAMERATE="${SIM_FRAMERATE:-90}" \
    SIMULATED_ENABLE=1 XRT_DEBUG_GUI=0 monado-service \
    > "$log_dir/monado-sim.log" 2>&1 &
  echo $! > "$pid_file"
  # The socket appears well within a second of startup; one bounded check.
  sleep 3
  [[ -S "$socket_path" ]] ||
    die "monado-service did not create $socket_path (see $log_dir/monado-sim.log)"
  cmd_status
}

cmd_stop() {
  local pids
  pids="$(service_pid)"
  if [[ -z "$pids" ]]; then
    say "monado-service is not running"
  else
    # shellcheck disable=SC2086 # pids is a whitespace-separated list by construction
    kill -TERM $pids
    local pid
    for pid in $pids; do
      timeout 15 tail --pid="$pid" -f /dev/null || die "pid $pid did not exit"
    done
    say "monado-service stopped"
  fi
  rm -f "$pid_file"
  [[ -S "$socket_path" ]] && rm -f "$socket_path"
  return 0
}

cmd_status() {
  local pids
  pids="$(service_pid)"
  if [[ -n "$pids" ]]; then
    say "monado-service running (pid ${pids//$'\n'/ })"
  else
    say "monado-service not running"
  fi
  if [[ -S "$socket_path" ]]; then say "socket present: $socket_path"; else say "socket missing: $socket_path"; fi
  if [[ -f "$runtime_json" ]]; then say "runtime manifest: $runtime_json"; else say "runtime manifest not exported yet"; fi
}

cmd_env() {
  printf 'XR_RUNTIME_JSON=%s\n' "$runtime_json"
  printf 'PRESSURE_VESSEL_IMPORT_OPENXR_1_RUNTIMES=1\n'
  printf 'PRESSURE_VESSEL_FILESYSTEMS_RW=%s\n' "$socket_path"
}

case "${1:-}" in
  start) cmd_start ;;
  stop) cmd_stop ;;
  status) cmd_status ;;
  env) cmd_env ;;
  -h | --help | "") sed -n '2,14p' "${BASH_SOURCE[0]}" | sed 's/^# \{0,1\}//'; [[ -n "${1:-}" ]] || exit 2 ;;
  *) die "unknown command: $1" ;;
esac
