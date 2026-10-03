"""Issue #70: == / != on the value types, with the official module. Mirrors
equality_cases.codon; tests/unit/test_issue70_equality.codon embeds this
module's result. Offline: no server needed."""
import math

import carla

def bits(pairs):
    return "".join("1" if a == b else "0" for a, b in pairs) + "/" + \
           "".join("1" if a != b else "0" for a, b in pairs)

E = 1e-12  # below float32 resolution near these values
NAN = float("nan")
V, L, V2, R, Q = carla.Vector3D, carla.Location, carla.Vector2D, carla.Rotation, carla.Quaternion
T, BB = carla.Transform, carla.BoundingBox
WGS84 = carla.GeoEllipsoid(a=6378137.0, f_inv=298.257223563)

def bbox(yaw):
    b = BB(L(), V(1, 1, 1))
    b.rotation = R(0, yaw, 0)
    return b

def utm(offset=None, zone=31):
    p = carla.GeoProjectionUTM(zone, True, WGS84)
    p.offset = offset
    return p

def wheel(radius=30.0):
    return carla.WheelPhysicsControl(wheel_radius=radius)

def physics(mass=1000.0, wheels=None):
    p = carla.VehiclePhysicsControl(mass=mass)
    if wheels is not None:
        p.wheels = wheels
    return p

def telemetry(speed=1.0, slip=0.1):
    t = carla.VehicleTelemetryData()
    t.speed = speed
    t.wheels = [carla.WheelTelemetryData(lat_slip=slip)]
    return t

def bone(x=1.0, yaw=0.0, name="b"):
    b = carla.bone_transform_out()
    b.name = name
    b.world = T(L(x, 0, 0), R(0, yaw, 0))
    return b

