"""Release tag parsing (tools/release_tag.py, used by .github/workflows/release.yml)."""

from __future__ import annotations

import datetime

import pytest

from tools import release_tag
from tools.release_tag import TagError, parse


@pytest.mark.parametrize("tag, version, ref, date", [
    ("0.1.0-ue5-dev-20261003", "0.1.0", "ue5-dev", datetime.date(2026, 10, 3)),
    ("0.1.0-0.10.0-20261003", "0.1.0", "0.10.0", datetime.date(2026, 10, 3)),
    ("1.2.3-a1b2c3d4e5f60718293a4b5c6d7e8f9012345678-20240229", "1.2.3",
     "a1b2c3d4e5f60718293a4b5c6d7e8f9012345678", datetime.date(2024, 2, 29)),
    ("0.1.0-feature/my-branch.v2-20261003", "0.1.0", "feature/my-branch.v2",
     datetime.date(2026, 10, 3)),
    ("0.1.0-release-0.10.1-hotfix-20261003", "0.1.0", "release-0.10.1-hotfix",
     datetime.date(2026, 10, 3)),
    ("0.2.0rc1-ue5-dev-20261003", "0.2.0rc1", "ue5-dev", datetime.date(2026, 10, 3)),
])
def test_valid_tags(tag, version, ref, date):
    assert parse(tag, version) == (version, ref, date)


@pytest.mark.parametrize("tag", [
    "v0.1.0",
    "0.1.0",
    "0.1.0-20261003",          # no ref
    "0.1.0--20261003",         # empty ref
    "0.1.0-ue5-dev",           # no date
    "0.1.0-ue5-dev-2026103",   # 7 digits
    "v0.1.0-ue5-dev-20261003",
    "0.1.0-ue5 dev-20261003",
    "0.1.0-ue5..dev-20261003",
    "0.1.0--x-20261003",
])
def test_malformed_tag(tag):
    with pytest.raises(TagError):
        parse(tag, "0.1.0")


@pytest.mark.parametrize("date", ["20261332", "20261301", "20250229", "20261000"])
def test_bad_date(date):
    with pytest.raises(TagError, match="calendar date"):
        parse(f"0.1.0-ue5-dev-{date}", "0.1.0")


def test_version_mismatch():
    with pytest.raises(TagError, match="0.2.0 != package version 0.1.0"):
        parse("0.2.0-ue5-dev-20261003", "0.1.0")


@pytest.mark.parametrize("ref", ["ue4-dev", "UE4-dev", "0.9.15", "0.9.15.2", "0.9"])
def test_ue4_ref(ref):
    with pytest.raises(TagError, match="UE4"):
        parse(f"0.1.0-{ref}-20261003", "0.1.0")


@pytest.mark.parametrize("ref", ["ue5-dev", "0.10.0", "0.90.0", "ue40"])
def test_not_ue4(ref):
    assert not release_tag.is_ue4_ref(ref)


SHA = "0123456789abcdef0123456789abcdef01234567"


def fake_github(commits=None, error=None):
    """A fetch() standing in for the GitHub commits API; records the URLs."""
    calls = []

    def fetch(url):
        calls.append(url)
        if error is not None:
            raise TagError(error)
        return commits

    fetch.calls = calls
    return fetch


def commit_json(sha=SHA, date="2026-09-15T18:30:00Z"):
    return {"sha": sha, "commit": {"committer": {"date": date}}}


def test_commits_url():
    url = release_tag.commits_url("feature/x-1.0", datetime.date(2026, 9, 15))
    assert url == ("https://api.github.com/repos/carla-simulator/carla/commits"
                   "?sha=feature%2Fx-1.0&per_page=1&until=2026-09-15T23%3A59%3A59Z")


def test_resolve_commit_on_the_day():
    fetch = fake_github([commit_json()])
    c = release_tag.resolve_commit("ue5-dev", datetime.date(2026, 9, 15), fetch)
    assert c.sha == SHA
    assert c.date == datetime.datetime(2026, 9, 15, 18, 30, tzinfo=datetime.timezone.utc)
    assert c.day == datetime.date(2026, 9, 15)
    assert "sha=ue5-dev" in fetch.calls[0] and "until=2026-09-15T23" in fetch.calls[0]


