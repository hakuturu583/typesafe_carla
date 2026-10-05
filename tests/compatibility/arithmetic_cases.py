"""Issue #77: arithmetic between the Vector3D family and Vector2D, with the
official module. Mirrors arithmetic_cases.codon; tests/unit/test_issue77_arithmetic.codon
embeds this module's result. Offline: no server needed.

Issue #81: float32_cases(), quaternion_cases() and rounding_cases() (embedded by
tests/unit/test_issue81_float32.codon) check float32 storage and arithmetic
on non-dyadic values, printing every float as "%.17g" (exact for a double).

Per case: the result's type and components times 16 (all are dyadic, so
float32, the official module's precision, and double, typesafe_carla's,
agree exactly), "&" appended when an in-place operator updated the left
operand itself (an alias sees it), or "E" when the module raises.
"""
import operator

import carla

def types():
    # A function: CARLA 0.10.0's module has no Velocity, AngularVelocity or
    # Acceleration, and runs only the issue #90 tables (fields, derived).
    return (carla.Vector3D, carla.Location, carla.Velocity, carla.AngularVelocity,
            carla.Acceleration, carla.Vector2D)

def lhs(t):
    return t(1.5, -2.25) if t is carla.Vector2D else t(1.5, -2.25, 4.0)

def rhs(t):
    return t(0.5, 1.0) if t is carla.Vector2D else t(0.5, 1.0, -3.0)

def fmt(v):
    cs = (v.x, v.y) if isinstance(v, carla.Vector2D) else (v.x, v.y, v.z)
    return type(v).__name__ + "(" + ",".join(str(int(c * 16)) for c in cs) + ")"

def show(f, a, b):
    alias = a
    try:
        r = f(a, b)
    except Exception:  # TypeError, Boost.Python.ArgumentError
        return "E"
    return fmt(r) + ("&" if r is alias else "")

def arithmetic_cases():
    TYPES = types()
    out = []
    for name, f in (("+", operator.add), ("-", operator.sub),
                    ("+=", operator.iadd), ("-=", operator.isub)):
        out.append(name + ":" + ",".join(show(f, lhs(a), rhs(b)) for a in TYPES for b in TYPES))
    for name, f in (("*", operator.mul), ("/", operator.truediv),
                    ("*=", operator.imul), ("/=", operator.itruediv)):
        out.append(name + ":" + ",".join(show(f, lhs(a), k) for a in TYPES for k in (4, 0.5)))
    for name, f in (("r*", operator.mul), ("r/", operator.truediv)):
        out.append(name + ":" + ",".join(show(f, k, lhs(a)) for a in TYPES for k in (4, 0.5)))
    return ";".join(out)


# Issue #81. Non-dyadic operands (float32 results differ from double ones),
# large and tiny magnitudes (16777217 is not a float32; 1e30 * 1e10 overflows).
F_LHS = ((-81.2, 0.1, 3.3), (16777217.0, 1e-3, -0.7), (1e30, 2.0 / 3.0, 1e-30),
         (1e-22, 0.0, 0.0), (0.3, -0.4, 1.2e19))
F_RHS = ((78.6634, 0.2, 7.1), (1.0, 0.3, 0.7), (1e10, 1e-8, 5.0))
F_K = (0.1, 3.3, 1e10, 7.0)

def g(x):
    return "%.17g" % x

def gv(v):
    cs = (v.x, v.y) if isinstance(v, carla.Vector2D) else (v.x, v.y, v.z)
    return "(" + ",".join(g(c) for c in cs) + ")"

def float32_cases():
    V, V2 = carla.Vector3D, carla.Vector2D
    out = []
    for a in F_LHS:
        for b in F_RHS:
            va, vb = V(*a), V(*b)
            out.append("|".join((
                gv(va + vb), gv(va - vb), gv(va.cross(vb)), g(va.dot(vb)), g(va.dot_2d(vb)),
                g(va.distance(vb)), g(va.distance_2d(vb)), g(va.distance_squared(vb)),
                g(va.distance_squared_2d(vb)), g(va.get_vector_angle(vb)))))
            c = V(*a)
            c += vb
            d = carla.Location(*a)
            d -= vb
            out.append(gv(c) + "|" + gv(d))
            wa, wb = V2(a[0], a[1]), V2(b[0], b[1])
            out.append(gv(wa + wb) + "|" + gv(wa - wb))
        va = V(*a)
        wa = V2(a[0], a[1])
        out.append("|".join((g(va.length()), g(va.squared_length()), gv(va.make_unit_vector()),
                             gv(abs(V(-a[0], a[1], -a[2]))), g(wa.length()), g(wa.squared_length()),
                             gv(wa.make_unit_vector()))))
        for k in F_K:
            c = carla.Velocity(*a)
            c *= k
            w = V2(a[0], a[1])
            w *= k
            out.append("|".join((gv(va * k), gv(k * va), gv(va / k), gv(k / va), gv(c),
                                 gv(wa * k), gv(wa / k), gv(w))))
    return ";".join(out)

