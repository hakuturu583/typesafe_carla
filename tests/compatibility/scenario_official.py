"""The compatibility scenario using the official CARLA Python API.

Prints one `key=value` line per observation; scenario_typesafe.codon prints
the same keys. compare.py runs both against one server and compares them.
"""

import os
import sys
import time

import carla

from equality_cases import equality_bits

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


def v2x_dict_paths(world, lib):
    """Issue #85: sends "hello v2x" between two vehicles' custom V2X sensors
    (as upstream's smoke/test_v2x.py) and prints the first message's get()
    dict paths."""
    bp = lib.filter("vehicle.*")[0]
    base = world.get_map().get_spawn_points()[-1]
    first = world.spawn_actor(bp, base)
    fwd = base.get_forward_vector()
    second = None
    for d in (12.0, 17.0, 22.0, 27.0):
        second = world.try_spawn_actor(bp, carla.Transform(
            base.location + carla.Location(x=fwd.x * d, y=fwd.y * d, z=0.3), base.rotation))
        if second is not None:
            break
    if second is None:
        first.destroy()
        return "nospawn"
    radio = lib.find("sensor.other.v2x_custom")
    sender = world.spawn_actor(radio, carla.Transform(), attach_to=first)
    receiver = world.spawn_actor(radio, carla.Transform(), attach_to=second)
    received = []
    receiver.listen(lambda data: received.extend(list(data)))
    for _ in range(5):
        world.tick()
    for _ in range(20):
        message = carla.CustomV2XBytes()
        message.set_string("hello v2x")
        sender.send(message)
        world.tick()
    for _ in range(5):
        world.tick()
    receiver.stop()
    for a in (sender, receiver, first, second):
        a.destroy()
    if not received:
        return "none"
    d = received[0].get()
    header = d["Message"]["Header"]
    payload = d["Message"]["Message"]
    return (f"{payload['DataSize']},{payload['MaxDataSize']},{payload['Bytes'].decode()},"
            f"{header['Message ID']},{header['Protocol Version']},"
            f"{int(header['Station ID'] == first.id)},{int(d['Power'] == received[0].power)}")


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
# Some calls here and below pass the official keyword names (issue #41).
dl = n - p0.location
u = dl.make_unit_vector()
cr = dl.cross(vector=carla.Vector3D(0.0, 0.0, 1.0))
out("location_vector_types", f"{type(dl).__name__},{type(dl * 2.0).__name__},"
    f"{type(carla.Vector3D() + dl).__name__},{type(abs(dl)).__name__},{type(u).__name__}")
out("location_vector", f"{dl.length():.3f},{dl.squared_length():.3f},"
    f"{dl.dot(vector=carla.Vector3D(1.0, 1.0, 0.0)):.3f},{cr.x:.3f},{cr.y:.3f},{u.x:.3f},{u.y:.3f},"
    f"{n.distance_2d(vector=p0.location):.3f},{abs(dl).x:.3f}")
r = client.apply_batch_sync([carla.command.DestroyActor(999999)])
out("batch_error", r[0].error)

# Milestone 4: map queries, traffic lights, weather.
out("topology", len(m.get_topology()))
# Issue #39: carla.Map(name, xodr_content) from the server map's OpenDRIVE.
xm = carla.Map("copy", m.to_opendrive())
xwp = xm.get_waypoint(p0.location)
try:
    carla.Map("bad", "")
    xbad = 0
except RuntimeError:
    xbad = 1
out("map_from_xodr", f"{xm.name},{len(xm.get_spawn_points())},{len(xm.get_topology())},"
                     f"{len(xm.generate_waypoints(2.0))},{xwp.road_id},{xwp.lane_id},{xwp.s:.3f},{xbad}")
