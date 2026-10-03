#!/usr/bin/env bash
# Guards a LibCarla build directory restored from the CI cache (see
# docs/releasing.md, "LibCarla build cache").
#
#   tools/libcarla_cache_guard.sh <build-dir> <CARLA commit SHA>
#   tools/libcarla_cache_guard.sh --toolchain
#
# Records what the directory is built with (CARLA SHA and toolchain) and
# empties it first if it was built with anything else, so a mismatch is a
# clean build, as on a cache miss. --toolchain only prints the toolchain part,
# which CI hashes into its cache key.
set -euo pipefail

toolchain() {
  echo "machine=$(uname -m)"
  echo "cc=$("${CC:-cc}" --version 2>&1 | head -n 1)"
  echo "cxx=$("${CXX:-c++}" --version 2>&1 | head -n 1)"
  echo "cmake=$(cmake --version 2>&1 | head -n 1)"
}

if [ "${1:-}" = --toolchain ]; then
  toolchain
  exit 0
fi

usage="usage: libcarla_cache_guard.sh <build-dir> <CARLA commit SHA> | --toolchain"
dir=${1:?$usage}
sha=${2:?$usage}
stamp="$dir/.tsc-libcarla-fingerprint"
want=$(echo "carla=${sha}"; toolchain)

mkdir -p "$dir"
if [ -z "$(ls -A "$dir")" ]; then
  echo "libcarla cache: $dir is empty; building from scratch"
elif [ -f "$stamp" ] && [ "$(cat "$stamp")" = "$want" ]; then
  echo "libcarla cache: reusing $dir"
else
  echo "libcarla cache: $dir was built with something else; starting clean"
  if [ -f "$stamp" ]; then diff "$stamp" - <<<"$want" || true; fi
  # The directory itself may be a mount point: empty it, do not remove it.
  find "$dir" -mindepth 1 -maxdepth 1 -exec rm -rf {} +
fi
printf '%s\n' "$want" > "$stamp"
printf '%s\n' "$want"