# Quaternion's methods, float32 step by step as LibCarla ue5-dev's Quaternion.h.
Q_VALUES = ((0.1, 0.2, 0.3, 0.9), (1.0, 2.0, 3.0, 4.0), (-0.7, 0.01, 0.33, 0.62),
            (1e-3, -2.5, 7.1, 1e-2), (0.0, 0.0, 0.0, 0.0), (1e20, 3.3, -1e-20, 0.5))

def gq(q):
    return "(" + ",".join(g(c) for c in (q.x, q.y, q.z, q.w)) + ")"

def quaternion_cases():
    out = []
    for a in Q_VALUES:
        qa = carla.Quaternion(*a)
        out.append("|".join((g(qa.length()), gq(qa.inverse()), gq(qa.unit_quaternion()),
                             gq(qa.conjugate()), gv(qa.get_forward_vector()),
                             gv(qa.get_right_vector()), gv(qa.get_up_vector()))))
        out.append("|".join(gq(qa * carla.Quaternion(*b)) for b in Q_VALUES))
    return ";".join(out)

# Values stored by a constructor or a field assignment: rounded to float32
# (out of range gives inf, 1e-46 underflows to 0), except GeoLocation's doubles.
R_VALUES = (0.1, 16777217.0, 1e40, -1e40, float("nan"), 1e-46, -0.0, 3.4028235e38, 2.0 / 3.0)

def rounding_cases():
    out = []
    for x in R_VALUES:
        set3 = carla.Location()
        set3.y = x
        set2 = carla.Vector2D()
        set2.y = x
        rot = carla.Rotation()
        rot.roll = x
        q = carla.Quaternion()
        q.w = x
        t = carla.Transform()
        t.location.z = x
        geo = carla.GeoLocation()
        geo.altitude = x
        out.append("|".join(g(v) for v in (
            carla.Vector3D(x).x, carla.Location(x).x, carla.Velocity(x).x,
            carla.AngularVelocity(x).x, carla.Acceleration(x).x, carla.Vector2D(x).x,
            set3.y, set2.y, carla.Rotation(x).pitch, rot.roll, carla.Quaternion(x).x, q.w,
            carla.Transform(carla.Location(0.0, 0.0, x)).location.z, t.location.z,
            carla.GeoLocation(x).latitude, geo.altitude)))
    return ";".join(out)


# Issue #90: the scalar float fields of the control, physics, weather, settings
# and measurement value types are LibCarla `float`s: rounded to float32 on
# construction and on assignment. WorldSettings.max_substep_delta_time is a
# double and stays unrounded. Only classes and fields the official CARLA 0.10.0
# module has (LidarDetection and its siblings cannot be constructed with values
# there; VehiclePhysicsControl's gear ratio lists cannot be read).
FIELD_VALUES = (0.1, 16777217.0, 1e40, float("nan"), 1e-46, 2.0 / 3.0)

