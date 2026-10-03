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
         "physics_wheels", "physics_codes", "camera", "lidar_channels", "topology", "crosswalk_points", "landmarks",
         "landmark0", "traffic_lights", "light0_times", "weather", "junction", "none_lookups",
         "destroyed_lookup", "lane_walk", "try_spawn_occupied", "callback_frames", "actor_compat",
         "location_vector_types", "waypoint_xodr", "lane_markings", "landmark_details",
         "landmarks_by_id", "landmarks_ahead", "light_geometry", "ackermann_settings",
         "failure_state", "doors", "telemetry", "vehicle_bones", "ackermann_batch",
         "walker_bones", "available_maps", "required_files", "load_world_same",
         "bp_filter_attr", "bp_tags", "frame_count", "actor_identity", "actor_parent",
         "attachment_type",
         "traffic_signs", "skeleton", "i21_spectator", "i21_environment", "i21_traffic_lights",
         "i21_landmark_lookup", "i21_vehicle_light_states", "i21_projections",
         "frame_number", "sensor_types", "image_saved", "palette", "semantic_lidar", "lidar_ply",
         "radar_view", "actors_by_id", "map_from_xodr", "i33_world", "i33_blueprint",
         "i33_weather_presets", "i70_equality", "i70_attr_equality", "i71_str",
         "i71_timestamp")
NUMERIC = {"i21_object_names": 5.0, "settled": 0.05, "driven": 0.5, "speed": 0.3, "spawn0": 0.001, "waypoint_s": 0.001,
           "next10": 0.001, "bbox": 0.001, "physics": 0.001, "physics_all": 0.001,
           "gnss": 0.0000005, "imu_compass": 0.01, "location_vector": 0.002,
           "georeference": 0.0000005, "geo_origin": 0.0000005, "waypoint_xodr_loc": 0.002,
           "light_trigger": 0.002, "wheel_steer": 0.5, "ackermann_speed": 0.3,
           "walker_pose": 0.01, "settings_ext": 0.001, "geometry_yaw": 0.002,
           "geometry_misc": 0.002, "unit_vector": 0.002, "quaternion": 0.002, "trigger_extent": 0.001,
           "constant_velocity": 0.3, "i21_ground": 0.01, "depth_log_mean": 1.0}


def parse(text: str) -> dict[str, str]:
    return dict(line.split("=", 1) for line in text.splitlines() if "=" in line)


def run(cmd: list[str], env: dict[str, str] | None = None) -> dict[str, str]:
    result = subprocess.run(cmd, capture_output=True, text=True, timeout=900,
                            env={**os.environ, **(env or {})})
    if result.returncode != 0:
        sys.exit(f"{cmd[0]} failed:\n{result.stdout}\n{result.stderr}")
    return parse(result.stdout)


def _short(v: str | None) -> str:
    if v is None:
        return "<missing>"
    return v if len(v) <= 40 else v[:37] + "..."


def main() -> int:
    python = os.environ.get("CARLA_PYTHON", sys.executable)
    typesafe = run([sys.executable, "-m", "typesafe_carla.cli", "run", "-release",
                    str(HERE / "scenario_typesafe.codon")])
    # Keys typesafe_carla's LibCarla build cannot provide (e.g. telemetry with
    # LibCarla 0.10.0) are skipped on both sides.
    skipped = ",".join(k for k, v in typesafe.items() if v == "skip")
    official = run([python, str(HERE / "scenario_official.py")], {"TSC_SKIP_KEYS": skipped})
    failures = 0
    print(f"{'key':20} {'official':40} {'typesafe_carla':40} result")
    for key in EXACT + tuple(NUMERIC):
        a, b = official.get(key), typesafe.get(key)
        # "skip" from either side: typesafe_carla's (passed on through
        # TSC_SKIP_KEYS above) or the official module's (e.g. one without
        # carla.Quaternion).
        if "skip" in (a, b):
            print(f"{key:20} {_short(a):40} {_short(b):40} SKIP")
            continue
        if a is None or b is None:
            ok = False
        elif key in NUMERIC:
            xs, ys = a.split(","), b.split(",")
            try:
                ok = len(xs) == len(ys) and all(abs(float(x) - float(y)) <= NUMERIC[key]
                                                for x, y in zip(xs, ys))
            except ValueError:
                ok = False
        elif key == "palette":
            # tag:color pairs seen in one rendered frame: which tags are visible
            # differs between runs, so compare the colors of the shared tags.
            pa = dict(p.split(":", 1) for p in a.split(";") if ":" in p)
            pb = dict(p.split(":", 1) for p in b.split(";") if ":" in p)
            common = pa.keys() & pb.keys()
            ok = len(common) >= 3 and all(pa[t] == pb[t] for t in common)
        else:
            ok = a == b
        failures += not ok
        print(f"{key:20} {_short(a):40} {_short(b):40} {'OK' if ok else 'MISMATCH'}")
    print("compatibility:", "PASS" if failures == 0 else f"FAIL ({failures})")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