out("crosswalk_points", len(m.get_crosswalks()))
lms = m.get_all_landmarks()
out("landmarks", len(lms))
out("landmark0", f"{lms[0].id},{lms[0].name},{lms[0].type},{lms[0].road_id}")
lights = [a for a in world.get_actors() if a.type_id == "traffic.traffic_light"]
out("traffic_lights", len(lights))
# Issue #38: get_actors(actor_ids) keeps request order and leaves unknown ids out.
unknown_id = max(a.id for a in world.get_actors()) + 100000
by_id = world.get_actors([lights[1].id, unknown_id, lights[0].id])
out("actors_by_id", f"{len(by_id)},{int(by_id[0].id == lights[1].id)},"
    f"{int(by_id[1].id == lights[0].id)},{by_id[0].type_id}")
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
    f"{int(world.get_snapshot().find(999999) is None)},{int(world.get_actors().find(id=999999) is None)},"
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
    # Issue #24: more measurement types, conversion and files.
    out("frame_number", int(image.frame_number == image.frame))
    new_ids = ("sensor.other.radar", "sensor.lidar.ray_cast_semantic", "sensor.other.lane_invasion",
               "sensor.other.obstacle", "sensor.camera.dvs", "sensor.camera.optical_flow",
               "sensor.camera.depth", "sensor.camera.semantic_segmentation")
    out("sensor_types", ",".join(str(int(len(lib.filter(i)) > 0)) for i in new_ids))
    tmp = os.path.join(os.environ.get("TMPDIR", "/tmp"), "tsc_compat_official")
    bps24 = []
    for i in ("sensor.camera.depth", "sensor.camera.semantic_segmentation"):
        b = lib.find(i)
        b.set_attribute("image_size_x", "32")
        b.set_attribute("image_size_y", "24")
        bps24.append(b)
    sem_bp = lib.find("sensor.lidar.ray_cast_semantic")
    sem_bp.set_attribute("channels", "16")
    bps24 += [sem_bp, lidar_bp, lib.find("sensor.other.radar")]
    sensors = [world.spawn_actor(b, carla.Transform(carla.Location(0.0, 0.0, 2.0)), attach_to=vehicle)
               for b in bps24]
    queues = [queue.Queue() for _ in sensors]
    for s, q in zip(sensors, queues):
        s.listen(q.put)
    world.tick()
    depth, seg, sem, sweep, radar = (q.get(timeout=20.0) for q in queues)
    saved = depth.save_to_disk(os.path.join(tmp, "depth.jpg"), carla.ColorConverter.Depth)
    out("image_saved", os.path.basename(saved))
    os.remove(saved)
    depth.convert(carla.ColorConverter.LogarithmicDepth)
    reds = bytes(depth.raw_data)[2::4]
    out("depth_log_mean", f"{sum(reds) / len(reds):.3f}")
    tags = bytes(seg.raw_data)[2::4]
    seg.convert(carla.ColorConverter.CityScapesPalette)
    data = bytes(seg.raw_data)
    pairs = sorted({f"{tags[i]}:{data[4 * i + 2]},{data[4 * i + 1]},{data[4 * i]},{data[4 * i + 3]}"
                    for i in range(len(tags))})
    out("palette", ";".join(pairs))
    out("semantic_lidar", f"{sem.channels},"
                          f"{int(sum(sem.get_point_count(c) for c in range(sem.channels)) == len(sem))},"
                          f"{int(len(sem.raw_data) == 24 * len(sem))}")
    ply = sweep.save_to_disk(os.path.join(tmp, "cloud.txt"))
    with open(ply) as f:
        header = [next(f).strip() for _ in range(8)]
    os.remove(ply)
    header[2] = header[2].rsplit(" ", 1)[0] + (" N" if header[2].endswith(f" {len(sweep)}") else " ?")
    out("lidar_ply", os.path.basename(ply) + ";" + ";".join(header))
    out("radar_view", f"{int(radar.get_detection_count() == len(radar))},"
                      f"{int(len(radar.raw_data) == 16 * len(radar))}")
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
                                                    carla.VehicleAckermannControl(speed=0.0))],
        True)
    # (command.ShowDebugTelemetry is not batched here: the official 0.10.0 module
    # has no converter for it in apply_batch_sync; test_issue20 covers ours.)
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
        # The server can apply the pose a tick late (issue #73): tick up to 10
        # times until the bone has moved by over half of the +0.1 z offset.
        posed = rel
        for _ in range(10):
            world.tick()
            posed = [b for b in walker.get_bones().bone_transforms if b.name == b1.name][0].relative
            if abs(posed.location.z - rel.location.z) > 0.05:
                break
        out("walker_pose", f"{posed.location.z - rel.location.z:.3f},"
                           f"{posed.rotation.yaw - rel.rotation.yaw:.3f}")
        walker.hide_pose()
        walker.blend_pose(0.5)
        walker.get_pose_from_animation()
        world.tick()
        walker.destroy()
    # Issue #19: actor state, attributes, parent, tags, signs, skeleton, constant velocity.
    attrs = vehicle.attributes
    out("actor_identity", f"{int(vehicle.actor_state)},{int(vehicle.is_active)},"
                          f"{int(vehicle.is_dormant)},{sorted(vehicle.semantic_tags)},"
                          f"{'|'.join(sorted(attrs))},{attrs['number_of_wheels']},"
                          f"{vehicle.get_actor_class_name()}")
    cam = world.spawn_actor(cam_bp, carla.Transform(carla.Location(0.0, 0.0, 2.0)),
                            attach_to=vehicle)
    out("actor_parent", f"{int(vehicle.parent is None)},{int(cam.parent.id == vehicle.id)},"
                        f"{sorted(cam.semantic_tags)}")
    cam.destroy()
    # Issue #34: attachment_type.
    arms = [world.spawn_actor(cam_bp, carla.Transform(carla.Location(-5.0, 0.0, 3.0)),
                              attach_to=vehicle, attachment_type=kind)
            for kind in (carla.AttachmentType.SpringArm, carla.AttachmentType.SpringArmGhost)]
    out("attachment_type", ",".join(str(int(a.parent.id == vehicle.id)) for a in arms))
    for a in arms:
        a.destroy()
    signs = [a for a in world.get_actors() if isinstance(a, carla.TrafficSign)]
    plain = sorted(a.type_id for a in signs if not isinstance(a, carla.TrafficLight))
    out("traffic_signs", f"{len(signs)},{'|'.join(sorted(set(plain)))}")
    e = signs[0].trigger_volume.extent
    out("trigger_extent", f"{e.x:.3f},{e.y:.3f},{e.z:.3f}")
    try:
        skeleton = f"{len(vehicle.get_bone_names())},{len(vehicle.get_component_names())}"
    except RuntimeError:
        skeleton = "error"
    out("skeleton", skeleton)
    vehicle.set_collisions(True)
    vehicle.enable_constant_velocity(carla.Vector3D(5.0, 0.0, 0.0))
    for _ in range(10):
        world.tick()
    out("constant_velocity", f"{vehicle.get_velocity().length():.3f}")
    vehicle.disable_constant_velocity()
    # Issue #85: the custom V2X message's get() dict paths, as upstream's
    # smoke/test_v2x.py reads them. Needs a ue5-dev server and module.
    if ("i85_v2x_dict" in os.environ.get("TSC_SKIP_KEYS", "").split(",")
            or not hasattr(carla, "CustomV2XBytes") or not lib.filter("sensor.other.v2x_custom")):
        out("i85_v2x_dict", "skip")
    else:
        out("i85_v2x_dict", v2x_dict_paths(world, lib))
