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
         "destroyed", "map_name", "spawn_points", "waypoint", "generated", "batch_error",
         "physics_wheels", "camera", "lidar_channels", "topology", "crosswalk_points", "landmarks",
         "landmark0", "traffic_lights", "light0_times", "weather", "junction", "none_lookups",
         "lane_walk", "try_spawn_occupied")
NUMERIC = {"settled": 0.05, "driven": 0.5, "speed": 0.3, "spawn0": 0.001, "waypoint_s": 0.001,
           "next10": 0.001, "bbox": 0.001, "physics": 0.001,
           "gnss": 0.0000005, "imu_compass": 0.01}


def parse(text: str) -> dict[str, str]:
    return dict(line.split("=", 1) for line in text.splitlines() if "=" in line)


def run(cmd: list[str]) -> dict[str, str]:
    result = subprocess.run(cmd, capture_output=True, text=True, timeout=900)
    if result.returncode != 0:
        sys.exit(f"{cmd[0]} failed:\n{result.stdout}\n{result.stderr}")
    return parse(result.stdout)


def _short(v: str | None) -> str:
    if v is None:
        return "<missing>"
    return v if len(v) <= 40 else v[:37] + "..."


def main() -> int:
    python = os.environ.get("CARLA_PYTHON", sys.executable)
    official = run([python, str(HERE / "scenario_official.py")])
    typesafe = run([sys.executable, "-m", "typesafe_carla.cli", "run", "-release",
                    str(HERE / "scenario_typesafe.codon")])
    failures = 0
    print(f"{'key':20} {'official':40} {'typesafe_carla':40} result")
    for key in EXACT + tuple(NUMERIC):
        a, b = official.get(key), typesafe.get(key)
        if a is None or b is None:
            ok = False
        elif key in NUMERIC:
            xs, ys = a.split(","), b.split(",")
            try:
                ok = len(xs) == len(ys) and all(abs(float(x) - float(y)) <= NUMERIC[key]
                                                for x, y in zip(xs, ys))
            except ValueError:
                ok = False
        else:
            ok = a == b
        failures += not ok
        print(f"{key:20} {_short(a):40} {_short(b):40} {'OK' if ok else 'MISMATCH'}")
    print("compatibility:", "PASS" if failures == 0 else f"FAIL ({failures})")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