def field_rounding_cases():
    out = []
    for x in FIELD_VALUES:
        row = []
        c0 = carla.VehicleControl(throttle=x, steer=x, brake=x)
        s0 = carla.VehicleControl()
        s0.throttle = x
        s0.steer = x
        s0.brake = x
        row += [c0.throttle, c0.steer, c0.brake, s0.throttle, s0.steer, s0.brake]
        c1 = carla.WalkerControl(speed=x)
        s1 = carla.WalkerControl()
        s1.speed = x
        row += [c1.speed, s1.speed]
        c2 = carla.VehicleAckermannControl(steer=x, steer_speed=x, speed=x, acceleration=x, jerk=x)
        s2 = carla.VehicleAckermannControl()
        s2.steer = x
        s2.steer_speed = x
        s2.speed = x
        s2.acceleration = x
        s2.jerk = x
        row += [c2.steer, c2.steer_speed, c2.speed, c2.acceleration, c2.jerk, s2.steer,
                s2.steer_speed, s2.speed, s2.acceleration, s2.jerk]
        c3 = carla.AckermannControllerSettings(speed_kp=x, speed_ki=x, speed_kd=x, accel_kp=x,
                                               accel_ki=x, accel_kd=x)
        s3 = carla.AckermannControllerSettings()
        s3.speed_kp = x
        s3.speed_ki = x
        s3.speed_kd = x
        s3.accel_kp = x
        s3.accel_ki = x
        s3.accel_kd = x
        row += [c3.speed_kp, c3.speed_ki, c3.speed_kd, c3.accel_kp, c3.accel_ki, c3.accel_kd,
                s3.speed_kp, s3.speed_ki, s3.speed_kd, s3.accel_kp, s3.accel_ki, s3.accel_kd]
        c4 = carla.VehiclePhysicsControl(max_torque=x, max_rpm=x, idle_rpm=x, brake_effect=x,
                                         rev_up_moi=x, rev_down_rate=x, front_rear_split=x,
                                         gear_change_time=x, final_ratio=x, change_up_rpm=x,
                                         change_down_rpm=x, transmission_efficiency=x, mass=x,
                                         drag_coefficient=x, chassis_width=x, chassis_height=x,
                                         downforce_coefficient=x, drag_area=x, sleep_threshold=x,
                                         sleep_slope_limit=x)
        s4 = carla.VehiclePhysicsControl()
        s4.max_torque = x
        s4.max_rpm = x
        s4.idle_rpm = x
        s4.brake_effect = x
        s4.rev_up_moi = x
        s4.rev_down_rate = x
        s4.front_rear_split = x
        s4.gear_change_time = x
        s4.final_ratio = x
        s4.change_up_rpm = x
        s4.change_down_rpm = x
        s4.transmission_efficiency = x
        s4.mass = x
        s4.drag_coefficient = x
        s4.chassis_width = x
        s4.chassis_height = x
        s4.downforce_coefficient = x
        s4.drag_area = x
        s4.sleep_threshold = x
        s4.sleep_slope_limit = x
        row += [c4.max_torque, c4.max_rpm, c4.idle_rpm, c4.brake_effect, c4.rev_up_moi,
                c4.rev_down_rate, c4.front_rear_split, c4.gear_change_time, c4.final_ratio,
                c4.change_up_rpm, c4.change_down_rpm, c4.transmission_efficiency, c4.mass,
                c4.drag_coefficient, c4.chassis_width, c4.chassis_height, c4.downforce_coefficient,
                c4.drag_area, c4.sleep_threshold, c4.sleep_slope_limit, s4.max_torque, s4.max_rpm,
                s4.idle_rpm, s4.brake_effect, s4.rev_up_moi, s4.rev_down_rate, s4.front_rear_split,
                s4.gear_change_time, s4.final_ratio, s4.change_up_rpm, s4.change_down_rpm,
                s4.transmission_efficiency, s4.mass, s4.drag_coefficient, s4.chassis_width,
                s4.chassis_height, s4.downforce_coefficient, s4.drag_area, s4.sleep_threshold,
                s4.sleep_slope_limit]
        c5 = carla.WheelPhysicsControl(wheel_radius=x, wheel_width=x, wheel_mass=x,
                                       cornering_stiffness=x, friction_force_multiplier=x,
                                       side_slip_modifier=x, slip_threshold=x, skid_threshold=x,
                                       max_steer_angle=x, max_wheelspin_rotation=x,
                                       suspension_max_raise=x, suspension_max_drop=x,
                                       suspension_damping_ratio=x, wheel_load_ratio=x,
                                       spring_rate=x, spring_preload=x, rollbar_scaling=x,
                                       max_brake_torque=x, max_hand_brake_torque=x)
        s5 = carla.WheelPhysicsControl()
        s5.wheel_radius = x
        s5.wheel_width = x
        s5.wheel_mass = x
        s5.cornering_stiffness = x
        s5.friction_force_multiplier = x
        s5.side_slip_modifier = x
        s5.slip_threshold = x
        s5.skid_threshold = x
        s5.max_steer_angle = x
        s5.max_wheelspin_rotation = x
        s5.suspension_max_raise = x
        s5.suspension_max_drop = x
        s5.suspension_damping_ratio = x
        s5.wheel_load_ratio = x
        s5.spring_rate = x
        s5.spring_preload = x
        s5.rollbar_scaling = x
        s5.max_brake_torque = x
        s5.max_hand_brake_torque = x
        row += [c5.wheel_radius, c5.wheel_width, c5.wheel_mass, c5.cornering_stiffness,
                c5.friction_force_multiplier, c5.side_slip_modifier, c5.slip_threshold,
                c5.skid_threshold, c5.max_steer_angle, c5.max_wheelspin_rotation,
                c5.suspension_max_raise, c5.suspension_max_drop, c5.suspension_damping_ratio,
                c5.wheel_load_ratio, c5.spring_rate, c5.spring_preload, c5.rollbar_scaling,
                c5.max_brake_torque, c5.max_hand_brake_torque, s5.wheel_radius, s5.wheel_width,
                s5.wheel_mass, s5.cornering_stiffness, s5.friction_force_multiplier,
                s5.side_slip_modifier, s5.slip_threshold, s5.skid_threshold, s5.max_steer_angle,
                s5.max_wheelspin_rotation, s5.suspension_max_raise, s5.suspension_max_drop,
                s5.suspension_damping_ratio, s5.wheel_load_ratio, s5.spring_rate,
                s5.spring_preload, s5.rollbar_scaling, s5.max_brake_torque,
                s5.max_hand_brake_torque]
        c6 = carla.WeatherParameters(cloudiness=x, precipitation=x, precipitation_deposits=x,
                                     wind_intensity=x, sun_azimuth_angle=x, sun_altitude_angle=x,
                                     fog_density=x, fog_distance=x, fog_falloff=x, wetness=x,
                                     scattering_intensity=x, mie_scattering_scale=x,
                                     rayleigh_scattering_scale=x, dust_storm=x)
        s6 = carla.WeatherParameters()
        s6.cloudiness = x
        s6.precipitation = x
        s6.precipitation_deposits = x
        s6.wind_intensity = x
        s6.sun_azimuth_angle = x
        s6.sun_altitude_angle = x
        s6.fog_density = x
        s6.fog_distance = x
        s6.fog_falloff = x
        s6.wetness = x
        s6.scattering_intensity = x
        s6.mie_scattering_scale = x
        s6.rayleigh_scattering_scale = x
        s6.dust_storm = x
        row += [c6.cloudiness, c6.precipitation, c6.precipitation_deposits, c6.wind_intensity,
                c6.sun_azimuth_angle, c6.sun_altitude_angle, c6.fog_density, c6.fog_distance,
                c6.fog_falloff, c6.wetness, c6.scattering_intensity, c6.mie_scattering_scale,
                c6.rayleigh_scattering_scale, c6.dust_storm, s6.cloudiness, s6.precipitation,
                s6.precipitation_deposits, s6.wind_intensity, s6.sun_azimuth_angle,
                s6.sun_altitude_angle, s6.fog_density, s6.fog_distance, s6.fog_falloff, s6.wetness,
                s6.scattering_intensity, s6.mie_scattering_scale, s6.rayleigh_scattering_scale,
                s6.dust_storm]
        c7 = carla.WorldSettings(max_culling_distance=x, tile_stream_distance=x,
                                 actor_active_distance=x, max_substep_delta_time=x)
        s7 = carla.WorldSettings()
        s7.max_culling_distance = x
        s7.tile_stream_distance = x
        s7.actor_active_distance = x
        s7.max_substep_delta_time = x
        row += [c7.max_culling_distance, c7.tile_stream_distance, c7.actor_active_distance,
                c7.max_substep_delta_time, s7.max_culling_distance, s7.tile_stream_distance,
                s7.actor_active_distance, s7.max_substep_delta_time]
        c8 = carla.FloatColor(r=x, g=x, b=x, a=x)
        s8 = carla.FloatColor()
        s8.r = x
        s8.g = x
        s8.b = x
        s8.a = x
        row += [c8.r, c8.g, c8.b, c8.a, s8.r, s8.g, s8.b, s8.a]
        c9 = carla.OpticalFlowPixel(x=x, y=x)
        s9 = carla.OpticalFlowPixel()
        s9.x = x
        s9.y = x
        row += [c9.x, c9.y, s9.x, s9.y]
        c10 = carla.LightState(intensity=x)
        s10 = carla.LightState()
        s10.intensity = x
        row += [c10.intensity, s10.intensity]
        s11 = carla.LidarDetection()
        s11.intensity = x
        row += [s11.intensity]
        s12 = carla.SemanticLidarDetection()
        s12.cos_inc_angle = x
        row += [s12.cos_inc_angle]
        s13 = carla.RadarDetection()
        s13.velocity = x
        s13.azimuth = x
        s13.altitude = x
        s13.depth = x
        row += [s13.velocity, s13.azimuth, s13.altitude, s13.depth]

        out.append("|".join(g(v) for v in row))
    return ";".join(out)

