"""The compatibility scenario using the official CARLA Python API.

Prints one `key=value` line per observation; scenario_typesafe.codon prints
the same keys. compare.py runs both against one server and compares them.
"""

import os
import sys
import time

import carla

host = os.environ.get("TSC_CARLA_HOST", "localhost")
port = int(os.environ.get("TSC_CARLA_PORT", "2000"))


def out(key, value):
    print(f"{key}={value}")


# Physics fields compared field by field (physics_all, physics_codes).
VEHICLE_FLOATS = ("max_torque", "max_rpm", "idle_rpm", "brake_effect", "rev_up_moi",
                  "rev_down_rate", "front_rear_split", "gear_change_time", "final_ratio",
                  "change_up_rpm", "change_down_rpm", "transmission_efficiency", "mass",
                  "drag_coefficient", "chassis_width", "chassis_height", "downforce_coefficient",
                  "drag_area", "sleep_threshold", "sleep_slope_limit")
WHEEL_FLOATS = ("wheel_radius", "wheel_width", "wheel_mass", "cornering_stiffness",
                "friction_force_multiplier", "side_slip_modifier", "slip_threshold",
                "skid_threshold", "max_steer_angle", "max_wheelspin_rotation",
                "suspension_max_raise", "suspension_max_drop", "suspension_damping_ratio",
                "wheel_load_ratio", "spring_rate", "spring_preload", "rollbar_scaling",
                "max_brake_torque", "max_hand_brake_torque")
WHEEL_INTS = ("axle_type", "external_torque_combine_method", "sweep_shape", "sweep_type",
              "suspension_smoothing", "wheel_index")
WHEEL_BOOLS = ("affected_by_steering", "affected_by_brake", "affected_by_handbrake",
               "affected_by_engine", "abs_enabled", "traction_control_enabled")


client = carla.Client(host, port)
client.set_timeout(20.0)
out("server_version", client.get_server_version())
world = client.get_world()
lib = world.get_blueprint_library()
out("vehicle_blueprints", ",".join(sorted(bp.id for bp in lib.filter("vehicle.*"))))
bp = lib.find("vehicle.lincoln.mkz")
out("color_type", str(bp.get_attribute("color").type))
out("wheels", bp.get_attribute("number_of_wheels").as_int())

# Milestone 1: map, waypoints, batch errors.
m = world.get_map()
out("map_name", m.name)
points = m.get_spawn_points()
out("spawn_points", len(points))
p0 = points[0]
out("spawn0", f"{p0.location.x:.3f},{p0.location.y:.3f},{p0.location.z:.3f},{p0.rotation.yaw:.3f}")
wp = m.get_waypoint(p0.location)
out("waypoint", f"{wp.road_id},{wp.section_id},{wp.lane_id},{int(wp.lane_type)}")
out("waypoint_s", f"{wp.s:.3f},{wp.lane_width:.3f}")
n = wp.next(10.0)[0].transform.location
out("next10", f"{n.x:.3f},{n.y:.3f}")
out("generated", len(m.generate_waypoints(20.0)))
# Issue #10: Location derives from Vector3D (result types and Vector3D methods).
dl = n - p0.location
u = dl.make_unit_vector()
cr = dl.cross(carla.Vector3D(0.0, 0.0, 1.0))
out("location_vector_types", f"{type(dl).__name__},{type(dl * 2.0).__name__},"
    f"{type(carla.Vector3D() + dl).__name__},{type(abs(dl)).__name__},{type(u).__name__}")
out("location_vector", f"{dl.length():.3f},{dl.squared_length():.3f},"
    f"{dl.dot(carla.Vector3D(1.0, 1.0, 0.0)):.3f},{cr.x:.3f},{cr.y:.3f},{u.x:.3f},{u.y:.3f},"
    f"{n.distance_2d(p0.location):.3f},{abs(dl).x:.3f}")
r = client.apply_batch_sync([carla.command.DestroyActor(999999)])
out("batch_error", r[0].error)

# Milestone 4: map queries, traffic lights, weather.
out("topology", len(m.get_topology()))
out("crosswalk_points", len(m.get_crosswalks()))
lms = m.get_all_landmarks()
out("landmarks", len(lms))
out("landmark0", f"{lms[0].id},{lms[0].name},{lms[0].type},{lms[0].road_id}")
lights = [a for a in world.get_actors() if a.type_id == "traffic.traffic_light"]
out("traffic_lights", len(lights))
out("light0_times", f"{lights[0].get_green_time():.3f},{lights[0].get_yellow_time():.3f},{lights[0].get_red_time():.3f}")
wthr = world.get_weather()
out("weather", f"{wthr.cloudiness:.3f},{wthr.precipitation:.3f},{wthr.sun_altitude_angle:.3f},{wthr.rayleigh_scattering_scale:.3f}")
waypoints10 = m.generate_waypoints(10.0)
junction_wp = [w for w in waypoints10 if w.is_junction][0]
jn = junction_wp.get_junction()
out("junction", f"{jn.id},{len(jn.get_waypoints(carla.LaneType.Driving))}")

