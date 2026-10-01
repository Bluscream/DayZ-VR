#!/usr/bin/env bash
set -euo pipefail
IFS=$'\n\t'
script_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
project_dir="$(dirname -- "$script_dir")"
: "${XWIN_ROOT:?Set XWIN_ROOT to the xwin SDK directory}"
: "${OPENXR_SDK:?Set OPENXR_SDK to the generated OpenXR SDK checkout}"
: "${OPENXR_LOADER:?Set OPENXR_LOADER to the Windows x64 loader DLL}"
cmake --fresh -S "$project_dir" -B "$project_dir/build" \
  -DCMAKE_TOOLCHAIN_FILE="$project_dir/cmake/windows-clang.cmake" \
  -DCMAKE_BUILD_TYPE=Release -DOPENXR_SDK="$OPENXR_SDK" \
  -DOPENXR_LOADER="$OPENXR_LOADER"
cmake --build "$project_dir/build" --parallel "${BUILD_JOBS:-4}"