# Issue #90: derived geometry, float32 step by step as LibCarla's
# Rotation::RotateVector, Transform::TransformPoint, BoundingBox's vertices and
# Contains, and Rotation::Normalize.
D_ROTATIONS = ((0.0, 30.0, 0.0), (0.0, -123.4, 0.0), (0.0, 0.1, 0.0), (10.0, 45.0, -20.0),
               (-33.3, 271.7, 98.6), (89.9, 0.3, 179.9))
D_LOCATIONS = ((10.5, -3.3, 0.7), (1e5, 2.5e5, -12.1))
D_POINTS = ((1.0, 2.0, 3.0), (-81.2, 0.1, 3.3), (1e5, -2e4, 7.7), (0.3, -0.4, 1.2e10))
D_ANGLES = (0.0, -0.0, 0.1, 180.0, -180.0, 179.99998, 359.99997, 360.0, 540.0, -540.5, 720.1,
            -1e-30, 1e7 + 0.5, 123456789.0, 1e10, -1e10, 3.4e38, float("inf"), float("nan"))

def rot(p, y, r):
    """Rotation(p, y, r) in the sign convention of LibCarla ue5-dev, which
    typesafe_carla's tests use (the mock's). CARLA 0.10.0 has the opposite sign
    on the pitch and roll terms of its rotation matrix, and its float formulas
    with (-p, y, -r) are ue5-dev's with (p, y, r), bit for bit: sinf is odd and
    cosf even, so every product and sum is exactly negated or unchanged."""
    if hasattr(carla, "Quaternion"):  # ue5-dev
        return carla.Rotation(p, y, r)
    return carla.Rotation(-p, y, -r)