def test_resolve_commit_uses_utc():
    # 2026-09-16 01:00 +09:00 is 2026-09-15 16:00 UTC.
    fetch = fake_github([commit_json(date="2026-09-16T01:00:00+09:00")])
    c = release_tag.resolve_commit("ue5-dev", datetime.date(2026, 9, 15), fetch)
    assert c.day == datetime.date(2026, 9, 15)


def test_resolve_commit_unknown_ref():
    fetch = fake_github(error="GitHub API 422: No commit found for SHA: nope")
    with pytest.raises(TagError, match="not found"):
        release_tag.resolve_commit("nope", datetime.date(2026, 9, 15), fetch)


def test_resolve_commit_none_before_date():
    with pytest.raises(TagError, match="no commit on or before 2017-01-01"):
        release_tag.resolve_commit("ue5-dev", datetime.date(2017, 1, 1), fake_github([]))


def test_resolve_commit_rejects_later_commit():
    fetch = fake_github([commit_json(date="2026-09-16T00:00:01Z")])
    with pytest.raises(TagError, match="after 2026-09-15"):
        release_tag.resolve_commit("ue5-dev", datetime.date(2026, 9, 15), fetch)


def test_check_writes_github_output(tmp_path, capsys):
    out = tmp_path / "github_output"
    version = release_tag.package_version()
    tag = f"{version}-ue5-dev-20260915"
    fetch = fake_github([commit_json()])
    assert release_tag.main(["check", tag, "--github-output", str(out)], fetch) == 0
    assert out.read_text() == (f"tag={tag}\nversion={version}\ncarla_ref=ue5-dev\n"
                               f"carla_commit={SHA}\n"
                               "carla_commit_date=2026-09-15T18:30:00+00:00\n")
    assert "::warning::" not in capsys.readouterr().out


def test_check_warns_for_an_earlier_commit(tmp_path, capsys):
    out = tmp_path / "github_output"
    version = release_tag.package_version()
    fetch = fake_github([commit_json(date="2026-09-12T09:00:00Z")])
    assert release_tag.main(["check", f"{version}-ue5-dev-20260915",
                             "--github-output", str(out)], fetch) == 0
    assert f"carla_commit={SHA}" in out.read_text()
    stdout = capsys.readouterr().out
    assert "::warning::ue5-dev has no commit on 2026-09-15" in stdout
    assert "2026-09-12" in stdout


def test_check_fails_without_commit(tmp_path, capsys):
    out = tmp_path / "github_output"
    version = release_tag.package_version()
    assert release_tag.main(["check", f"{version}-ue5-dev-20170101",
                             "--github-output", str(out)], fake_github([])) == 1
    assert not out.exists()
    assert "::error::" in capsys.readouterr().err


def test_check_offline_prints_outputs(capsys):
    version = release_tag.package_version()
    fetch = fake_github(error="no network in tests")
    tag = f"{version}-0.10.0-20250320"
    assert release_tag.main(["check", tag, "--offline"], fetch) == 0
    assert capsys.readouterr().out == f"tag={tag}\nversion={version}\ncarla_ref=0.10.0\n"
    assert fetch.calls == []


def test_make_tag_uses_latest_commit_date():
    fetch = fake_github([commit_json(date="2026-09-16T01:00:00+09:00")])
    tag, commit = release_tag.make_tag("ue5-dev", "0.2.0", fetch)
    assert str(tag) == "0.2.0-ue5-dev-20260915"
    assert commit.sha == SHA
    assert "until" not in fetch.calls[0]


def test_make_tag_rejects_ue4_without_lookup():
    fetch = fake_github([commit_json()])
    with pytest.raises(TagError, match="UE4"):
        release_tag.make_tag("ue4-dev", "0.2.0", fetch)
    assert fetch.calls == []


def test_make_cli(tmp_path):
    out = tmp_path / "github_output"
    version = release_tag.package_version()
    assert release_tag.main(["make", "0.10.0", "--github-output", str(out)],
                            fake_github([commit_json(date="2025-03-20T12:00:00Z")])) == 0
    assert out.read_text().startswith(f"tag={version}-0.10.0-20250320\n")
    assert f"carla_commit={SHA}\n" in out.read_text()


def test_make_unknown_ref(capsys):
    assert release_tag.main(["make", "nope"], fake_github(error="422")) == 1
    assert "not found" in capsys.readouterr().err
