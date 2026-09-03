#!/usr/bin/env bash
set -euo pipefail

# Manual build helper. Builds the Debug and Release trees for one or more
# targets, defaulting to just `le_shell` for fast iteration.
#
# Usage: build.sh [--debug-only|--release-only] [--full] [-j N] [target...]
#   --debug-only    only build the Debug tree
#   --release-only  only build the Release tree
#   --full          build everything (no --target filter) instead of the
#                   default `le_shell`-only build
#   -j N            parallel jobs (default: nproc/hw.ncpu)
#   target...       explicit CMake target(s) to build instead of le_shell

cd "$(dirname "$0")/.."

JOBS=$(nproc 2>/dev/null || sysctl -n hw.ncpu 2>/dev/null || echo 4)
DO_DEBUG=1
DO_RELEASE=1
TARGETS=()

while [[ $# -gt 0 ]]; do
  case "$1" in
    --debug-only) DO_RELEASE=0; shift ;;
    --release-only) DO_DEBUG=0; shift ;;
    --full) TARGETS=(__full__); shift ;;
    -j) JOBS="$2"; shift 2 ;;
    -h|--help)
      sed -n '4,13{s/^# \{0,1\}//;p}' "$0"
      exit 0
      ;;
    *) TARGETS+=("$1"); shift ;;
  esac
done

if [[ ${#TARGETS[@]} -eq 0 ]]; then
  TARGETS=(le_shell)
fi

# Prefer the `-linux`-suffixed trees when present, falling back to the
# unsuffixed names used on macOS dev machines.
debug_dir=build-linux
[[ -d "$debug_dir" ]] || debug_dir=build
release_dir=build_release-linux
[[ -d "$release_dir" ]] || release_dir=build_release

build_tree() {
  local dir="$1" label="$2"
  if [[ ! -d "$dir" ]]; then
    echo "==> Skipping $label ($dir not configured)"
    return
  fi
  if [[ "${TARGETS[0]}" == "__full__" ]]; then
    echo "==> Building $dir ($label, full)"
    cmake --build "$dir" -j"$JOBS"
  else
    echo "==> Building $dir ($label): ${TARGETS[*]}"
    cmake --build "$dir" -j"$JOBS" --target "${TARGETS[@]}"
  fi
}

[[ $DO_DEBUG -eq 1 ]] && build_tree "$debug_dir" Debug
[[ $DO_RELEASE -eq 1 ]] && build_tree "$release_dir" Release

exit 0