def derived_cases():
    L, V = carla.Location, carla.Vector3D
    out = []
    for r in D_ROTATIONS:
        ro = rot(*r)
        out.append("|".join((gv(ro.get_forward_vector()), gv(ro.get_right_vector()),
                             gv(ro.get_up_vector()))))
        for loc in D_LOCATIONS:
            t = carla.Transform(L(*loc), rot(*r))
            row = []
            for p in D_POINTS:
                a = L(*p)
                t.transform(a)
                b = V(*p)
                t.transform_vector(b)
                row += [gv(a), gv(b)]
            pts = [L(*p) for p in D_POINTS]
            t.transform(pts)
            row += [gv(q) for q in pts]
            out.append("|".join(row))
            bb = carla.BoundingBox()  # 0.10.0's BoundingBox(l, e, r) is unusable
            bb.location = L(*loc)
            bb.extent = V(2.4, 0.9, 1.7)
            bb.rotation = rot(*r)
            row = [gv(q) for q in bb.get_local_vertices()]
            row += [gv(q) for q in bb.get_world_vertices(t)]
            row += ["1" if bb.contains(L(*p), t) else "0" for p in D_POINTS]
            row += ["1" if bb.contains(q, t) else "0" for q in bb.get_world_vertices(t)]
            out.append("|".join(row))
    for a in D_ANGLES:
        n = carla.Rotation(a, -a, 0.5 * a).get_normalized()
        out.append(g(n.pitch) + "," + g(n.yaw) + "," + g(n.roll))
    return ";".join(out)

# Issue #90: Quaternion(rotation) and rotator() as LibCarla ue5-dev's
# Quaternion.h (float32, cosf / sinf / asinf / atan2f). CARLA 0.10.0 has no
# Quaternion: tests/unit/test_issue90_float32.codon embeds the output of
# ue5-dev's own Quaternion.h compiled into a small C++ program, which prints the
# same table.
Q_ROTATIONS = ((0.0, 0.0, 0.0), (10.0, 45.0, -20.0), (-33.3, 271.7, 98.6), (89.9, 0.3, 179.9),
               (0.1, -0.1, 1e-3), (90.0, 0.0, 0.0), (-90.0, 180.0, -180.0), (1e7, 3.3, -721.5))

def gr(r):
    return "(" + g(r.pitch) + "," + g(r.yaw) + "," + g(r.roll) + ")"

def quaternion_rotation_cases():
    out = []
    for r in Q_ROTATIONS:
        q = carla.Quaternion(carla.Rotation(*r))
        out.append(gq(q) + "|" + gr(q.rotator()))
    for a in Q_VALUES:
        out.append(gr(carla.Quaternion(*a).rotator()))
    return ";".join(out)


if __name__ == "__main__":
    import sys
    if sys.argv[1:] == ["float32"]:
        print(float32_cases())
    elif sys.argv[1:] == ["quaternion"]:
        print(quaternion_cases())
    elif sys.argv[1:] == ["rounding"]:
        print(rounding_cases())
    elif sys.argv[1:] == ["fields"]:
        print(field_rounding_cases())
    elif sys.argv[1:] == ["derived"]:
        print(derived_cases())
    elif sys.argv[1:] == ["quaternion_rotation"]:
        print(quaternion_rotation_cases())
    else:
        print(arithmetic_cases())
