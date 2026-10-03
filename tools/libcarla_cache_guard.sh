#!/usr/bin/env bash
# Guards a LibCarla build directory restored from the CI cache.
#
#   tools/libcarla_cache_guard.sh <build-dir> <CARLA commit SHA>
#
# The workflows cache the whole CMake build directory of a libcarla build
# (LibCarla, its fetched and built dependencies, and the shim), keyed by the
# CARLA commit SHA. The cache key already names the SHA and the toolchain;
# this is the second line of defence, run right before configuring: it records
# what the directory was built with (CARLA SHA, compilers, CMake, machine) and
# empties the directory if that differs from the current build, so a restored
# directory is reused only for the very same CARLA commit and toolchain. On a
# mismatch the build is a clean one, as on a cache miss.
set -euo pipefail

dir=${1:?usage: libcarla_cache_guard.sh <build-dir> <CARLA commit SHA>}
sha=${2:?usage: libcarla_cache_guard.sh <build-dir> <CARLA commit SHA>}
stamp="$dir/.tsc-libcarla-fingerprint"

fingerprint() {
  echo "carla=${sha}"
  echo "machine=$(uname -m)"
  echo "cc=$("${CC:-cc}" --version 2>&1 | head -n 1)"
  echo "cxx=$("${CXX:-c++}" --version 2>&1 | head -n 1)"
  echo "cmake=$(cmake --version 2>&1 | head -n 1)"
}
want=$(fingerprint)

mkdir -p "$dir"
if [ -n "$(ls -A "$dir")" ]; then
  if [ -f "$stamp" ] && [ "$(cat "$stamp")" = "$want" ]; then
    echo "libcarla cache: reusing $dir"
    printf '%s\n' "$want"
    exit 0
  fi
  echo "libcarla cache: $dir was built with something else; starting clean"
  if [ -f "$stamp" ]; then diff <(cat "$stamp") <(printf '%s\n' "$want") || true; fi
  # The directory itself may be a mount point: empty it, do not remove it.
  find "$dir" -mindepth 1 -maxdepth 1 -exec rm -rf {} +
else
  echo "libcarla cache: $dir is empty; building from scratch"
fi
printf '%s\n' "$want" > "$stamp"
printf '%s\n' "$want"
