#!/usr/bin/env bash
# Exercise the real Windows DLL in an isolated prefix on the project drive.
# Uses the installed Proton/runtime; never installs anything or opens DayZ.
set -euo pipefail
IFS=$'\n\t'
script_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
project_dir="$(dirname -- "$script_dir")"
steam_root="${STEAM_ROOT:-$HOME/.local/share/Steam}"
steam_library="${STEAM_LIBRARY:-/run/media/system/Data/Games/Steam}"
proton_dir="${PROTON_DIR:-$steam_root/compatibilitytools.d/Proton-GE Latest}"
slr_dir="${SLR_DIR:-$steam_library/steamapps/common/SteamLinuxRuntime_4}"
test_dir="$project_dir/build/debug-plugin-test"
[[ -x "$proton_dir/files/bin/wine" && -x "$slr_dir/_v2-entry-point" ]] || {
  printf 'error: installed Proton and Steam Linux Runtime are required\n' >&2; exit 1;
}
mkdir -p "$test_dir/compat/0"
[[ -f "$project_dir/build/debug_plugin_lifecycle_test.exe" ]] || {
  printf 'error: build the lifecycle test first\n' >&2; exit 1;
}
# Keep this executable separate from dxgi.dll: the socket test must never load the game proxy.
cp "$project_dir/build/debug_plugin_lifecycle_test.exe" "$project_dir/build/dayz_openxr_debug.dll" "$test_dir/"
cd "$test_dir"
timeout --kill-after=10 90 env \
  WINEPREFIX="$test_dir/compat/0/pfx" \
  "$slr_dir/_v2-entry-point" -- \
  "$proton_dir/files/bin/wine" "$test_dir/debug_plugin_lifecycle_test.exe"
