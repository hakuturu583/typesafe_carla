"""`python -m tools.pycarla`: the checkout's build of the `carla` package
(typesafe_carla as a CPython package under the CARLA Python API's import name,
for running CARLA's own tests unmodified; tools/upstream_tests.py).

The generator is typesafe_carla.pycarla, shipped in the package: CPython
programs use `import typesafe_carla.carla as carla` (typesafe_carla.carla_build).
This module is that generator, with `--package carla` as the default.

    python -m tools.pycarla [-o DIR]          # default DIR: <build dir>/pycarla
    PYTHONPATH=DIR python -c 'import carla'
"""

from __future__ import annotations

import sys

from typesafe_carla import pycarla as _pycarla

if __name__ == "__main__":
    sys.exit(_pycarla.main(prog="python -m tools.pycarla", default_package=_pycarla.PACKAGE_NAME))
else:
    sys.modules[__name__] = _pycarla