finally:
    world.apply_settings(original)
    out("destroyed", int(vehicle.destroy()))
# LibCarla's client cache keeps actors destroyed in this episode.
out("destroyed_lookup", int(world.get_actor(vehicle.id) is None))

# Issue #23: Client, blueprint, settings and value-type additions.
out("available_maps", ",".join(sorted(client.get_available_maps())))
out("required_files", ",".join(sorted(client.get_required_files(download=False))))
# The current map's own name always matches LibCarla's comparison, whatever
# its format ("Carla/Maps/X", "/Game/Carla/Maps/X"), so nothing reloads.
before = client.get_world().id
client.load_world_if_different(m.name)
out("load_world_same", int(client.get_world().id == before))
out("bp_filter_attr", ",".join(sorted(b.id for b in lib.filter_by_attribute("number_of_wheels", "2"))))
mkz = lib.find("vehicle.lincoln.mkz")
out("bp_tags", ",".join(sorted(mkz.tags)) + f";{int(mkz.match_tags('lincoln'))},{int(mkz.match_tags('*mkz'))},{int(mkz.match_tags('ford'))}")
s23 = world.get_settings()
out("settings_ext", f"{s23.max_culling_distance:.3f},{int(s23.deterministic_ragdolls)},{s23.tile_stream_distance:.3f},{s23.actor_active_distance:.3f},{int(s23.spectator_as_ego)}")
snap23 = world.get_snapshot()
out("frame_count", f"{int(snap23.frame_count == snap23.frame)},{int(snap23.timestamp.frame_count == snap23.frame)}")


def vec(v):
    return f"{v.x:.3f},{v.y:.3f},{v.z:.3f}"


