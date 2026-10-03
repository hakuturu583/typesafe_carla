"""Issue #77: arithmetic between the Vector3D family and Vector2D, with the
official module. Mirrors arithmetic_cases.codon; tests/unit/test_issue77_arithmetic.codon
embeds this module's result. Offline: no server needed.

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


if __name__ == "__main__":
    print(arithmetic_cases())
