"""tools/bump_version.py, run against a copy of the version files."""

from __future__ import annotations

import shutil
from pathlib import Path

import pytest

from tools import bump_version

ROOT = Path(__file__).resolve().parent.parent


@pytest.fixture
def root(tmp_path):
    for rel in bump_version.VERSION_FILES:
        (tmp_path / rel).parent.mkdir(parents=True, exist_ok=True)
        shutil.copy(ROOT / rel, tmp_path / rel)
    return tmp_path


@pytest.mark.parametrize("current, spec, expected", [
    ("0.1.0", "patch", "0.1.1"),
    ("0.1.9", "patch", "0.1.10"),
    ("0.1.3", "minor", "0.2.0"),
    ("0.9.3", "major", "1.0.0"),
    ("0.1.0", "1.4.2", "1.4.2"),
])
def test_next_version(current, spec, expected):
    assert bump_version.next_version(current, spec) == expected


@pytest.mark.parametrize("current, spec", [
    ("0.1.0", "v1.0.0"), ("0.1.0", "1.0"), ("0.1.0", "1.0.0rc1"), ("0.1.0", "huge"),
    ("0.1.0", ""),
    ("0.2.0rc1", "patch"),  # a prerelease cannot be bumped, only set
])
def test_next_version_rejects(current, spec):
    with pytest.raises(SystemExit, match="X.Y.Z"):
        bump_version.next_version(current, spec)


def test_current_version_is_the_package_version():
    import typesafe_carla
    assert bump_version.current_version() == typesafe_carla.__version__


def test_main_bumps_every_file_and_prints_only_the_version(root, capsys):
    before = bump_version.current_version(root)
    major, minor, _ = map(int, before.split("."))
    bump_version.main(["minor"], root)
    new = f"{major}.{minor + 1}.0"
    assert capsys.readouterr().out == f"{new}\n"
    # Only the version line changes.
    for rel in bump_version.VERSION_FILES:
        assert (root / rel).read_text() == (ROOT / rel).read_text().replace(
            f'__version__ = "{before}"', f'__version__ = "{new}"')


def test_main_refuses_disagreeing_files(root):
    rel = bump_version.VERSION_FILES[1]
    path = root / rel
    path.write_text(path.read_text().replace("__version__ = ", '__version__ = "9.9.9"\n#', 1))
    with pytest.raises(SystemExit, match="disagree"):
        bump_version.main(["patch"], root)


def test_main_usage(root):
    with pytest.raises(SystemExit):
        bump_version.main([], root)