# Yaw-only rotations: CARLA 0.10.0 and ue5-dev agree (they differ in the sign
# of the pitch and roll terms).
t23 = carla.Transform(carla.Location(1.0, 2.0, 3.0), carla.Rotation(yaw=30.0))
geometry = [",".join(f"{x:.3f}" for row in t23.get_matrix() for x in row),
            ",".join(f"{x:.3f}" for row in t23.get_inverse_matrix() for x in row),
            vec(t23.get_right_vector()), vec(t23.get_up_vector())]
p23 = carla.Location(1.0, 0.0, 0.0)
t23.transform(p23)
v23 = carla.Vector3D(1.0, 0.0, 0.0)
t23.transform_vector(in_point=v23)
geometry += [vec(p23), vec(v23)]
bb23 = carla.BoundingBox(carla.Location(0.5, 0.0, 0.0), carla.Vector3D(1.0, 2.0, 3.0))
bb23.rotation = carla.Rotation(yaw=90.0)
geometry += [vec(x) for x in bb23.get_local_vertices()] + [vec(x) for x in bb23.get_world_vertices(t23)]
geometry += [str(int(bb23.contains(carla.Location(1.0, 2.0, 3.0), point=t23))),
             str(int(bb23.contains(carla.Location(10.0, 2.0, 3.0), t23)))]
out("geometry_yaw", ",".join(geometry))
n23 = carla.Rotation(-190.0, 370.0, 540.0).get_normalized()
u23 = carla.Vector2D(3.0, 4.0).make_unit_vector()
out("geometry_misc", f"{n23.pitch:.3f},{n23.yaw:.3f},{n23.roll:.3f},{u23.x:.3f},{u23.y:.3f},"
                     f"{carla.Vector2D(3.0, 4.0).squared_length():.3f}")
# Issue #40: make_unit_vector(epsilon).
out("unit_vector", ",".join([vec(carla.Vector3D(3.0, 4.0, 0.0).make_unit_vector()),
                             vec(carla.Vector3D(0.1, 0.2, 0.0).make_unit_vector(epsilon=1.0)),
                             vec(carla.Location(3.0, 4.0, 12.0).make_unit_vector(0.5)),
                             vec(carla.Vector3D(0.1, 0.2, 0.0).make_unit_vector(epsilon=0.0)),
                             vec(carla.Vector3D(1e-7, 0.0, 0.0).make_unit_vector())]))
# carla.Quaternion exists only in a module built from ue5-dev: "skip" otherwise
# (compare.py then skips the key on both sides).
if not hasattr(carla, "Quaternion"):
    out("quaternion", "skip")
else:
    q23 = carla.Quaternion(carla.Rotation(10.0, 20.0, 30.0))
    r23 = q23.rotator()
    out("quaternion", f"{q23.x:.3f},{q23.y:.3f},{q23.z:.3f},{q23.w:.3f},{r23.pitch:.3f},{r23.yaw:.3f},"
                      f"{r23.roll:.3f}," + vec(q23.get_forward_vector()) + "," + vec(q23.get_right_vector())
        + "," + vec(q23.get_up_vector()))
# Issue #21: world queries.
out("i21_spectator", world.get_spectator().type_id)
out("i21_environment", f"{len(world.get_environment_objects())},"
                       f"{len(world.get_environment_objects(carla.CityObjectLabel.Buildings))},"
                       f"{len(world.get_level_bbs(carla.CityObjectLabel.TrafficLight))}")
# The object names include live actors, so the count can differ by a few.
out("i21_object_names", len(world.get_names_of_all_objects()))
tl_marks = m.get_all_landmarks_of_type("1000001")
tl0 = world.get_traffic_light_from_opendrive_id(tl_marks[0].id) if tl_marks else None
out("i21_traffic_lights", f"{len(tl_marks)},{int(tl0 is not None)},"
                          f"{int(world.get_traffic_light_from_opendrive_id('no-such-signal') is None)},"
                          f"{len(world.get_traffic_lights_in_junction(jn.id))},"
                          f"{len(world.get_traffic_lights_from_waypoint(junction_wp, 100.0))}")
out("i21_landmark_lookup", f"{int(world.get_traffic_light(tl_marks[0]) is not None)},"
                           f"{int(world.get_traffic_sign(tl_marks[0]) is not None)}")
