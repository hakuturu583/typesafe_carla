"""Release tags of the form ``<version>-<CARLA ref>-<YYYYMMDD>``.

    tools/release_tag.py check <tag> [--github-output FILE] [--offline]
    tools/release_tag.py make <CARLA ref> [--github-output FILE]

Examples: ``0.1.0-ue5-dev-20260915``, ``0.1.0-0.10.0-20250320``.

- ``<version>`` is everything up to the first ``-`` and must equal
  ``__version__`` in ``python/typesafe_carla/__init__.py``;
- ``<YYYYMMDD>`` follows the last ``-``: a real calendar date, the (UTC
  committer) date of the CARLA ref's last commit to build;
- ``<CARLA ref>`` is everything in between (a branch, tag or commit SHA of
  carla-simulator/carla; it may contain ``-``, ``.`` and ``/``) and must not
  be a UE4 ref.

``check`` parses and checks a tag and resolves the CARLA commit it names: the
latest commit reachable from the ref whose committer date is on or before the
end of that day (UTC). It fails if the ref does not exist or has no such
commit, and warns if the commit is from an earlier day. ``--offline`` skips
the lookup.

``make`` forms the tag for the current package version and the ref's latest
commit (used by the auto-release job after ``tools/bump_version.py``).

Commits are looked up with the GitHub API (``GITHUB_TOKEN`` is used if set).
Outputs are ``key=value`` lines (``tag``, ``version``, ``carla_ref`` as in the
tag, ``carla_commit`` = the SHA LibCarla is built from, ``carla_commit_date``),
printed to stdout or appended to FILE (``$GITHUB_OUTPUT`` in the release
workflow); messages go to stderr, or to stdout with ``--github-output``.
"""

from __future__ import annotations

import argparse
import datetime
import json
import os
import re
import sys
import urllib.error
import urllib.parse
import urllib.request
from pathlib import Path
from typing import Any, Callable, NamedTuple

CARLA_REPO = "carla-simulator/carla"
ROOT = Path(__file__).resolve().parent.parent
INIT = ROOT / "python" / "typesafe_carla" / "__init__.py"

# A PEP 440 public version in canonical form (it never contains "-").
_VERSION = re.compile(r"\d+(\.\d+)*((a|b|rc)\d+)?(\.post\d+)?(\.dev\d+)?")
# Characters git accepts in branch/tag names that are also safe in
# $GITHUB_OUTPUT and shell words; SHAs are a subset.
_REF = re.compile(r"[A-Za-z0-9][A-Za-z0-9._/-]*")
# UE4 refs, rejected by the build too (cmake/FetchCarla.cmake, _tsc_check_ue5:
# "UE4 refs such as 0.9.x or ue4-dev cannot be used").
_UE4_REF = re.compile(r"(0\.9(\.|$)|ue4([-_/.]|$))", re.IGNORECASE)


class TagError(ValueError):
    pass


class ReleaseTag(NamedTuple):
    version: str
    carla_ref: str
    date: datetime.date

    def __str__(self) -> str:
        return f"{self.version}-{self.carla_ref}-{self.date:%Y%m%d}"


class Commit(NamedTuple):
    sha: str
    date: datetime.datetime  # committer date, UTC

    @property
    def day(self) -> datetime.date:
        return self.date.date()


# fetch(url) -> decoded JSON. Raises TagError for a ref GitHub does not know.
Fetch = Callable[[str], Any]


def package_version(init: Path = INIT) -> str:
    m = re.search(r'^__version__ = "([^"]+)"', init.read_text(), re.MULTILINE)
    if not m:
        raise TagError(f"no __version__ in {init}")
    return m.group(1)


def is_ue4_ref(ref: str) -> bool:
    return _UE4_REF.match(ref) is not None


def check_ref(ref: str) -> None:
    if not _REF.fullmatch(ref) or ".." in ref or ref.endswith((".", "/", ".lock")):
        raise TagError(f"invalid CARLA ref {ref!r}")
    if is_ue4_ref(ref):
        raise TagError(f"CARLA ref {ref!r} is a UE4 ref; typesafe_carla supports only "
                       "CARLA UE5 (ue5-dev, 0.10.x and later)")


def parse(tag: str, expected_version: str | None = None) -> ReleaseTag:
    form = "expected <version>-<CARLA ref>-<YYYYMMDD>, e.g. 0.1.0-ue5-dev-20260915"
    version, sep, rest = tag.partition("-")
    ref, sep2, date_s = rest.rpartition("-")
    if not (sep and sep2 and ref):
        raise TagError(f"malformed tag {tag!r}: {form}")
    if not _VERSION.fullmatch(version):
        raise TagError(f"malformed version {version!r} in tag {tag!r}: {form}")
    if not re.fullmatch(r"\d{8}", date_s):
        raise TagError(f"malformed date {date_s!r} in tag {tag!r}: {form}")
    try:
        date = datetime.datetime.strptime(date_s, "%Y%m%d").date()
    except ValueError:
        raise TagError(f"{date_s!r} in tag {tag!r} is not a calendar date") from None
    try:
        check_ref(ref)
    except TagError as e:
        raise TagError(f"{e} in tag {tag!r}") from None
    if expected_version is not None and version != expected_version:
        raise TagError(f"tag version {version} != package version {expected_version} "
                       f"({INIT.relative_to(ROOT)})")
    return ReleaseTag(version, ref, date)


