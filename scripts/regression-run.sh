#!/usr/bin/env bash
# One-shot regression run on the headless sim rig: build + test gate, stop DayZ, deploy,
# restart the local server with the fresh server mod, launch the client on the Monado
# sim, wait until it is in-world, exercise the bridge (server command, client command,
# haptic test pulse, aim calibration sample) and stop the game again. Every step's full
# output goes to build/logs/regression-<stamp>/; the summary says PASS or FAIL per step.
#
#   scripts/regression-run.sh                full run (about 6 minutes)
#   scripts/regression-run.sh --skip-build   reuse the current build and deployment
#   scripts/regression-run.sh --keep         leave DayZ running at the end
#   scripts/regression-run.sh --no-sim       real headset: launch without the Monado sim runtime
#
# Needs the sim (scripts/xr-sim.sh start) and a set-up local server
# (scripts/local-server.sh setup). Exit status 0 only when every step passed.
# The game is stopped through scripts/build.sh --stop in its own step; this script's
# own command line never contains the game executable's name, so that stop cannot
# match it.
set -euo pipefail
IFS=$'\n\t'

script_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
project_dir="$(dirname -- "$script_dir")"
stamp="$(date +%Y%m%d-%H%M%S)"
run_dir="$project_dir/build/logs/regression-$stamp"
container_name="dayz-vr-local-server"
server_ready_seconds=120
in_world_seconds=300

skip_build=0
keep_running=0
use_sim=1
for arg in "$@"; do
  case "$arg" in
    --skip-build) skip_build=1 ;;
    --keep) keep_running=1 ;;
    --no-sim) use_sim=0 ;;
    -h | --help) sed -n '2,16p' "${BASH_SOURCE[0]}" | sed 's/^# \{0,1\}//'; exit 0 ;;
    *) printf 'unknown argument: %s\n' "$arg" >&2; exit 2 ;;
  esac
done

mkdir -p "$run_dir"
say() { printf '==> %s\n' "$*"; }
declare -a results=()
failed=0

# step NAME COMMAND...: runs the command with full output on the terminal and in the
# step log, records PASS/FAIL, never aborts the run (later steps still report).
step() {
  local name="$1"
  shift
  say "step $name: $*"
  local status=0
  "$@" 2>&1 | tee "$run_dir/$name.log" || status=${PIPESTATUS[0]}
  if [[ $status -eq 0 ]]; then
    results+=("PASS  $name")
  else
    results+=("FAIL  $name (exit $status, $run_dir/$name.log)")
    failed=1
  fi
  return 0
}

# Blocking wait (no polling loop) for the server mod's start-up line in the container
# log; the container is recreated by every restart, so following it from the start
# cannot miss the line, and timeout(1) bounds the wait. Invoked through step().
# shellcheck disable=SC2329
wait_server_ready() {
  # grep -m1 closes the pipe, which ends podman logs with SIGPIPE (141); that is the
  # expected way out, so pipefail is off for this pipeline only.
  (
    set +o pipefail
    timeout "$server_ready_seconds" podman logs -f "$container_name" 2>&1 |
      grep -m1 "server command channel started"
  )
}

# Invoked through step().
# shellcheck disable=SC2329
launch_client() {
  local -a sim_flag=()
  if [[ $use_sim -eq 1 ]]; then
    sim_flag=(--sim)
  fi
  (
    setsid "$script_dir/run-dayz-direct.sh" "${sim_flag[@]}" -- -connect=127.0.0.1 -port=2302 "-mod=@DayZVR" \
      >"$run_dir/launch.log" 2>&1 &
  )
  sleep 1
  cat "$run_dir/launch.log"
}

say "regression run $stamp, logs in $run_dir"
if [[ $skip_build -eq 0 ]]; then
  step build-deploy "$script_dir/build.sh" --stop --deploy
else
  step stop "$script_dir/build.sh" --stop
fi
step server-restart "$script_dir/local-server.sh" restart
step server-ready wait_server_ready
step launch launch_client
step in-world "$script_dir/dayz-status.sh" --wait "$in_world_seconds"
step server-cmd "$script_dir/dayz-cmd.sh" info
step client-cmd "$script_dir/dayz-cmd.sh" --client print regression
step haptic "$script_dir/dayz-vr-ctl.py" haptic
# Forty seconds of one-line samples: the aim loop is still converging right after the
# spawn (the first in-world frames run at half the usual rate), and the samples are the
# evidence when the calibration below disagrees with an earlier run.
step settle "$script_dir/dayz-vr-ctl.py" watch --interval 2 --count 20
# The calibration sample judges the aim loop: with controller aim on it passes when the
# camera sits on the right controller within 3 deg (static sim controllers).
step calibrate "$script_dir/dayz-vr-ctl.py" calibrate --seconds 15 --min-degrees 10
if [[ $keep_running -eq 0 ]]; then
  step stop-game "$script_dir/build.sh" --stop
fi

say "summary"
printf '  %s\n' "${results[@]}"
if [[ $failed -eq 0 ]]; then
  say "RESULT PASS"
  exit 0
fi
say "RESULT FAIL"
exit 1
