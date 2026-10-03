"""LibCarla prebuilt download (tools/fetch_libcarla_prebuilt.py): naming,
artifact selection and verification, with `gh` replaced by fakes."""

from __future__ import annotations

import hashlib
import io
import json
import zipfile

import pytest
from pathlib import Path

from tools import fetch_libcarla_prebuilt as fp

SHA = "ada75f920642e18cace0a9f85ecf9d4077ddb531"
ABI = ("target=x86_64-linux-gnu\ncc=gcc 11.4.0, glibc 2.35\n"
       "cxx=gcc 11.4.0, libstdc++ 11 (20230528) cxx11-abi=1, glibc 2.35\n")
REPO = "https://github.com/carla-simulator/carla.git"


def make_zip(contents: dict[str, bytes], **manifest_overrides) -> bytes:
    manifest = {
        "format": 1, "carla_repository": REPO, "carla_ref": "0.10.0", "carla_commit": SHA,
        "carla_version": "0.10.0", "abi": ABI.rstrip("\n"),
        "files": {k: hashlib.sha256(v).hexdigest() for k, v in contents.items()},
    }
    manifest.update(manifest_overrides)
    buf = io.BytesIO()
    with zipfile.ZipFile(buf, "w") as z:
        z.writestr("prebuilt.json", json.dumps(manifest))
        for name, data in contents.items():
            z.writestr(name, data)
    return buf.getvalue()


FILES = {"lib/libcarla-client.a": b"!<arch>\n", "include/carla/Version.h": b"#pragma once\n"}


def fake_gh(archive: bytes):
    def gh(*args, out=None):
        assert args[0] == "api" and args[1].endswith("/zip")
        out.write(archive)
    return gh


def artifact(archive: bytes) -> dict:
    return {"id": 1, "name": "x", "size_in_bytes": len(archive),
            "digest": "sha256:" + hashlib.sha256(archive).hexdigest()}


def test_artifact_name(monkeypatch):
    monkeypatch.setattr(fp, "os_label", lambda: "ubuntu-22.04")
    digest = hashlib.sha256(ABI.encode()).hexdigest()[:12]
    assert fp.artifact_name(SHA, ABI) == f"libcarla-prebuilt-ubuntu-22.04-gcc11-{digest}-{SHA}"


def test_download_verifies_and_unpacks(tmp_path, monkeypatch):
    archive = make_zip(FILES)
    monkeypatch.setattr(fp, "gh", fake_gh(archive))
    dest = tmp_path / "cache" / "name"
    fp.download("o/r", artifact(archive), dest, SHA, ABI, REPO)
    assert (dest / "prebuilt.json").is_file()
    assert (dest / "lib" / "libcarla-client.a").read_bytes() == b"!<arch>\n"
    assert [p.name for p in dest.parent.iterdir()] == ["name"]  # no leftovers


@pytest.mark.parametrize("archive, art_digest, message", [
    (make_zip(FILES, carla_commit="0" * 40), None, "expected " + SHA),
    (make_zip(FILES, abi="target=x86_64-linux-gnu\ncc=gcc 13.3.0"), None, "differs"),
    (make_zip(FILES, files={"lib/libcarla-client.a": "0" * 64,
                            "include/carla/Version.h": "0" * 64}), None, "sha256 mismatch"),
    (make_zip(FILES, files={"lib/libcarla-client.a": "0" * 64}), None, "extra"),
    (make_zip(FILES), "sha256:" + "0" * 64, "digest"),
    (make_zip({**FILES, "../evil": b""}), None, "outside the prefix"),
])
def test_download_rejects(tmp_path, monkeypatch, archive, art_digest, message):
    monkeypatch.setattr(fp, "gh", fake_gh(archive))
    art = artifact(archive)
    if art_digest:
        art["digest"] = art_digest
    dest = tmp_path / "name"
    with pytest.raises(ValueError, match=message):
        fp.download("o/r", art, dest, SHA, ABI, REPO)
    assert not dest.exists()
    assert list(tmp_path.iterdir()) == []


