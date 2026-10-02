#!/usr/bin/env bash
# Build gate and release build of the config editor (tools/config-editor) inside the
# build-box container: cargo fmt --check, clippy (pedantic, warnings are errors), tests,
# docs, then the Linux and Windows release binaries, copied to build/config-editor/.
#
#   scripts/build-config-editor.sh            gate + both release targets
#   scripts/build-config-editor.sh --check    gate only (no release build)
#
# Output is never filtered: every cargo line reaches the terminal.
set -euo pipefail
IFS=$'\n\t'

script_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
project_dir="$(dirname -- "$script_dir")"
crate_dir="$project_dir/tools/config-editor"
out_dir="$project_dir/build/config-editor"
container="build-box"
windows_target="x86_64-pc-windows-gnu"

check_only=0
for arg in "$@"; do
  case "$arg" in
    --check) check_only=1 ;;
    -h | --help) sed -n '2,9p' "${BASH_SOURCE[0]}" | sed 's/^# \{0,1\}//'; exit 0 ;;
    *) printf 'unknown argument: %s\n' "$arg" >&2; exit 2 ;;
  esac
done

say() { printf '==> %s\n' "$*"; }

# in_box COMMAND...: runs one cargo invocation in the crate directory inside build-box.
in_box() {
  distrobox enter "$container" -- bash -c "cd \"\$1\" && shift && \"\$@\"" -- "$crate_dir" "$@"
}

say "config editor gate (container: $container)"
in_box cargo fmt --all -- --check
in_box cargo clippy --all-targets --all-features -- -D warnings
in_box cargo test
in_box env RUSTDOCFLAGS="-D warnings" cargo doc --no-deps
if [[ $check_only -eq 1 ]]; then
  say "gate passed (--check, no release build)"
  exit 0
fi

say "release builds"
in_box cargo build --release
in_box cargo build --release --target "$windows_target"

mkdir -p "$out_dir"
cp -f "$crate_dir/target/release/dayz-vr-config" "$out_dir/dayz-vr-config"
cp -f "$crate_dir/target/$windows_target/release/dayz-vr-config.exe" "$out_dir/dayz-vr-config.exe"
say "binaries in $out_dir"
ls -l "$out_dir"