# Lookups that can miss return None (issue #9).
offroad = m.get_waypoint(carla.Location(10000.0, 10000.0, 0.0), project_to_road=False)
out("none_lookups", f"{int(world.get_actor(999999) is None)},"
    f"{int(world.get_snapshot().find(999999) is None)},{int(world.get_actors().find(999999) is None)},"
    f"{int(offroad is None)},{int(wp.get_junction() is None)}")

# Issue #22: geo-reference, XODR waypoints, lane markings, landmarks, light geometry.
# (Only the origin is compared for transform_to_geolocation: CARLA 0.10.0's
# LibCarla and newer ones with geo projections differ elsewhere.)
ref = m.get_georeference()
out("georeference", f"{ref.latitude:.7f},{ref.longitude:.7f},{ref.altitude:.3f}")
g0 = m.transform_to_geolocation(carla.Location(0.0, 0.0, 0.0))
out("geo_origin", f"{g0.latitude:.7f},{g0.longitude:.7f},{g0.altitude:.3f}")
xw = m.get_waypoint_xodr(wp.road_id, wp.lane_id, wp.s)
out("waypoint_xodr", f"{xw.road_id},{xw.lane_id},{int(m.get_waypoint_xodr(999999, 1, 0.0) is None)}")
out("waypoint_xodr_loc", f"{xw.transform.location.x:.3f},{xw.transform.location.y:.3f},{xw.s:.3f}")


def marking_str(mk):
    if mk is None:
        return "None"
    return f"{int(mk.type)}:{int(mk.color)}:{int(mk.lane_change)}:{mk.width:.3f}"


out("lane_markings", f"{marking_str(wp.left_lane_marking)},{marking_str(wp.right_lane_marking)},"
    f"{int(wp.lane_change)},{int(wp.is_rht)},{int(wp.is_intersection)}")
lm0 = lms[0]
out("landmark_details", f"{int(lm0.waypoint is None)},"
    + "/".join(f"{a}:{b}" for a, b in lm0.get_lane_validities())
    + f",{lm0.h_offset:.3f},{int(lm0.is_dynamic)},{lm0.pitch:.3f},{lm0.roll:.3f}")
out("landmarks_by_id", f"{len(m.get_all_landmarks_from_id(lm0.id))},{len(m.get_landmark_group(lm0))}")
ahead = wp.get_landmarks(200.0)
out("landmarks_ahead", ";".join(sorted(f"{l.id}:{int(l.waypoint is None)}" for l in ahead)) + ","
    + str(len(wp.get_landmarks_of_type(200.0, "1000001"))) + "," + str(len(wp.get_landmarks(200.0, True))))
light_a = lights[0]
out("light_geometry", f"{light_a.get_opendrive_id()},{len(light_a.get_affected_lane_waypoints())},"
    f"{len(light_a.get_stop_waypoints())},{len(light_a.get_group_traffic_lights())},"
    f"{len(light_a.get_light_boxes())}")
tv = light_a.trigger_volume
out("light_trigger", f"{tv.location.x:.3f},{tv.location.y:.3f},{tv.extent.x:.3f},{tv.extent.y:.3f},{tv.extent.z:.3f}")


def lane_walk(w, left):
    ids = []
    for _ in range(20):
        w = w.get_left_lane() if left else w.get_right_lane()
        if w is None:
            break
        ids.append(str(w.lane_id))
    return "/".join(ids)


# Start from a waypoint that has a lane to its left, so the walk visits real
# lanes before reaching None. generate_waypoints' order depends on the LibCarla
# build (it iterates an unordered_map), so pick the smallest (road, lane, s).
multi = min((w for w in waypoints10 if w.get_left_lane() is not None),
            key=lambda w: (w.road_id, w.lane_id, w.s))
out("lane_walk", f"{multi.lane_id}:" + lane_walk(multi, True) + ";" + lane_walk(multi, False))