def test_find_artifact_takes_successful_main_runs_only(monkeypatch):
    arts = [
        # newest first: a pull request from a fork whose branch is called main
        {"id": 4, "name": "n", "expired": False, "created_at": "2026-10-04",
         "workflow_run": {"id": 40, "head_branch": "main", "repository_id": 1,
                          "head_repository_id": 2}},
        # a failed run
        {"id": 3, "name": "n", "expired": False, "created_at": "2026-10-03",
         "workflow_run": {"id": 30, "head_branch": "main", "repository_id": 1,
                          "head_repository_id": 1}},
        {"id": 2, "name": "n", "expired": False, "created_at": "2026-10-02",
         "workflow_run": {"id": 20, "head_branch": "main", "repository_id": 1,
                          "head_repository_id": 1}},
        {"id": 1, "name": "n", "expired": True, "created_at": "2026-10-05",
         "workflow_run": {"id": 10, "head_branch": "main", "repository_id": 1,
                          "head_repository_id": 1}},
    ]
    runs = {
        30: {"path": fp.WORKFLOW_PATH, "event": "push", "head_branch": "main",
             "conclusion": "failure"},
        20: {"path": fp.WORKFLOW_PATH, "event": "schedule", "head_branch": "main",
             "conclusion": "success"},
    }

    def gh(*args, out=None):
        if "repos/o/r/actions/artifacts" in args:
            return {"artifacts": arts}
        return runs[int(args[-1].rsplit("/", 1)[1])]

    monkeypatch.setattr(fp, "gh", gh)
    monkeypatch.setattr(fp.shutil, "which", lambda _: "/usr/bin/gh")
    monkeypatch.setattr(fp.subprocess, "run", lambda *a, **k: type("R", (), {"returncode": 0})())
    assert fp.find_artifact("o/r", "n")["id"] == 2
    runs[20]["conclusion"] = "failure"
    with pytest.raises(fp.Unavailable):
        fp.find_artifact("o/r", "n")


def test_download_requires_a_digest(tmp_path, monkeypatch):
    archive = make_zip(FILES)
    monkeypatch.setattr(fp, "gh", fake_gh(archive))
    art = artifact(archive)
    del art["digest"]
    with pytest.raises(ValueError, match="no sha256 digest"):
        fp.download("o/r", art, tmp_path / "name", SHA, ABI, REPO)


def test_cache_root_matches_typesafe_carla(monkeypatch, tmp_path):
    from typesafe_carla import paths
    monkeypatch.delenv("TYPESAFE_CARLA_CACHE_DIR", raising=False)
    monkeypatch.setenv("XDG_CACHE_HOME", str(tmp_path))
    assert fp.cache_root() == paths._cache_root() / "libcarla-prebuilt"
    assert fp.cache_root() == tmp_path / "typesafe-carla" / "libcarla-prebuilt"
    monkeypatch.setenv("TYPESAFE_CARLA_CACHE_DIR", str(tmp_path / "c"))
    assert fp.cache_root() == paths._cache_root() / "libcarla-prebuilt"


def test_prune(tmp_path):
    import os
    import time
    old, new, partial = tmp_path / "old", tmp_path / "new", tmp_path / ".download-x"
    for d in (old, new, partial):
        d.mkdir()
    day = 86400
    os.utime(old, (time.time() - 40 * day,) * 2)
    os.utime(partial, (time.time() - 2 * day,) * 2)
    assert sorted(fp.prune(tmp_path, 30)) == sorted([old, partial])
    assert new.exists()
    assert fp.prune(tmp_path, 0, keep=new) == []
    assert fp.prune(tmp_path, 0) == [new]


def test_cached_prefix_is_reverified(tmp_path, monkeypatch, capsys):
    """A cached prefix whose files changed is discarded and downloaded again."""
    archive = make_zip(FILES)
    monkeypatch.setattr(fp, "gh", fake_gh(archive))
    monkeypatch.setenv("TYPESAFE_CARLA_CACHE_DIR", str(tmp_path))
    monkeypatch.setattr(fp, "resolve", lambda ref, repo: SHA)
    monkeypatch.setattr(fp, "abi_fingerprint", lambda: ABI)
    monkeypatch.setattr(fp, "find_artifact", lambda repo, name: artifact(archive))
    assert fp.main(["0.10.0"]) == 0
    dest = Path(capsys.readouterr().out.strip())
    (dest / "include" / "carla" / "Version.h").write_text("tampered\n")
    downloads = []
    monkeypatch.setattr(fp, "find_artifact",
                        lambda repo, name: downloads.append(name) or artifact(archive))
    assert fp.main(["0.10.0"]) == 0
    assert len(downloads) == 1
    assert (dest / "include" / "carla" / "Version.h").read_bytes() == b"#pragma once\n"
    assert fp.main(["0.10.0"]) == 0 and len(downloads) == 1  # intact: no download
