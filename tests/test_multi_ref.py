"""One wheel, a native library per CARLA ref (tools/release_carla_refs.txt):
merging the wheels (tools/merge_native_wheels.py) and selecting a ref at run
time (typesafe_carla.paths, TYPESAFE_CARLA_CARLA_REF)."""

from __future__ import annotations

import base64
import hashlib
import json
import zipfile

import pytest

from tools import merge_native_wheels
from typesafe_carla import paths

WHEEL = "typesafe_carla-0.5.0-py3-none-manylinux_2_28_x86_64.whl"
NATIVE = "typesafe_carla/_native"


def _native_files(ref):
    return {
        f"{NATIVE}/libtypesafe_carla_ffi.so": f"lib for {ref}".encode(),
        f"{NATIVE}/BUILD_INFO.json": json.dumps({"carla_git_ref": ref}).encode(),
        f"{NATIVE}/THIRD_PARTY_NOTICES": f"notices for {ref}".encode(),
    }


def make_wheel(directory, ref, extra=None):
    files = {"typesafe_carla/__init__.py": b"__version__ = '0.5.0'\n",
             "typesafe_carla/_codon/typesafe_carla/__init__.codon": b"# codon\n",
             **_native_files(ref), **(extra or {})}
    directory.mkdir(parents=True, exist_ok=True)
    wheel = directory / WHEEL
    record = []
    with zipfile.ZipFile(wheel, "w") as z:
        for name, data in files.items():
            z.writestr(name, data)
            digest = base64.urlsafe_b64encode(hashlib.sha256(data).digest()).rstrip(b"=").decode()
            record.append(f"{name},sha256={digest},{len(data)}")
        record.append("typesafe_carla-0.5.0.dist-info/RECORD,,")
        z.writestr("typesafe_carla-0.5.0.dist-info/RECORD", "\n".join(record) + "\n")
    return wheel


def test_merge_adds_the_other_refs_under_refs(tmp_path):
    base = make_wheel(tmp_path / "a", "ue5-dev")
    other = make_wheel(tmp_path / "b", "ue58-dev")
    assert merge_native_wheels.merge(base, [other]) == ["ue5-dev", "ue58-dev"]
    with zipfile.ZipFile(base) as z:
        names = set(z.namelist())
        assert z.read(f"{NATIVE}/libtypesafe_carla_ffi.so") == b"lib for ue5-dev"
        assert z.read(f"{NATIVE}/refs/ue58-dev/libtypesafe_carla_ffi.so") == b"lib for ue58-dev"
        assert f"{NATIVE}/refs/ue58-dev/THIRD_PARTY_NOTICES" in names
        record = z.read("typesafe_carla-0.5.0.dist-info/RECORD").decode().splitlines()
    recorded = {line.split(",")[0] for line in record}
    assert recorded == names


def test_merge_rejects_a_ref_twice(tmp_path):
    base = make_wheel(tmp_path / "a", "ue5-dev")
    other = make_wheel(tmp_path / "b", "ue5-dev")
    with pytest.raises(merge_native_wheels.MergeError, match="already"):
        merge_native_wheels.merge(base, [other])


def test_merge_rejects_wheels_that_differ_outside_native(tmp_path):
    base = make_wheel(tmp_path / "a", "ue5-dev")
    other = make_wheel(tmp_path / "b", "ue58-dev",
                       {"typesafe_carla/_codon/typesafe_carla/__init__.codon": b"# other\n"})
    with pytest.raises(merge_native_wheels.MergeError, match="__init__.codon"):
        merge_native_wheels.merge(base, [other])


def test_merge_rejects_another_version(tmp_path):
    base = make_wheel(tmp_path / "a", "ue5-dev")
    other = make_wheel(tmp_path / "b", "ue58-dev")
    other = other.rename(other.with_name(WHEEL.replace("0.5.0", "0.5.1")))
    with pytest.raises(merge_native_wheels.MergeError, match="same version"):
        merge_native_wheels.merge(base, [other])


@pytest.fixture
def installed(tmp_path, monkeypatch):
    """An installed `_native` directory with ue5-dev (default) and ue58-dev."""
    native = tmp_path / "_native"
    for rel, ref in (("", "ue5-dev"), ("refs/ue58-dev", "ue58-dev")):
        d = native / rel
        d.mkdir(parents=True, exist_ok=True)
        (d / paths.LIBRARY_NAME).write_bytes(b"")
        (d / "BUILD_INFO.json").write_text(json.dumps({"carla_git_ref": ref}))
    monkeypatch.setattr(paths, "_source_root", lambda: None)
    monkeypatch.setattr(paths, "_installed_dirs", lambda name: [tmp_path / name])
    monkeypatch.delenv(paths.ENV_LIB, raising=False)
    monkeypatch.delenv(paths.ENV_CARLA_REF, raising=False)
    return native


def test_the_default_ref_is_the_one_in_native(installed):
    assert paths.native_library() == (installed / paths.LIBRARY_NAME).resolve()
    assert paths.build_info()["carla_git_ref"] == "ue5-dev"
    assert list(paths.carla_refs()) == ["ue5-dev", "ue58-dev"]


@pytest.mark.parametrize("ref, rel", [("ue58-dev", "refs/ue58-dev"), ("ue5-dev", "")])
def test_the_environment_selects_a_ref(installed, monkeypatch, ref, rel):
    monkeypatch.setenv(paths.ENV_CARLA_REF, ref)
    assert paths.native_library() == (installed / rel / paths.LIBRARY_NAME).resolve()
    assert paths.build_info()["carla_git_ref"] == ref


def test_an_unknown_ref_names_the_ones_there_are(installed, monkeypatch):
    monkeypatch.setenv(paths.ENV_CARLA_REF, "0.10.0")
    with pytest.raises(paths.PathError, match="ue5-dev, ue58-dev only"):
        paths.native_library()


def test_the_library_override_wins_over_the_ref(installed, monkeypatch, tmp_path):
    lib = tmp_path / "other.so"
    lib.write_bytes(b"")
    monkeypatch.setenv(paths.ENV_LIB, str(lib))
    monkeypatch.setenv(paths.ENV_CARLA_REF, "ue58-dev")
    assert paths.native_library() == lib.resolve()


def test_a_checkout_build_of_another_ref_is_an_error(tmp_path, monkeypatch):
    build = tmp_path / "build"
    build.mkdir()
    (build / paths.LIBRARY_NAME).write_bytes(b"")
    (build / "BUILD_INFO.json").write_text(json.dumps({"carla_git_ref": "mock"}))
    monkeypatch.setattr(paths, "_source_root", lambda: tmp_path)
    monkeypatch.delenv(paths.ENV_LIB, raising=False)
    monkeypatch.delenv(paths.ENV_BUILD_DIR, raising=False)
    monkeypatch.delenv(paths.ENV_CARLA_REF, raising=False)
    assert paths.native_library() == (build / paths.LIBRARY_NAME).resolve()
    monkeypatch.setenv(paths.ENV_CARLA_REF, "ue58-dev")
    with pytest.raises(paths.PathError, match="built from CARLA mock"):
        paths.native_library()