spawn = carla.Transform(carla.Location(-64.644844, 24.471010, 0.6), carla.Rotation(0.0, 0.159198, 0.0))
original = world.get_settings()
vehicle = world.spawn_actor(bp, spawn)
try:
    blocked = world.try_spawn_actor(bp, spawn)
    out("try_spawn_occupied", int(blocked is None))
    if blocked is not None:
        blocked.destroy()
    settings = world.get_settings()
    settings.synchronous_mode = True
    settings.fixed_delta_seconds = 0.05
    world.apply_settings(settings)
    out("type_id", vehicle.type_id)
    e = vehicle.bounding_box.extent
    out("bbox", f"{e.x:.3f},{e.y:.3f},{e.z:.3f}")
    pc = vehicle.get_physics_control()
    out("physics", f"{pc.mass:.3f},{pc.max_rpm:.3f},{pc.wheels[0].wheel_radius:.3f}")
    out("physics_wheels", len(pc.wheels))
    # Every physics field the official API can read (it cannot convert the gear
    # ratio lists in 0.10.0), except the wheels' per-frame location/velocity.
    vals = [getattr(pc, n) for n in VEHICLE_FLOATS]
    vals += [pc.center_of_mass.x, pc.center_of_mass.y, pc.center_of_mass.z]
    vals += [pc.inertia_tensor_scale.x, pc.inertia_tensor_scale.y, pc.inertia_tensor_scale.z]
    vals += [c for p in list(pc.torque_curve) + list(pc.steering_curve) for c in (p.x, p.y)]
    codes = [pc.differential_type, int(pc.use_automatic_gears),
             int(pc.use_sweep_wheel_collision), len(pc.torque_curve), len(pc.steering_curve)]
    for w in pc.wheels:
        vals += [getattr(w, n) for n in WHEEL_FLOATS]
        for v in (w.offset, w.suspension_axis, w.suspension_force_offset):
            vals += [v.x, v.y, v.z]
        vals += [c for p in w.lateral_slip_graph for c in (p.x, p.y)]
        codes += [getattr(w, n) for n in WHEEL_INTS]
        codes += [int(getattr(w, n)) for n in WHEEL_BOOLS]
        codes.append(len(w.lateral_slip_graph))
    out("physics_all", ",".join(f"{v:.3f}" for v in vals))
    out("physics_codes", ",".join(str(c) for c in codes))
    for _ in range(20):
        world.tick()
    t = vehicle.get_transform()
    out("settled", f"{t.location.x:.3f},{t.location.y:.3f},{t.rotation.yaw:.3f}")
    # Milestone 2: sensors on the settled vehicle.
    import queue
    sensor_bps = []
    cam_bp = lib.find("sensor.camera.rgb")
    cam_bp.set_attribute("image_size_x", "320")
    cam_bp.set_attribute("image_size_y", "240")
    cam_bp.set_attribute("fov", "100")
    lidar_bp = lib.find("sensor.lidar.ray_cast")
    lidar_bp.set_attribute("channels", "16")
    sensors = [world.spawn_actor(b, carla.Transform(carla.Location(0.0, 0.0, 2.0)), attach_to=vehicle)
               for b in (cam_bp, lidar_bp, lib.find("sensor.other.gnss"), lib.find("sensor.other.imu"))]
    queues = [queue.Queue() for _ in sensors]
    for s, q in zip(sensors, queues):
        s.listen(q.put)
    world.tick()
    image, sweep, fix, imu = (q.get(timeout=20.0) for q in queues)
    out("camera", f"{image.width},{image.height},{image.fov:.3f},{len(image.raw_data)}")
    out("lidar_channels", sweep.channels)
    out("gnss", f"{fix.latitude:.7f},{fix.longitude:.7f},{fix.altitude:.3f}")
    out("imu_compass", f"{imu.compass:.3f}")
    for s in sensors:
        s.stop()
        s.destroy()
    # Issue #11: Sensor.listen(callback); one image per synchronous tick.
    cb_frames = []
    cam = world.spawn_actor(cam_bp, carla.Transform(carla.Location(0.0, 0.0, 2.0)),
                            attach_to=vehicle)
    cam.listen(lambda image: cb_frames.append(image.frame))
    ticked = [world.tick() for _ in range(5)]
    deadline = time.time() + 20.0
    while len(cb_frames) < 5 and time.time() < deadline:
        time.sleep(0.01)
    cam.stop()
    cam.destroy()
    out("callback_frames", f"{len(cb_frames[:5])},{int(cb_frames[:5] == ticked)}")
    vehicle.apply_control(carla.VehicleControl(throttle=0.6, steer=0.0))
    for _ in range(40):
        world.tick()
    c = vehicle.get_control()
    out("control", f"{c.throttle:.3f},{c.steer:.3f},{c.brake:.3f},{int(c.reverse)},{c.gear}")
    loc = vehicle.get_location()
    out("driven", f"{loc.x:.3f},{loc.y:.3f}")
    out("speed", f"{vehicle.get_velocity().length():.3f}")
    # Subclass methods on what world.get_actor() returns (issue #8).
    same = world.get_actor(vehicle.id)
    same.apply_control(carla.VehicleControl(throttle=0.0, brake=1.0))
    world.tick()
    c = same.get_control()
    light0 = world.get_actor(lights[0].id)
    out("actor_compat", f"{c.throttle:.3f},{c.brake:.3f},{int(same.is_at_traffic_light())},"
                        f"{light0.get_pole_index()},{light0.get_green_time():.3f}")
    # Issue #20: Ackermann control, failure state, wheel steering, doors,
    # telemetry, vehicle bones, the batch commands, walker bones and poses.
    s0 = vehicle.get_ackermann_controller_settings()
    vehicle.apply_ackermann_controller_settings(
        carla.AckermannControllerSettings(0.5, 0.0, 0.3, 0.02, 0.0, 0.02))
    world.tick()
    s1 = vehicle.get_ackermann_controller_settings()
    out("ackermann_settings", ",".join(f"{v:.3f}" for s in (s0, s1) for v in
                                       (s.speed_kp, s.speed_ki, s.speed_kd, s.accel_kp,
                                        s.accel_ki, s.accel_kd)))
    out("failure_state", int(vehicle.get_failure_state()))
    vehicle.apply_control(carla.VehicleControl(steer=0.5, brake=1.0))
    for _ in range(10):
        world.tick()
    out("wheel_steer", f"{vehicle.get_wheel_steer_angle(carla.VehicleWheelLocation.FL_Wheel):.3f},"
                       f"{vehicle.get_wheel_steer_angle(carla.VehicleWheelLocation.BL_Wheel):.3f}")
    vehicle.set_wheel_steer_direction(carla.VehicleWheelLocation.FR_Wheel, 10.0)
    vehicle.open_door(carla.VehicleDoor.All)
    vehicle.show_debug_telemetry(True)
    world.tick()
    vehicle.close_door(carla.VehicleDoor.All)
    vehicle.show_debug_telemetry(False)
    world.tick()
    out("doors", 1)
    vehicle.apply_ackermann_control(carla.VehicleAckermannControl(speed=3.0))
    for _ in range(60):
        world.tick()
    out("ackermann_speed", f"{vehicle.get_velocity().length():.3f}")
    # get_telemetry_data and get_vehicle_bone_world_transforms need a LibCarla
    # newer than 0.10.0. When typesafe_carla's build lacks them it prints
    # "skip", and compare.py runs this script with those keys in TSC_SKIP_KEYS.
    skip = os.environ.get("TSC_SKIP_KEYS", "").split(",")
    for key, call in (("telemetry", lambda: len(vehicle.get_telemetry_data().wheels)),
                      ("vehicle_bones", lambda: len(vehicle.get_vehicle_bone_world_transforms()))):
        if key in skip:
            out(key, "skip")
            continue
        try:
            out(key, call())
        except Exception:
            out(key, "error")
    r20 = client.apply_batch_sync(
        [carla.command.ApplyVehicleAckermannControl(vehicle.id,
                                                    carla.VehicleAckermannControl(speed=0.0)),
         carla.command.ShowDebugTelemetry(vehicle.id, False)], True)
    out("ackermann_batch", ",".join(str(int(x.has_error())) for x in r20))
    nav = world.get_random_location_from_navigation()
    walker = None if nav is None else world.try_spawn_actor(
        lib.filter("walker.pedestrian.*")[0],
        carla.Transform(carla.Location(nav.x, nav.y, nav.z + 1.0)))
    if walker is None:
        out("walker_bones", "nospawn")
        out("walker_pose", "nospawn")
    else:
        world.tick()
        bones = list(walker.get_bones().bone_transforms)
        out("walker_bones", f"{len(bones)},{bones[0].name if bones else ''}")
        b1 = bones[1]
        rel = b1.relative
        walker.set_bones(carla.WalkerBoneControlIn([(b1.name, carla.Transform(
            carla.Location(rel.location.x, rel.location.y, rel.location.z + 0.1),
            carla.Rotation(rel.rotation.pitch, rel.rotation.yaw + 30.0, rel.rotation.roll)))]))
        walker.show_pose()
        world.tick()
        posed = [b for b in walker.get_bones().bone_transforms if b.name == b1.name][0].relative
        out("walker_pose", f"{posed.location.z - rel.location.z:.3f},"
                           f"{posed.rotation.yaw - rel.rotation.yaw:.3f}")
        walker.hide_pose()
        walker.blend_pose(0.5)
        walker.get_pose_from_animation()
        world.tick()
        walker.destroy()
finally:
    world.apply_settings(original)
    out("destroyed", int(vehicle.destroy()))
# LibCarla's client cache keeps actors destroyed in this episode.
out("destroyed_lookup", int(world.get_actor(vehicle.id) is None))
sys.stdout.flush()
