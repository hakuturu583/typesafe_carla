#!/usr/bin/env bash
# Design section 44 success criteria, checked in a clean container:
#
#   uv sync && uv run typesafe-codon build -release main.py && ./main
#
# with no system CARLA Python package, no manually installed Codon, no
# CODON_PATH and no LibCarla installation. The container gets only uv, a C
# compiler (Codon links executables with `cc`) and a directory of wheels
# (typesafe-carla + typesafe-carla-toolchain) standing in for PyPI.
#
# usage: tests/distribution/clean_env.sh WHEEL_DIR [CARLA_PORT] [IMAGE]
# The CARLA server must listen on localhost:CARLA_PORT (host network).
set -euo pipefail
wheels=$(realpath "${1:?usage: clean_env.sh WHEEL_DIR [CARLA_PORT] [IMAGE]}")
port=${2:-2000}
image=${3:-ubuntu:24.04}
here=$(cd "$(dirname "$0")" && pwd)
uv_bin=$(command -v uv)

docker run --rm --net=host \
  -v "$wheels":/wheels:ro -v "$here":/src:ro -v "$uv_bin":/usr/local/bin/uv:ro \
  -e CARLA_PORT="$port" "$image" bash -euo pipefail -c '
    export DEBIAN_FRONTEND=noninteractive
    apt-get update -qq >/dev/null && apt-get install -y -qq gcc ca-certificates >/dev/null
    echo "== clean environment"
    ! command -v codon && echo "no codon on PATH"
    ! python3 -c "import carla" 2>/dev/null && echo "no CARLA Python package"
    echo "CODON_PATH=${CODON_PATH:-<unset>}"

    mkdir /app && cd /app
    cp /src/main.py /src/bad.py .
    cat > pyproject.toml <<TOML
[project]
name = "my-carla-project"
version = "0.1.0"
requires-python = ">=3.11"
dependencies = ["typesafe-carla==0.1.*"]

[tool.uv]
find-links = ["/wheels"]
TOML
    echo "== uv sync"
    uv sync 2>&1 | tail -3
    uv run typesafe-codon info

    echo "== compile-time rejection"
    if uv run typesafe-codon build -release -o bad bad.py 2>err.txt; then
      echo "bad.py compiled but must not"; exit 1
    fi
    sed "s/\x1b\[[0-9;]*m//g" err.txt | head -1
    test ! -e bad && echo "no executable produced"

    echo "== typesafe-codon build -release main.py && ./main"
    uv run typesafe-codon build -release -o main main.py
    ldd ./main | grep -c python && { echo "main links libpython"; exit 1; } || echo "main does not link libpython"
    ./main
  '
