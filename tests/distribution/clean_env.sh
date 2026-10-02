#!/usr/bin/env bash
# Design section 44 success criteria, checked in a clean container:
#
#   uv sync && uv run typesafe-codon build -release main.py && ./main
#
# with no system CARLA Python package, no manually installed Codon, no
# CODON_PATH and no LibCarla installation. The container gets only uv, g++
# and zlib (Codon links executables with `g++ ... -lz`) and a directory of
# wheels (typesafe-carla + typesafe-carla-toolchain) standing in for PyPI.
# The compile-time rejection reuses tests/compile/fail/control_with_transform.
#
# usage: tests/distribution/clean_env.sh WHEEL_DIR [CARLA_PORT] [IMAGE]
# The CARLA server must listen on localhost:CARLA_PORT (host network).
set -euo pipefail
wheels=$(realpath "${1:?usage: clean_env.sh WHEEL_DIR [CARLA_PORT] [IMAGE]}")
port=${2:-2000}
image=${3:-ubuntu:24.04}
tests=$(cd "$(dirname "$0")/.." && pwd)
version=$(sed -n 's/^__version__ = "\(.*\)"/\1/p' "$tests/../python/typesafe_carla/__init__.py")
: "${version:?could not read __version__}"
uv_bin=$(command -v uv)

docker run --rm --net=host \
  -v "$wheels":/wheels:ro -v "$tests":/src:ro -v "$uv_bin":/usr/local/bin/uv:ro \
  -e TSC_CARLA_PORT="$port" -e TSC_VERSION="$version" "$image" bash -euo pipefail -c '
    export DEBIAN_FRONTEND=noninteractive
    apt-get update -qq >/dev/null && apt-get install -y -qq g++ zlib1g-dev ca-certificates >/dev/null
    echo "== clean environment"
    if command -v codon; then echo "codon is on PATH"; exit 1; fi
    echo "no codon on PATH"
    if [ -n "${CODON_PATH:-}" ]; then echo "CODON_PATH is set"; exit 1; fi
    echo "CODON_PATH unset"

    mkdir /app && cd /app
    cp /src/distribution/main.py .
    cp /src/compile/fail/control_with_transform.codon bad.py
    cat > pyproject.toml <<TOML
[project]
name = "my-carla-project"
version = "0.1.0"
requires-python = ">=3.10"
dependencies = ["typesafe-carla==${TSC_VERSION}"]

[tool.uv]
# Only the wheels under test, never a published release.
find-links = ["/wheels"]
no-index = true
TOML
    echo "== uv sync"
    uv sync 2>&1 | tail -3
    if uv run python -c "import carla" 2>/dev/null; then echo "a CARLA Python package is installed"; exit 1; fi
    echo "no CARLA Python package"
    uv run typesafe-codon info

    echo "== compile-time rejection"
    if uv run typesafe-codon build -release -o bad bad.py 2>err.txt; then
      echo "bad.py compiled but must not"; exit 1
    fi
    expected=$(sed -n "s/^# expect-error: //p" bad.py)
    if ! sed "s/\x1b\[[0-9;]*m//g" err.txt | grep -qF "$expected"; then
      echo "bad.py failed for another reason:"; cat err.txt; exit 1
    fi
    sed "s/\x1b\[[0-9;]*m//g" err.txt | head -1
    if [ -e bad ]; then echo "an executable was produced"; exit 1; fi
    echo "no executable produced"

    echo "== typesafe-codon build -release main.py && ./main"
    uv run typesafe-codon build -release -o main main.py
    if ldd ./main | grep -q libpython; then echo "main links libpython"; exit 1; fi
    echo "main does not link libpython"; ldd ./main | grep -E "codon|typesafe"
    ./main
  '