def equality_bits():
    CASES = [
        ("Vector3D", [(V(1, 2, 3), V(1, 2, 3)), (V(0.1, 0, 0), V(0.1 + E, 0, 0)),
                      (V(0.1, 0, 0), V(0.1 + 1e-7, 0, 0)), (V(1, 2, 3), L(1, 2, 3)),
                      (V(NAN, 0, 0), V(NAN, 0, 0)), (V(0.0, 0, 0), V(-0.0, 0, 0)), (V(1, 2, 3), V(1, 2, 4))]),
        ("Location", [(L(1, 2, 3), L(1, 2, 3)), (L(0.3, 0, 0), L(0.3 + E, 0, 0)),
                      (L(1e-30, 0, 0), L(0, 0, 0)), (L(1, 2, 3), V(1, 2, 3)), (L(1, 2, 3), L(3, 2, 1))]),
        ("Vector2D", [(V2(0.1, 1), V2(0.1 + E, 1)), (V2(1, 2), V2(2, 1)), (V2(1, 2), V2(1, 2))]),
        ("Rotation", [(R(10, 20, 30), R(10, 20, 30)), (R(0.1, 0, 0), R(0.1 + E, 0, 0)),
                      (R(90, 90, 90), R(-90, -90, -90)), (R(179.99999, 0, 0), R(0.00001, 180, 180)),
                      (R(90.5, 90, 90), R(90.4, 90, 90)), (R(90.5, 0, 0), R(89.5, 180, 180)),
                      (R(100, 0, 0), R(80, 180, 180)), (R(10, 20, 30), R(10, 20, 31)),
                      (R(0, 0, 0), R(180, 180, 180)), (R(10, 0, 0), R(-10, 0, 0)),
                      (R(-170.5, -180, 180), R(9.5, 0, 0)), (R(-170.5, -180, 180), R(9.6, 0, 0))]),
        ("Quaternion", [(Q(0.1, 0, 0, 1), Q(0.1 + E, 0, 0, 1)), (Q(0, 0, 0, 1), Q(0, 0, 0, -1)),
                        (Q(), Q())]),
        ("Transform", [(T(L(1, 2, 3), R(4, 5, 6)), T(L(1, 2, 3), R(4, 5, 6))),
                       (T(L(1, 2, 3), R(90, 90, 90)), T(L(1, 2, 3), R(-90, -90, -90))),
                       (T(L(1, 2, 3)), T(L(1, 2, 3.5))), (T(L(0.1, 0, 0)), T(L(0.1 + E, 0, 0)))]),
        ("BoundingBox", [(BB(L(1, 0, 0), V(1, 2, 3)), BB(L(1, 0, 0), V(1, 2, 3))),
                         (BB(L(1, 0, 0), V(1, 2, 3)), BB(L(1, 0, 0), V(1, 2, 4))),
                         (bbox(10.0), bbox(11.0)), (bbox(10.0), bbox(10.0))]),
        ("Color", [(carla.Color(1, 2, 3, 4), carla.Color(1, 2, 3, 5)),
                   (carla.Color(1, 2, 3), carla.Color(1, 2, 4)), (carla.Color(), carla.Color())]),
        ("FloatColor", [(carla.FloatColor(0.1, 0.2, 0.3, 0.4), carla.FloatColor(0.1 + E, 0.2, 0.3, 0.4)),
                        (carla.FloatColor(0.1, 0.2, 0.3, 0.4), carla.FloatColor(0.1, 0.2, 0.3, 0.5))]),
        ("OpticalFlowPixel", [(carla.OpticalFlowPixel(0.1, 2), carla.OpticalFlowPixel(0.1 + E, 2)),
                              (carla.OpticalFlowPixel(1, 2), carla.OpticalFlowPixel(1, 3))]),
        ("GeoLocation", [(carla.GeoLocation(1, 2, 3), carla.GeoLocation(1, 2, 3)),
                         (carla.GeoLocation(0.1, 0, 0), carla.GeoLocation(0.1 + E, 0, 0))]),
        ("GeoEllipsoid", [(carla.GeoEllipsoid(), carla.GeoEllipsoid()), (WGS84, carla.GeoEllipsoid()),
                          (carla.GeoEllipsoid(6378137.0 + 1e-9), carla.GeoEllipsoid()),
                          (carla.GeoEllipsoid(1.0, 298.257223563), carla.GeoEllipsoid(1.0, 298.257223563))]),
        ("GeoOffsetTransform", [(carla.GeoOffsetTransform(1, 2, 3, math.pi / 2), carla.GeoOffsetTransform(1, 2, 3, math.pi / 2)),
                                (carla.GeoOffsetTransform(1, 2, 3, math.pi / 2), carla.GeoOffsetTransform(1, 2, 3, 0)),
                                (carla.GeoOffsetTransform(0.1, 0, 0), carla.GeoOffsetTransform(0.1 + E, 0, 0)),
                                (carla.GeoOffsetTransform(), carla.GeoOffsetTransform())]),
        ("GeoProjectionTM", [(carla.GeoProjectionTM(), carla.GeoProjectionTM()),
                             (carla.GeoProjectionTM(k=1.0), carla.GeoProjectionTM(k=0.9996)),
                             (carla.GeoProjectionTM(lat_0=1.0, ellps=WGS84), carla.GeoProjectionTM(lat_0=1.0, ellps=WGS84)),
                             (carla.GeoProjectionTM(ellps=WGS84), carla.GeoProjectionTM())]),
        ("GeoProjectionUTM", [(utm(), utm()), (utm(zone=32), utm()),
                              (utm(carla.GeoOffsetTransform(1, 2, 3, 0)), utm()),
                              (utm(carla.GeoOffsetTransform(1, 2, 3, 0)), utm(carla.GeoOffsetTransform(1, 2, 3, 0))),
                              (utm(carla.GeoOffsetTransform(1, 2, 3, 0)), utm(carla.GeoOffsetTransform(1, 2, 3, 0.1))),
                              (utm(carla.GeoOffsetTransform()), utm())]),
        ("GeoProjectionWebMerc", [(carla.GeoProjectionWebMerc(), carla.GeoProjectionWebMerc()),
                                  (carla.GeoProjectionWebMerc(WGS84), carla.GeoProjectionWebMerc())]),
        ("GeoProjectionLCC2SP", [(carla.GeoProjectionLCC2SP(1, 2, 3, 4, 5, 6), carla.GeoProjectionLCC2SP(1, 2, 3, 4, 5, 6)),
                                 (carla.GeoProjectionLCC2SP(lat_1=1.0), carla.GeoProjectionLCC2SP(lat_1=2.0))]),
        ("Timestamp", [(carla.Timestamp(1, 2.0, 3.0, 4.0), carla.Timestamp(1, 5.0, 6.0, 7.0)),
                       (carla.Timestamp(1), carla.Timestamp(2))]),
        ("VehicleControl", [(carla.VehicleControl(throttle=0.1), carla.VehicleControl(throttle=0.1 + E)),
                            (carla.VehicleControl(gear=1), carla.VehicleControl(gear=2)),
                            (carla.VehicleControl(hand_brake=True), carla.VehicleControl())]),
        ("VehicleAckermannControl", [(carla.VehicleAckermannControl(speed=0.1), carla.VehicleAckermannControl(speed=0.1 + E)),
                                     (carla.VehicleAckermannControl(jerk=1.0), carla.VehicleAckermannControl())]),
        ("AckermannControllerSettings", [(carla.AckermannControllerSettings(0.1), carla.AckermannControllerSettings(0.1 + E)),
                                         (carla.AckermannControllerSettings(accel_kd=1.0), carla.AckermannControllerSettings())]),
        ("WalkerControl", [(carla.WalkerControl(V(0.1, 0, 0), 1.0), carla.WalkerControl(V(0.1 + E, 0, 0), 1.0)),
                           (carla.WalkerControl(V(1, 0, 0), 1.0, True), carla.WalkerControl(V(1, 0, 0), 1.0, False)),
                           (carla.WalkerControl(speed=1.0), carla.WalkerControl(speed=2.0))]),
        ("WheelTelemetryData", [(carla.WheelTelemetryData(0.1, 0.2, 0.3), carla.WheelTelemetryData(0.1 + E, 0.2, 0.3)),
                                (carla.WheelTelemetryData(omega=1.0), carla.WheelTelemetryData())]),
        ("VehicleTelemetryData", [(telemetry(), telemetry()), (telemetry(0.1), telemetry(0.1 + E)),
                                  (telemetry(slip=0.1), telemetry(slip=0.2)), (telemetry(), carla.VehicleTelemetryData())]),
        ("WheelPhysicsControl", [(wheel(), wheel()), (wheel(0.1), wheel(0.1 + E)), (wheel(30.0), wheel(31.0))]),
        ("VehiclePhysicsControl", [(physics(), physics()), (physics(1000.1), physics(1000.1 + 1e-9)),
                                   (physics(wheels=[wheel()]), physics(wheels=[wheel(31.0)])),
                                   (physics(mass=1.0), physics(mass=2.0))]),
        ("WeatherParameters", [(carla.WeatherParameters(cloudiness=0.1), carla.WeatherParameters(cloudiness=0.1 + E)),
                               (carla.WeatherParameters(dust_storm=1.0), carla.WeatherParameters()),
                               (carla.WeatherParameters.ClearNoon, carla.WeatherParameters.ClearNoon),
                               (carla.WeatherParameters.ClearNoon, carla.WeatherParameters.WetNoon)]),
        ("WorldSettings", [(carla.WorldSettings(), carla.WorldSettings()),
                           (carla.WorldSettings(fixed_delta_seconds=0.05), carla.WorldSettings(fixed_delta_seconds=0.05 + E)),
                           (carla.WorldSettings(fixed_delta_seconds=-1.0), carla.WorldSettings()),
                           (carla.WorldSettings(fixed_delta_seconds=0.0), carla.WorldSettings()),
                           (carla.WorldSettings(max_culling_distance=0.1), carla.WorldSettings(max_culling_distance=0.1 + E)),
                           (carla.WorldSettings(max_substep_delta_time=0.01 + E), carla.WorldSettings()),
                           (carla.WorldSettings(synchronous_mode=True), carla.WorldSettings())]),
        ("bone_transform_out", [(bone(), bone()), (bone(0.1), bone(0.1 + E)), (bone(name="c"), bone()),
                                (bone(yaw=90.0), bone(yaw=-90.0)), (bone(yaw=10.0), bone(yaw=11.0))]),
    ]

    return ";".join(f"{name}:{bits(pairs)}" for name, pairs in CASES)


if __name__ == "__main__":
    print(equality_bits())
