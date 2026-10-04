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

TYPES = (carla.Vector3D, carla.Location, carla.Velocity, carla.AngularVelocity,
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


if __name__ == "__main__":
    import sys
    if sys.argv[1:] == ["float32"]:
        print(float32_cases())
    elif sys.argv[1:] == ["quaternion"]:
        print(quaternion_cases())
    elif sys.argv[1:] == ["rounding"]:
        print(rounding_cases())
    else:
        print(arithmetic_cases())
