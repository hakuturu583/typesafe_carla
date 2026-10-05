"""typesafe_carla for CPython, with the CARLA Python API's names.

    import typesafe_carla.carla as carla

    client = carla.Client("localhost", 2000)
    world = client.get_world()

The classes are typesafe_carla's (typesafe_carla.pycarla generates the
bindings), not the `carla` package's. A released wheel carries them built;
elsewhere the first import builds them (typesafe_carla.carla_build; 15-30
min, or ahead of time with `typesafe-codon pycarla`).
"""

from typesafe_carla import carla_build as _carla_build

# The generated files live in the prebuilt or the build directory; this
# module becomes the package they make.
_package_dir = _carla_build.package_dir()
_carla_build.prepare_runtime()
__path__ = [str(_package_dir)]

from ._runtime import install as _install  # noqa: E402  (needs __path__)

_install(globals())
del _install, _carla_build, _package_dir
