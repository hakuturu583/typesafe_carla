#!/usr/bin/env bash
# Guards a LibCarla build directory restored from the CI cache (see
# docs/releasing.md, "LibCarla build cache").
#
#   tools/libcarla_cache_guard.sh <build-dir> <CARLA commit SHA>
#   tools/libcarla_cache_guard.sh --toolchain
#   tools/libcarla_cache_guard.sh --abi
#
# Records what the directory is built with (CARLA SHA and toolchain) and
# empties it first if it was built with anything else, so a mismatch is a
# clean build, as on a cache miss. --toolchain only prints the toolchain part,
# which CI hashes into its cache key.
#
# --abi prints what a LibCarla prebuilt (static libraries and headers; see
# docs/releasing.md, "LibCarla prebuilt") must share with the compiler that
# links it: the target triple, the C and C++ compilers and their versions,
# the C++ standard library (release and dual-ABI mode), the glibc headers'
# version, the data model, and macros that change layouts or mangling
# (_GLIBCXX_DEBUG, _FILE_OFFSET_BITS, RTTI, exceptions, ...). It leaves out
# what --toolchain has but a relocatable prefix does not depend on: the
# command's name ("g++-11" vs "c++"), the distribution's package revision of
# the compiler, CMake and Ninja. CFLAGS and CXXFLAGS are applied, so e.g.
# -D_GLIBCXX_USE_CXX11_ABI=0 or -m32 changes it.
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

# "<compiler> <version>" from a compiler's predefined macros, plus for C++ the
# standard library, and the glibc headers' version.
#   abi_of <CC or CXX value> <c|c++> <extra flags>
abi_of() {
  local words flags header macros
  read -ra words <<<"$1"
  read -ra flags <<<"$3"
  if ! command -v "${words[0]}" >/dev/null; then
    echo "libcarla abi: '${words[0]}' not found (from '$1')" >&2
    return 1
  fi
  header='#include <stddef.h>'
  if [ "$2" = c++ ]; then header='#include <cstddef>'; fi
  # <limits.h> pulls in <features.h>, which defines __GLIBC__.
  macros=$(printf '%s\n#include <limits.h>\n' "$header" |
           "${words[@]}" "${flags[@]}" -dM -E -x "$2" -)
  awk -v lang="$2" '
    $1 == "#define" { m[$2] = $3 }
    END {
      if ("__clang__" in m)
        printf "clang %s.%s.%s", m["__clang_major__"], m["__clang_minor__"], m["__clang_patchlevel__"]
      else if ("__GNUC__" in m)
        printf "gcc %s.%s.%s", m["__GNUC__"], m["__GNUC_MINOR__"], m["__GNUC_PATCHLEVEL__"]
      else
        printf "unknown-compiler"
      if (lang == "c++") {
        if ("_LIBCPP_VERSION" in m)
          printf ", libc++ %s", m["_LIBCPP_VERSION"]
        else if ("__GLIBCXX__" in m)
          printf ", libstdc++ %s (%s) cxx11-abi=%s", m["_GLIBCXX_RELEASE"], m["__GLIBCXX__"], m["_GLIBCXX_USE_CXX11_ABI"]
        else
          printf ", unknown-stdlib"
      }
      if ("__GLIBC__" in m) printf ", glibc %s.%s", m["__GLIBC__"], m["__GLIBC_MINOR__"]
      # Data model and the macros that change layouts or mangled names:
      # -m32/-mx32, long double, _FILE_OFFSET_BITS/_TIME_BITS, libstdc++
      # debug mode, RTTI and exceptions.
      n = split("__SIZEOF_POINTER__ __SIZEOF_LONG__ __SIZEOF_LONG_DOUBLE__ _FILE_OFFSET_BITS _TIME_BITS _GLIBCXX_DEBUG _GLIBCXX_DEBUG_PEDANTIC _GLIBCXX_PARALLEL _LIBCPP_ABI_VERSION __GXX_RTTI __EXCEPTIONS", keys, " ")
      for (i = 1; i <= n; i++)
        if (keys[i] in m) printf ", %s=%s", keys[i], m[keys[i]]
      printf "\n"
    }' <<<"$macros"
}

abi() {
  local cc cxx words flags target
  cc=$(abi_of "${CC:-cc}" c "${CFLAGS:-}")
  cxx=$(abi_of "${CXX:-c++}" c++ "${CXXFLAGS:-}")
  read -ra words <<<"${CXX:-c++}"
  read -ra flags <<<"${CXXFLAGS:-}"
  target=$("${words[@]}" "${flags[@]}" -dumpmachine)
  echo "target=${target}"
  echo "cc=${cc}"
  echo "cxx=${cxx}"
}

if [ "${1:-}" = --abi ]; then
  out=$(abi)
  echo "$out"
  exit 0
fi

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