def commits_url(ref: str, day: datetime.date | None = None, repo: str = CARLA_REPO) -> str:
    query: dict[str, Any] = {"sha": ref, "per_page": 1}
    if day is not None:
        query["until"] = f"{day.isoformat()}T23:59:59Z"
    return f"https://api.github.com/repos/{repo}/commits?{urllib.parse.urlencode(query)}"


def github_fetch(url: str) -> Any:
    headers = {"Accept": "application/vnd.github+json",
               "X-GitHub-Api-Version": "2022-11-28"}
    token = os.environ.get("GITHUB_TOKEN")
    if token:
        headers["Authorization"] = f"Bearer {token}"
    try:
        with urllib.request.urlopen(urllib.request.Request(url, headers=headers),
                                    timeout=30) as r:
            return json.load(r)
    except urllib.error.HTTPError as e:
        if e.code in (404, 422):  # unknown repository / no commit for that sha
            raise TagError(f"GitHub API {e.code} for {url}: "
                           f"{e.read().decode(errors='replace')}") from None
        raise


def resolve_commit(ref: str, day: datetime.date | None = None, fetch: Fetch = github_fetch,
                   repo: str = CARLA_REPO) -> Commit:
    """Latest commit reachable from ``ref``, committed on or before ``day`` (UTC) if given."""
    try:
        commits = fetch(commits_url(ref, day, repo))
    except TagError as e:
        raise TagError(f"CARLA ref {ref!r} not found in {repo}: {e}") from None
    if not commits:
        raise TagError(f"CARLA ref {ref!r} has no commit on or before {day}")
    c = commits[0]
    when = datetime.datetime.fromisoformat(c["commit"]["committer"]["date"].replace("Z", "+00:00"))
    commit = Commit(c["sha"], when.astimezone(datetime.timezone.utc))
    if day is not None and commit.day > day:
        raise TagError(f"GitHub returned {commit.sha} from {commit.date.isoformat()}, "
                       f"after {day.isoformat()}")
    return commit


def make_tag(ref: str, version: str, fetch: Fetch = github_fetch) -> tuple[ReleaseTag, Commit]:
    """The tag for ``version`` and the latest commit of ``ref``."""
    check_ref(ref)
    commit = resolve_commit(ref, None, fetch)
    return parse(str(ReleaseTag(version, ref, commit.day)), version), commit


def _outputs(tag: ReleaseTag, commit: Commit | None) -> dict[str, str]:
    out = {"tag": str(tag), "version": tag.version, "carla_ref": tag.carla_ref}
    if commit is not None:
        out["carla_commit"] = commit.sha
        out["carla_commit_date"] = commit.date.isoformat()
    return out


def main(argv: list[str] | None = None, fetch: Fetch = github_fetch) -> int:
    parser = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    sub = parser.add_subparsers(dest="command", required=True)
    check = sub.add_parser("check", help="parse and check a tag, resolve its CARLA commit")
    check.add_argument("tag")
    check.add_argument("--offline", action="store_true",
                       help="only parse and check the tag; do not resolve the commit")
    make = sub.add_parser("make", help="form the tag for the package version and a ref's "
                                       "latest commit")
    make.add_argument("ref")
    for p in (check, make):
        p.add_argument("--github-output", metavar="FILE",
                       help="append key=value outputs to FILE instead of printing them")
    args = parser.parse_args(argv)
    log = sys.stdout if args.github_output else sys.stderr  # stdout carries outputs otherwise
    try:
        if args.command == "make":
            tag, commit = make_tag(args.ref, package_version(), fetch)
            print(f"{tag}: CARLA {tag.carla_ref} = {commit.sha} ({commit.date.isoformat()})",
                  file=log)
        else:
            tag = parse(args.tag, package_version())
            commit = None
            if not args.offline:
                commit = resolve_commit(tag.carla_ref, tag.date, fetch)
                print(f"CARLA {tag.carla_ref} as of {tag.date.isoformat()}: "
                      f"{commit.sha} ({commit.date.isoformat()})", file=log)
                if commit.day < tag.date:
                    print(f"::warning::{tag.carla_ref} has no commit on {tag.date.isoformat()}; "
                          f"building its last earlier commit {commit.sha} from "
                          f"{commit.day.isoformat()}", file=log)
    except TagError as e:
        print(f"::error::{e}" if args.github_output else f"ERROR: {e}", file=sys.stderr)
        return 1
    lines = "".join(f"{k}={v}\n" for k, v in _outputs(tag, commit).items())
    if args.github_output:
        with open(args.github_output, "a") as f:
            f.write(lines)
    else:
        sys.stdout.write(lines)
    return 0


if __name__ == "__main__":
    sys.exit(main())
