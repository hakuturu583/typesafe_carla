"""Release tag parsing (tools/release_tag.py, used by .github/workflows/release.yml)."""

from __future__ import annotations

import datetime
import json

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
    assert str(parse(tag, version)) == tag


@pytest.mark.parametrize("tag, version, date", [
    ("0.1.0-20261003", "0.1.0", datetime.date(2026, 10, 3)),
    ("0.2.0rc1-20240229", "0.2.0rc1", datetime.date(2024, 2, 29)),
])
def test_valid_tags_of_every_listed_ref(tag, version, date):
    assert parse(tag, version) == (version, None, date)
    assert str(parse(tag, version)) == tag


@pytest.mark.parametrize("tag", [
    "v0.1.0",
    "0.1.0",
    "0.1.0-2026103",           # no ref, 7 digits
    "0.1.0-20261003-",         # empty date
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


def fake_github(commits=None, error=None, error_cls=release_tag.RefNotFound):
    """A fetch() standing in for the GitHub commits API; records the URLs."""
    calls = []

    def fetch(url):
        calls.append(url)
        if error is not None:
            raise error_cls(error)
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


def test_resolve_commit_other_api_error_is_not_not_found():
    fetch = fake_github(error="GitHub API 500: boom", error_cls=TagError)
    with pytest.raises(TagError, match="500: boom") as e:
        release_tag.resolve_commit("ue5-dev", datetime.date(2026, 9, 15), fetch)
    assert "not found" not in str(e.value)


@pytest.mark.parametrize("sha", ["abc", "A" * 40, None, SHA + "0"])
def test_resolve_commit_rejects_invalid_api_sha(sha):
    with pytest.raises(TagError, match="invalid commit SHA"):
        release_tag.resolve_commit("ue5-dev", None, fake_github([commit_json(sha=sha)]))


def test_sha_ref_must_resolve_to_itself():
    # The reviewer's repro: GitHub answers a SHA with its newest ancestor by
    # the day, which must not be built in its place.
    want, older = "a" * 40, "b" * 40
    fetch = fake_github([commit_json(sha=older, date="2025-01-01T00:00:00Z")])
    with pytest.raises(TagError, match="resolves to " + older):
        release_tag.resolve_commit(want, datetime.date(2025, 1, 1), fetch)


@pytest.mark.parametrize("ref", [SHA, SHA[:7], SHA[:12]])
def test_sha_ref_on_its_day(ref):
    c = release_tag.resolve_commit(ref, datetime.date(2026, 9, 15), fake_github([commit_json()]))
    assert c.sha == SHA


def test_sha_ref_on_another_day():
    with pytest.raises(TagError, match="from 2026-09-15, not 2026-09-16"):
        release_tag.resolve_commit(SHA, datetime.date(2026, 9, 16), fake_github([commit_json()]))


@pytest.mark.parametrize("today, ok", [
    (datetime.date(2026, 9, 16), True),
    (datetime.date(2026, 9, 15), False),
    (datetime.date(2026, 9, 14), False),
])
def test_check_rejects_today_and_later(today, ok, capsys):
    version = release_tag.package_version()
    rc = release_tag.main(["check", f"{version}-ue5-dev-20260915", "--offline"],
                          fake_github(error="offline"), today=today)
    assert rc == (0 if ok else 1)
    if not ok:
        assert "not before today" in capsys.readouterr().err


def fake_status(code):
    calls = []

    def status(url):
        calls.append(url)
        return code

    status.calls = calls
    return status


def test_unpublished_ok(capsys):
    status = fake_status(404)
    assert release_tag.main(["unpublished", "--version", "9.9.9"], status=status) == 0
    assert status.calls == ["https://pypi.org/pypi/typesafe-carla/9.9.9/json"]


def test_unpublished_default_version_and_testpypi():
    status = fake_status(404)
    assert release_tag.main(["unpublished", "--index", "testpypi"], status=status) == 0
    version = release_tag.package_version()
    assert status.calls == [f"https://test.pypi.org/pypi/typesafe-carla/{version}/json"]


@pytest.mark.parametrize("code, message", [(200, "already on pypi"), (503, "HTTP 503")])
def test_unpublished_fails(code, message, capsys):
    assert release_tag.main(["unpublished", "--version", "0.1.0"],
                            status=fake_status(code)) == 1
    assert message in capsys.readouterr().err


def test_check_writes_github_output(tmp_path, capsys):
    out = tmp_path / "github_output"
    version = release_tag.package_version()
    tag = f"{version}-ue5-dev-20260915"
    fetch = fake_github([commit_json()])
    assert release_tag.main(["check", tag, "--github-output", str(out)], fetch) == 0
    assert out.read_text() == (
        f"tag={tag}\nversion={version}\ncarla_ref=ue5-dev\n"
        f"carla_commit={SHA}\n"
        "carla_commit_date=2026-09-15T18:30:00+00:00\n"
        "carla_refs=ue5-dev\n"
        f'carla_matrix=[{{"index":0,"ref":"ue5-dev","commit":"{SHA}",'
        '"date":"2026-09-15T18:30:00+00:00"}]\n')
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
    assert capsys.readouterr().out == (f"tag={tag}\nversion={version}\ncarla_ref=0.10.0\n"
                                       "carla_refs=0.10.0\n")
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


SHA2 = "89abcdef0123456789abcdef0123456789abcdef"


def fake_github_per_ref(commits):
    """A fetch() answering each ref (``sha=<ref>``) with its own commit list."""
    calls = []

    def fetch(url):
        calls.append(url)
        for ref, answer in commits.items():
            if f"sha={ref}&" in url:
                return answer
        raise release_tag.RefNotFound(url)

    fetch.calls = calls
    return fetch


@pytest.fixture
def refs_file(tmp_path):
    path = tmp_path / "refs.txt"
    path.write_text("# comment\nue5-dev\n\nue58-dev  # the newer one\n")
    return path


def test_release_refs(refs_file):
    assert release_tag.release_refs(refs_file) == ["ue5-dev", "ue58-dev"]


def test_the_repository_lists_release_refs():
    assert release_tag.release_refs()[0] == "ue5-dev"


@pytest.mark.parametrize("text, message", [
    ("# nothing\n", "no CARLA ref"),
    ("ue5-dev\nue5-dev\n", "twice"),
    ("feature/x\n", "directory"),
    ("ue4-dev\n", "UE4"),
])
def test_bad_release_refs(tmp_path, text, message):
    path = tmp_path / "refs.txt"
    path.write_text(text)
    with pytest.raises(TagError, match=message):
        release_tag.release_refs(path)


def test_make_multi_tag_is_dated_with_the_newest_commit():
    fetch = fake_github_per_ref({"ue5-dev": [commit_json(date="2026-09-02T17:42:40Z")],
                                 "ue58-dev": [commit_json(SHA2, "2026-10-09T09:40:31Z")]})
    tag, resolved = release_tag.make_multi_tag("0.5.0", ["ue5-dev", "ue58-dev"], fetch)
    assert str(tag) == "0.5.0-20261009"
    assert [(ref, c.sha) for ref, c in resolved] == [("ue5-dev", SHA), ("ue58-dev", SHA2)]


def test_make_cli_without_ref_builds_every_listed_ref(tmp_path, refs_file):
    out = tmp_path / "github_output"
    version = release_tag.package_version()
    fetch = fake_github_per_ref({"ue5-dev": [commit_json(date="2026-09-02T17:42:40Z")],
                                 "ue58-dev": [commit_json(SHA2, "2026-10-09T09:40:31Z")]})
    assert release_tag.main(["make", "--github-output", str(out)], fetch,
                            refs_file=refs_file) == 0
    lines = dict(ln.split("=", 1) for ln in out.read_text().splitlines())
    assert lines["tag"] == f"{version}-20261009"
    assert "carla_ref" not in lines and "carla_commit" not in lines
    assert lines["carla_refs"] == "ue5-dev,ue58-dev"
    assert json.loads(lines["carla_matrix"]) == [
        {"index": 0, "ref": "ue5-dev", "commit": SHA, "date": "2026-09-02T17:42:40+00:00"},
        {"index": 1, "ref": "ue58-dev", "commit": SHA2, "date": "2026-10-09T09:40:31+00:00"}]


def test_check_multi_tag_resolves_every_ref_by_the_date(tmp_path, refs_file, capsys):
    out = tmp_path / "github_output"
    version = release_tag.package_version()
    fetch = fake_github_per_ref({"ue5-dev": [commit_json(date="2026-09-02T17:42:40Z")],
                                 "ue58-dev": [commit_json(SHA2, "2026-10-09T09:40:31Z")]})
    assert release_tag.main(["check", f"{version}-20261009", "--github-output", str(out)],
                            fetch, today=datetime.date(2026, 10, 12), refs_file=refs_file) == 0
    assert all("until=2026-10-09T23" in url for url in fetch.calls)
    matrix = json.loads(dict(ln.split("=", 1) for ln in out.read_text().splitlines())
                        ["carla_matrix"])
    assert [(m["ref"], m["commit"]) for m in matrix] == [("ue5-dev", SHA), ("ue58-dev", SHA2)]
    # ue5-dev has no commit that day, but ue58-dev does: no warning.
    assert "::warning::" not in capsys.readouterr().out


def test_check_multi_tag_warns_when_no_ref_has_a_commit_that_day(tmp_path, refs_file, capsys):
    out = tmp_path / "github_output"
    version = release_tag.package_version()
    fetch = fake_github_per_ref({"ue5-dev": [commit_json(date="2026-09-02T17:42:40Z")],
                                 "ue58-dev": [commit_json(SHA2, "2026-10-09T09:40:31Z")]})
    assert release_tag.main(["check", f"{version}-20261010", "--github-output", str(out)],
                            fetch, today=datetime.date(2026, 10, 12), refs_file=refs_file) == 0
    assert "::warning::none of ue5-dev, ue58-dev has a commit on 2026-10-10" in \
        capsys.readouterr().out


def test_check_multi_tag_fails_if_a_ref_is_missing(refs_file, capsys):
    version = release_tag.package_version()
    fetch = fake_github_per_ref({"ue5-dev": [commit_json(date="2026-09-02T17:42:40Z")]})
    assert release_tag.main(["check", f"{version}-20261009"], fetch,
                            today=datetime.date(2026, 10, 12), refs_file=refs_file) == 1
    assert "ue58-dev" in capsys.readouterr().err


def test_refs_cli(refs_file, capsys):
    fetch = fake_github_per_ref({"ue5-dev": [commit_json(date="2026-09-02T17:42:40Z")],
                                 "ue58-dev": [commit_json(SHA2, "2026-10-09T09:40:31Z")],
                                 "0.10.0": [commit_json(date="2025-03-20T12:00:00Z")]})
    assert release_tag.main(["refs"], fetch, refs_file=refs_file) == 0
    out = dict(ln.split("=", 1) for ln in capsys.readouterr().out.splitlines())
    assert "tag" not in out and out["carla_refs"] == "ue5-dev,ue58-dev"
    assert release_tag.main(["refs", "0.10.0"], fetch, refs_file=refs_file) == 0
    out = dict(ln.split("=", 1) for ln in capsys.readouterr().out.splitlines())
    assert json.loads(out["carla_matrix"])[0]["ref"] == "0.10.0"


def test_refs_cli_accepts_a_slash_only_in_the_first_ref(capsys):
    fetch = fake_github_per_ref({"feature%2Fx": [commit_json()],
                                 "ue58-dev": [commit_json(SHA2)]})
    assert release_tag.main(["refs", "feature/x", "ue58-dev"], fetch) == 0
    capsys.readouterr()
    assert release_tag.main(["refs", "ue58-dev", "feature/x"], fetch) == 1
    assert "only the first ref" in capsys.readouterr().err
