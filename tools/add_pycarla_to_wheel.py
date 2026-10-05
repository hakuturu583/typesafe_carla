"""Adds the prebuilt `typesafe_carla.carla` package to a built wheel.

    python tools/add_pycarla_to_wheel.py dist/typesafe_carla-<v>-py3-none-<plat>.whl

Run with an interpreter that has that wheel (and its typesafe-carla-toolchain)
installed: the package is compiled from the installed Codon sources with the
installed Codon (carla_build.make_prebuilt, 15-50 min, ~14 GB of memory, needs
`cc`), and must match them (carla_build.prebuilt_stamp()). Its files go into
the wheel as typesafe_carla/carla/_prebuilt/, RECORD updated, in place. The
wheel stays py3-none: Codon's output does not depend on the Python version.
Used by the release workflow, inside the manylinux image the wheel is built
in, so `_carla.so` needs no newer glibc than the native library does.
"""

from __future__ import annotations

import argparse
import base64
import hashlib
import shutil
import sys
import tempfile
import zipfile
from pathlib import Path

PREBUILT = "typesafe_carla/carla/_prebuilt"


def _record_line(name: str, data: bytes) -> str:
    digest = base64.urlsafe_b64encode(hashlib.sha256(data).digest()).rstrip(b"=").decode()
    return f"{name},sha256={digest},{len(data)}"


def add(wheel: Path, prebuilt: Path) -> None:
    """Rewrites `wheel` with the files of `prebuilt` under PREBUILT."""
    added = {f"{PREBUILT}/{p.name}": p.read_bytes() for p in sorted(prebuilt.iterdir()) if p.is_file()}
    tmp = wheel.with_suffix(".tmp")
    with zipfile.ZipFile(wheel) as src, zipfile.ZipFile(tmp, "w", zipfile.ZIP_DEFLATED) as dst:
        record_name = next(n for n in src.namelist() if n.endswith(".dist-info/RECORD"))
        for info in src.infolist():
            if info.filename == record_name or info.filename.startswith(PREBUILT + "/"):
                continue
            dst.writestr(info, src.read(info.filename))
        record = [ln for ln in src.read(record_name).decode().splitlines()
                  if ln and not ln.startswith(PREBUILT + "/") and not ln.startswith(record_name + ",")]
        for name, data in added.items():
            info = zipfile.ZipInfo(name, date_time=(2020, 1, 1, 0, 0, 0))
            info.external_attr = (0o755 if name.endswith(".so") else 0o644) << 16
            info.compress_type = zipfile.ZIP_DEFLATED
            dst.writestr(info, data)
            record.append(_record_line(name, data))
        record.append(f"{record_name},,")
        dst.writestr(record_name, "\n".join(record) + "\n")
    tmp.replace(wheel)


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("wheel", type=Path)
    args = parser.parse_args(argv)

    import typesafe_carla
    from typesafe_carla import carla_build

    version = args.wheel.name.split("-")[1]
    if typesafe_carla.__version__ != version:
        print(f"the installed typesafe_carla is {typesafe_carla.__version__}, the wheel {version}: "
              f"install the wheel first", file=sys.stderr)
        return 1
    with tempfile.TemporaryDirectory() as tmp:
        prebuilt = carla_build.make_prebuilt(Path(tmp) / "_prebuilt")
        add(args.wheel, prebuilt)
        print(f"{args.wheel}: added {', '.join(sorted(p.name for p in prebuilt.iterdir()))}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
