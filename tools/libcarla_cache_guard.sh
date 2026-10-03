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
shopt -s inherit_errexit  # a failing $(...) fails the script

# The first line of `<command> --version`; fails loudly if the command is
# missing. CC/CXX may carry a launcher or flags ("ccache gcc"): the version is
# that of what the build actually runs.
version_of() {
  local words
  read -ra words <<<"$1"
  if ! command -v "${words[0]}" >/dev/null; then
    echo "libcarla cache: '${words[0]}' not found (from '$1')" >&2
    return 1
  fi
  "${words[@]}" --version 2>&1 | sed -n 1p
}

toolchain() {
  local cc cxx cmake ninja=""
  cc=$(version_of "${CC:-cc}")
  cxx=$(version_of "${CXX:-c++}")
  cmake=$(version_of cmake)
  if command -v ninja >/dev/null; then ninja=$(version_of ninja); fi
  echo "machine=$(uname -m)"
  echo "cc=${cc}"
  echo "cxx=${cxx}"
  echo "cmake=${cmake}"
  if [ -n "$ninja" ]; then echo "ninja=${ninja}"; fi
}

if [ "${1:-}" = --toolchain ]; then
  tc=$(toolchain)
  echo "$tc"
  exit 0
fi

usage="usage: libcarla_cache_guard.sh <build-dir> <CARLA commit SHA> | --toolchain"
dir=${1:?$usage}
sha=${2:?$usage}
stamp="$dir/.tsc-libcarla-fingerprint"
tc=$(toolchain)
want=$(echo "carla=${sha}"; echo "$tc")

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