out("i21_vehicle_light_states", len(world.get_vehicles_light_states()))
p0_above = carla.Location(p0.location.x, p0.location.y, p0.location.z + 5.0)
ground = world.ground_projection(p0_above)
ray = world.cast_ray(p0_above, carla.Location(p0_above.x, p0_above.y, p0_above.z - 20.0))
out("i21_ground", f"{int(ground.label)},{ground.location.z:.3f},{int(ray[0].label)},{ray[0].location.z:.3f}")
sky = world.project_point(p0_above, carla.Vector3D(0.0, 0.0, 1.0), 50.0)
out("i21_projections", f"{int(sky is None)},{len(ray) > 0:d}")
# Issue #33: Python API members found missing by the survey.
out("i33_world", f"{int(world.get_spectator().get_world().id == world.id)},"
                 f"{-1 if tl0 is None else int(tl0.state == tl0.get_state())}")
bp33 = lib.find("vehicle.lincoln.mkz")
out("i33_blueprint", f"{len(bp33)};" + ";".join(
    a.id + ":" + "|".join(a.recommended_values) for a in sorted(bp33, key=lambda a: a.id)))
PRESETS33 = ['ClearNight', 'ClearNoon', 'ClearSunset', 'CloudyNight', 'CloudyNoon', 'CloudySunset', 'Default', 'DustStorm', 'HardRainNight', 'HardRainNoon', 'HardRainSunset', 'MidRainSunset', 'MidRainyNight', 'MidRainyNoon', 'SoftRainNight', 'SoftRainNoon', 'SoftRainSunset', 'WetCloudyNight', 'WetCloudyNoon', 'WetCloudySunset', 'WetNight', 'WetNoon', 'WetSunset']
out("i33_weather_presets", ";".join(
    f"{w.cloudiness:.3f},{w.precipitation:.3f},{w.precipitation_deposits:.3f},{w.wind_intensity:.3f},"
    f"{w.sun_azimuth_angle:.3f},{w.sun_altitude_angle:.3f},{w.fog_density:.3f},{w.fog_distance:.3f},"
    f"{w.fog_falloff:.3f},{w.wetness:.3f},{w.scattering_intensity:.3f},{w.mie_scattering_scale:.3f},"
    f"{w.rayleigh_scattering_scale:.3f},{w.dust_storm:.3f}"
    for w in (getattr(carla.WeatherParameters, n) for n in PRESETS33)))
# Issue #70: == / != on the value types (offline values).
out("i70_equality", equality_bits())
# ActorAttribute == / != (Boost.Python overloads: an int or a bool reaches
# the float one). Per case: 1/0, E if it raised, M if the attribute is missing.
def attr_eq(bp, name, f):
    if not bp.has_attribute(name):
        return "M"
    try:
        return "1" if f(bp.get_attribute(name)) else "0"
    except Exception:
        return "E"


veh70 = lib.find("vehicle.lincoln.mkz")
cam70 = lib.find("sensor.camera.rgb")
out("i70_attr_equality", "".join([
    attr_eq(veh70, "number_of_wheels", lambda a: a == a.as_int()),
    attr_eq(veh70, "number_of_wheels", lambda a: a != 3),
    attr_eq(veh70, "sticky_control", lambda a: a == True),
    attr_eq(cam70, "fov", lambda a: a == 90),
    attr_eq(cam70, "fov", lambda a: a == a.as_float()),
    attr_eq(cam70, "fov", lambda a: a == 91.5),
    attr_eq(cam70, "image_size_x", lambda a: a == a.as_int()),
    attr_eq(veh70, "role_name", lambda a: a == a.as_str()),
    attr_eq(veh70, "role_name", lambda a: a != "no-such-role"),
    attr_eq(veh70, "color", lambda a: a == a.as_color()),
    attr_eq(veh70, "color", lambda a: a == "0,0,0"),
    attr_eq(veh70, "role_name", lambda a: a == veh70.get_attribute("role_name")),
    attr_eq(veh70, "role_name", lambda a: a == a.as_int()),
]))
sys.stdout.flush()
# Issue #71: str() in the Python API's format, for server objects.
m71 = world.get_map()
bp71 = lib.find("vehicle.lincoln.mkz")
attrs71 = [bp71.get_attribute(a) for a in ("number_of_wheels", "role_name", "color", "sticky_control")
           if bp71.has_attribute(a)]
attrs71.append(lib.find("sensor.camera.rgb").get_attribute("fov"))
out("i71_str", " | ".join([str(m71.get_waypoint(p0.location)), str(world.get_spectator()), str(m71)] +
                          [str(a) for a in attrs71]))
import re
# Each run of digits becomes one "#": the frame and times grow between the runs.
out("i71_timestamp", re.sub(r"\d+", "#", str(world.get_snapshot().timestamp)))
