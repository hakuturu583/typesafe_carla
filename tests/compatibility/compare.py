"""Runs the compatibility scenario with the official CARLA Python API and with
typesafe_carla against the same server, and compares the observations.

    TSC_CARLA_PORT=2000 CARLA_PYTHON=/path/to/venv/bin/python \\
        uv run python tests/compatibility/compare.py

Exact keys must match exactly; physics-derived keys within a tolerance (two
separate runs of the same deterministic-step scenario).
"""

from __future__ import annotations

import os
import subprocess
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
EXACT = ("server_version", "vehicle_blueprints", "color_type", "wheels", "type_id", "control",
         "destroyed")
NUMERIC = {"settled": 0.05, "driven": 0.5, "speed": 0.3}


def parse(text: str) -> dict[str, str]:
    return dict(line.split("=", 1) for line in text.splitlines() if "=" in line)


def run(cmd: list[str]) -> dict[str, str]:
    result = subprocess.run(cmd, capture_output=True, text=True, timeout=900)
    if result.returncode != 0:
        sys.exit(f"{cmd[0]} failed:\n{result.stdout}\n{result.stderr}")
    return parse(result.stdout)


def main() -> int:
    python = os.environ.get("CARLA_PYTHON", sys.executable)
    official = run([python, str(HERE / "scenario_official.py")])
    typesafe = run([sys.executable, "-m", "typesafe_carla.cli", "run", "-release",
                    str(HERE / "scenario_typesafe.codon")])
    failures = 0
    print(f"{'key':20} {'official':40} {'typesafe_carla':40} result")
    for key in EXACT + tuple(NUMERIC):
        a, b = official.get(key, "<missing>"), typesafe.get(key, "<missing>")
        if key in NUMERIC:
            try:
                ok = all(abs(float(x) - float(y)) <= NUMERIC[key]
                         for x, y in zip(a.split(","), b.split(",")))
            except ValueError:
                ok = False
        else:
            ok = a == b
        failures += not ok
        show = lambda v: v if len(v) <= 40 else v[:37] + "..."
        print(f"{key:20} {show(a):40} {show(b):40} {'OK' if ok else 'MISMATCH'}")
    print("compatibility:", "PASS" if failures == 0 else f"FAIL ({failures})")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
