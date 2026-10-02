#!/usr/bin/env bash
# Switch the deployed dayz_openxr.ini between feature presets without touching the
# other keys. Section-aware: only the listed section/key pairs change.
#
#   scripts/ini-preset.sh rendering-only [ini]   stereo rendering + head tracking only:
#                                                controllers, bridge, haptics, HUD quad,
#                                                vehicle, melee, stance and vignette off
#   scripts/ini-preset.sh full [ini]             the same keys back on
#
# [ini] defaults to the deployed game ini. Hooks, keep_focus, the GUI quad and the
# debug plugin stay as they are (needed for fps, menus and the automated tests).
set -euo pipefail
preset=${1:-}
ini=${2:-/run/media/system/Data/Games/Steam/steamapps/common/DayZ/dayz_openxr.ini}
case "$preset" in
  rendering-only) value=false ;;
  full) value=true ;;
  *) echo "usage: $0 rendering-only|full [ini]" >&2; exit 2 ;;
esac
[[ -f "$ini" ]] || { echo "ini not found: $ini" >&2; exit 1; }
python3 - "$ini" "$value" <<'PY'
import sys, re
path, value = sys.argv[1], sys.argv[2]
keys = {
    "controls": ["enabled", "show_controller_axes", "show_gui_ray", "show_direction_rays"],
    "hud": ["ammo_quad"],
    "haptics": ["fire"],
    "vehicle": ["lock_view", "steering"],
    "input": ["direct_actions"],
    "bridge": ["enabled", "ammo_counter", "dashboard"],
    "melee": ["motion_swing"],
    "stance": ["physical"],
    "comfort": ["vignette"],
}
# Features that are off in both presets (not ready for general use).
always_off = {"melee", "stance", "comfort"}
section, changed = None, []
out = []
for line in open(path, encoding="utf-8").read().splitlines(keepends=True):
    m = re.match(r"^\[(.+)\]\s*$", line)
    if m:
        section = m.group(1)
    else:
        m = re.match(r"^([A-Za-z_]+)=(.*?)(\r?\n?)$", line)
        if m and section in keys and m.group(1) in keys[section]:
            new = "false" if section in always_off else value
            if m.group(2) != new:
                changed.append(f"{section}.{m.group(1)}: {m.group(2)} -> {new}")
                line = f"{m.group(1)}={new}{m.group(3)}"
    out.append(line)
open(path, "w", encoding="utf-8").write("".join(out))
print("\n".join(changed) if changed else "no changes")
PY
