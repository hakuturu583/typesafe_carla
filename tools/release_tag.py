"""Release tags: ``<version>-<YYYYMMDD>`` or ``<version>-<CARLA ref>-<YYYYMMDD>``.

    tools/release_tag.py check <tag> [--github-output FILE] [--offline]
    tools/release_tag.py make [<CARLA ref>] [--github-output FILE]
    tools/release_tag.py refs [<CARLA ref>...] [--github-output FILE]

Examples: ``0.5.0-20261009`` (every ref of ``tools/release_carla_refs.txt``),
``0.1.0-ue5-dev-20260915``, ``0.1.0-0.10.0-20250320`` (that ref alone).

A tag without a ref builds every ref listed in ``tools/release_carla_refs.txt``
(of the tagged commit), each from its last commit on or before the date, into
one wheel per platform (``tools/merge_native_wheels.py``); a tag with a ref
builds that ref alone.

- ``<version>`` is everything up to the first ``-`` and must equal the
  package version (``__version__`` in the Python and Codon packages, read by
  ``tools/bump_version.py``);
- ``<YYYYMMDD>`` follows the last ``-``: a real calendar date, the (UTC
  committer) date of the CARLA ref's last commit to build;
- ``<CARLA ref>`` is everything in between (a branch, tag or commit SHA of
  carla-simulator/carla; it may contain ``-``, ``.`` and ``/``) and must not
  be a UE4 ref.

``check`` parses and checks a tag and resolves the CARLA commit it names: the
latest commit reachable from the ref whose committer date is on or before the
end of that day (UTC). It fails if the ref does not exist or has no such
commit, and warns if the commit is from an earlier day. The date must be
before today (UTC), so that the tag always resolves to the same commit. A ref
that is a commit SHA (7 to 40 hex digits) must resolve to that very commit,
committed on the tag date. ``--offline`` skips the lookup.

``make`` forms the tag for the current package version and the ref's latest
commit (used by the auto-release job after ``tools/bump_version.py``). Its date
is that commit's, which may be today: such a tag is not re-checked; the SHA
the job records in the annotated tag and the GitHub release is authoritative.
Without a ref, it forms the tag of every listed ref: each one's latest commit,
dated with the newest of them.

``refs`` resolves refs (default: the listed ones) to their current commits,
without a tag (the release workflow's dry run).

``unpublished`` fails if the package version (or ``--version``) is already on
PyPI (or ``--index testpypi``): PyPI accepts each version once.

Commits are looked up with the GitHub API (``GITHUB_TOKEN`` is used if set).
Outputs are ``key=value`` lines (``tag``, ``version``, ``carla_ref`` as in the
tag, ``carla_commit`` = the SHA LibCarla is built from, ``carla_commit_date``;
the last three only for a tag with a ref), and for every tag ``carla_refs``
(the refs, comma-separated, the default first) and ``carla_matrix`` (JSON, one
``{"index", "ref", "commit", "date"}`` per ref, the default first: the
workflow's build matrix), printed to stdout or appended to FILE
(``$GITHUB_OUTPUT`` in the release workflow); messages go to stderr, or to
stdout with ``--github-output``.
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

try:  # imported as tools.release_tag (tests) or run as tools/release_tag.py
    from tools import bump_version
except ImportError:
    import bump_version

CARLA_REPO = "carla-simulator/carla"
PACKAGE = "typesafe-carla"
REFS_FILE = Path(__file__).resolve().parent / "release_carla_refs.txt"
INDEX_JSON = {"pypi": "https://pypi.org/pypi", "testpypi": "https://test.pypi.org/pypi"}

# A PEP 440 public version in canonical form (it never contains "-").
_VERSION = re.compile(r"\d+(\.\d+)*((a|b|rc)\d+)?(\.post\d+)?(\.dev\d+)?")
# Characters git accepts in branch/tag names that are also safe in
# $GITHUB_OUTPUT and shell words; SHAs are a subset.
_REF = re.compile(r"[A-Za-z0-9][A-Za-z0-9._/-]*")
# UE4 refs, rejected by the build too (cmake/FetchCarla.cmake, _tsc_check_ue5:
# "UE4 refs such as 0.9.x or ue4-dev cannot be used").
_UE4_REF = re.compile(r"(0\.9(\.|$)|ue4([-_/.]|$))", re.IGNORECASE)
# A ref that names a commit by (abbreviated) SHA.
_SHA_REF = re.compile(r"[0-9a-f]{7,40}")
# A listed release ref also names a directory (typesafe_carla/_native/refs/<ref>).
_LISTED_REF = re.compile(r"[A-Za-z0-9][A-Za-z0-9._-]*")


class TagError(ValueError):
    pass


class RefNotFound(TagError):
    """GitHub does not know the ref (HTTP 404 / 422)."""


class ReleaseTag(NamedTuple):
    version: str
    carla_ref: str | None  # None: every ref of tools/release_carla_refs.txt
    date: datetime.date

    def __str__(self) -> str:
        if self.carla_ref is None:
            return f"{self.version}-{self.date:%Y%m%d}"
        return f"{self.version}-{self.carla_ref}-{self.date:%Y%m%d}"


class Commit(NamedTuple):
    sha: str
    date: datetime.datetime  # committer date, UTC

    @property
    def day(self) -> datetime.date:
        return self.date.date()


# fetch(url) -> decoded JSON. Raises RefNotFound for a ref GitHub does not
# know, TagError for any other failure.
Fetch = Callable[[str], Any]
# status(url) -> HTTP status code of a GET.
Status = Callable[[str], int]


def package_version() -> str:
    """The Python and Codon package version (they must agree)."""
    try:
        return bump_version.current_version()
    except SystemExit as e:
        raise TagError(str(e.code)) from None


def is_ue4_ref(ref: str) -> bool:
    return _UE4_REF.match(ref) is not None


def check_ref(ref: str) -> None:
    if not _REF.fullmatch(ref) or ".." in ref or ref.endswith((".", "/", ".lock")):
        raise TagError(f"invalid CARLA ref {ref!r}")
    if is_ue4_ref(ref):
        raise TagError(f"CARLA ref {ref!r} is a UE4 ref; typesafe_carla supports only "
                       "CARLA UE5 (ue5-dev, 0.10.x and later)")


def release_refs(path: Path = REFS_FILE) -> list[str]:
    """The refs of tools/release_carla_refs.txt, the default first."""
    try:
        lines = path.read_text().splitlines()
    except OSError as e:
        raise TagError(f"cannot read the release refs: {e}") from None
    refs = [ln.split("#", 1)[0].strip() for ln in lines]
    refs = [r for r in refs if r]
    if not refs:
        raise TagError(f"{path} lists no CARLA ref")
    for ref in refs:
        check_ref(ref)
        if not _LISTED_REF.fullmatch(ref):
            raise TagError(f"{path}: CARLA ref {ref!r} cannot name a directory "
                           "(typesafe_carla/_native/refs/<ref>)")
    if len(set(refs)) != len(refs):
        raise TagError(f"{path} lists a CARLA ref twice")
    return refs


def check_bundled(refs: list[str]) -> None:
    """Every ref but the first goes to typesafe_carla/_native/refs/<ref>/, so it
    must name a directory; fail before building rather than when merging."""
    for ref in refs[1:]:
        if not _LISTED_REF.fullmatch(ref):
            raise TagError(f"CARLA ref {ref!r} cannot name a directory "
                           "(typesafe_carla/_native/refs/<ref>); only the first ref may contain '/'")


def parse(tag: str, expected_version: str) -> ReleaseTag:
    form = ("expected <version>-<YYYYMMDD> or <version>-<CARLA ref>-<YYYYMMDD>, "
            "e.g. 0.5.0-20261009 or 0.1.0-ue5-dev-20260915")
    version, sep, rest = tag.partition("-")
    if re.fullmatch(r"\d{8}", rest):
        ref: str | None = None
        date_s = rest
    else:
        ref, sep2, date_s = rest.rpartition("-")
        if not (sep2 and ref):
            raise TagError(f"malformed tag {tag!r}: {form}")
    if not sep:
        raise TagError(f"malformed tag {tag!r}: {form}")
    if not _VERSION.fullmatch(version):
        raise TagError(f"malformed version {version!r} in tag {tag!r}: {form}")
    if not re.fullmatch(r"\d{8}", date_s):
        raise TagError(f"malformed date {date_s!r} in tag {tag!r}: {form}")
    try:
        date = datetime.datetime.strptime(date_s, "%Y%m%d").date()
    except ValueError:
        raise TagError(f"{date_s!r} in tag {tag!r} is not a calendar date") from None
    if ref is not None:
        try:
            check_ref(ref)
        except TagError as e:
            raise TagError(f"{e} in tag {tag!r}") from None
    if version != expected_version:
        raise TagError(f"tag version {version} != package version {expected_version}")
    return ReleaseTag(version, ref, date)


def check_past(tag: ReleaseTag, today: datetime.date | None = None) -> None:
    """A hand-cut tag's date must be over (UTC): later commits that day would
    otherwise change what the tag resolves to."""
    today = today or datetime.datetime.now(datetime.timezone.utc).date()
    if tag.date >= today:
        raise TagError(f"tag date {tag.date.isoformat()} is not before today "
                       f"({today.isoformat()}, UTC): commits may still land on that day")


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
        body = e.read().decode(errors="replace")
        if e.code in (404, 422):  # unknown repository / no commit for that sha
            raise RefNotFound(f"GitHub API {e.code} for {url}: {body}") from None
        raise TagError(f"GitHub API {e.code} for {url}: {body}") from None
    except (urllib.error.URLError, OSError, ValueError) as e:
        raise TagError(f"GitHub API request {url} failed: {e}") from None


def http_status(url: str) -> int:
    try:
        with urllib.request.urlopen(url, timeout=30) as r:
            return r.status
    except urllib.error.HTTPError as e:
        return e.code
    except (urllib.error.URLError, OSError) as e:
        raise TagError(f"request {url} failed: {e}") from None


def check_unpublished(version: str, index: str = "pypi", status: Status = http_status) -> None:
    """Fail if ``version`` of typesafe-carla is already on the index."""
    url = f"{INDEX_JSON[index]}/{PACKAGE}/{version}/json"
    code = status(url)
    if code == 200:
        raise TagError(f"{PACKAGE} {version} is already on {index}; PyPI accepts a version "
                       "once, so bump the version (tools/bump_version.py)")
    if code != 404:
        raise TagError(f"unexpected HTTP {code} from {url}")


def resolve_commit(ref: str, day: datetime.date | None = None, fetch: Fetch = github_fetch,
                   repo: str = CARLA_REPO) -> Commit:
    """Latest commit reachable from ``ref``, committed on or before ``day`` (UTC) if given."""
    try:
        commits = fetch(commits_url(ref, day, repo))
    except RefNotFound as e:
        raise TagError(f"CARLA ref {ref!r} not found in {repo}: {e}") from None
    if not commits:
        raise TagError(f"CARLA ref {ref!r} has no commit on or before {day}")
    try:
        c = commits[0]
        sha = c["sha"]
        when = datetime.datetime.fromisoformat(
            c["commit"]["committer"]["date"].replace("Z", "+00:00"))
    except (KeyError, IndexError, TypeError, ValueError) as e:
        raise TagError(f"unexpected GitHub API response for {ref!r}: {e!r}") from None
    if not isinstance(sha, str) or not re.fullmatch(r"[0-9a-f]{40}", sha):
        raise TagError(f"GitHub API returned an invalid commit SHA {sha!r} for {ref!r}")
    commit = Commit(sha, when.astimezone(datetime.timezone.utc))
    if day is not None and commit.day > day:
        raise TagError(f"GitHub returned {commit.sha} from {commit.date.isoformat()}, "
                       f"after {day.isoformat()}")
    if _SHA_REF.fullmatch(ref):
        # GitHub answers a SHA with its newest ancestor on or before the day,
        # which would silently build another commit.
        if not commit.sha.startswith(ref):
            raise TagError(f"CARLA ref {ref!r} is a commit SHA, but it resolves to "
                           f"{commit.sha} (its last ancestor by {day}); tag {ref} "
                           "with its own commit date")
        if day is not None and commit.day != day:
            raise TagError(f"CARLA commit {ref} is from {commit.day.isoformat()}, "
                           f"not {day.isoformat()}")
    return commit


def make_tag(ref: str, version: str, fetch: Fetch = github_fetch) -> tuple[ReleaseTag, Commit]:
    """The tag for ``version`` and the latest commit of ``ref``."""
    check_ref(ref)
    commit = resolve_commit(ref, None, fetch)
    return parse(str(ReleaseTag(version, ref, commit.day)), version), commit


Resolved = list[tuple[str, Commit]]  # (ref, commit), the default ref first


def resolve_refs(refs: list[str], day: datetime.date | None = None,
                 fetch: Fetch = github_fetch) -> Resolved:
    """Each ref's latest commit (on or before ``day``, if given)."""
    for ref in refs:
        check_ref(ref)
    return [(ref, resolve_commit(ref, day, fetch)) for ref in refs]


def make_multi_tag(version: str, refs: list[str],
                   fetch: Fetch = github_fetch) -> tuple[ReleaseTag, Resolved]:
    """The tag for ``version`` and the latest commit of every ref, dated with
    the newest of them: each ref's last commit by that day is the one found."""
    resolved = resolve_refs(refs, None, fetch)
    day = max(commit.day for _, commit in resolved)
    return parse(str(ReleaseTag(version, None, day)), version), resolved


def _matrix(resolved: Resolved) -> str:
    return json.dumps([{"index": i, "ref": ref, "commit": c.sha, "date": c.date.isoformat()}
                       for i, (ref, c) in enumerate(resolved)], separators=(",", ":"))


def _outputs(tag: ReleaseTag | None, resolved: Resolved | None,
             refs: list[str]) -> dict[str, str]:
    out: dict[str, str] = {}
    if tag is not None:
        out = {"tag": str(tag), "version": tag.version}
        if tag.carla_ref is not None:
            out["carla_ref"] = tag.carla_ref
            if resolved:
                out["carla_commit"] = resolved[0][1].sha
                out["carla_commit_date"] = resolved[0][1].date.isoformat()
    out["carla_refs"] = ",".join(refs)
    if resolved is not None:
        out["carla_matrix"] = _matrix(resolved)
    return out


def main(argv: list[str] | None = None, fetch: Fetch = github_fetch,
         status: Status = http_status, today: datetime.date | None = None,
         refs_file: Path = REFS_FILE) -> int:
    parser = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    sub = parser.add_subparsers(dest="command", required=True)
    check = sub.add_parser("check", help="parse and check a tag, resolve its CARLA commit")
    check.add_argument("tag")
    check.add_argument("--offline", action="store_true",
                       help="only parse and check the tag; do not resolve the commit")
    make = sub.add_parser("make", help="form the tag for the package version and the latest "
                                       "commit of a ref (default: of every listed ref)")
    make.add_argument("ref", nargs="?")
    refs_cmd = sub.add_parser("refs", help="resolve refs (default: the listed ones) to their "
                                           "current commits")
    refs_cmd.add_argument("refs", nargs="*")
    unpublished = sub.add_parser("unpublished", help="fail if the version is already on "
                                                     "the index")
    unpublished.add_argument("--version", help="default: the package version")
    unpublished.add_argument("--index", choices=sorted(INDEX_JSON), default="pypi")
    for p in (check, make, refs_cmd, unpublished):
        p.add_argument("--github-output", metavar="FILE",
                       help="append key=value outputs to FILE instead of printing them")
    args = parser.parse_args(argv)
    log = sys.stdout if args.github_output else sys.stderr  # stdout carries outputs otherwise
    tag: ReleaseTag | None = None
    resolved: Resolved | None = None
    try:
        if args.command == "unpublished":
            version = args.version or package_version()
            check_unpublished(version, args.index, status)
            print(f"{PACKAGE} {version} is not on {args.index} yet", file=log)
            return 0
        if args.command == "refs":
            refs = args.refs or release_refs(refs_file)
            check_bundled(refs)
            resolved = resolve_refs(refs, None, fetch)
        elif args.command == "make":
            if args.ref is not None:
                tag, commit = make_tag(args.ref, package_version(), fetch)
                refs, resolved = [args.ref], [(args.ref, commit)]
            else:
                refs = release_refs(refs_file)
                tag, resolved = make_multi_tag(package_version(), refs, fetch)
            print(f"{tag}:", file=log)
        else:
            tag = parse(args.tag, package_version())
            check_past(tag, today)
            refs = [tag.carla_ref] if tag.carla_ref is not None else release_refs(refs_file)
            if not args.offline:
                resolved = resolve_refs(refs, tag.date, fetch)
                print(f"CARLA as of {tag.date.isoformat()}:", file=log)
                if tag.carla_ref is not None and resolved[0][1].day < tag.date:
                    print(f"::warning::{tag.carla_ref} has no commit on {tag.date.isoformat()}; "
                          f"building its last earlier commit {resolved[0][1].sha} from "
                          f"{resolved[0][1].day.isoformat()}", file=log)
                elif all(commit.day < tag.date for _, commit in resolved):
                    print(f"::warning::none of {', '.join(refs)} has a commit on "
                          f"{tag.date.isoformat()}; building the last earlier ones", file=log)
        for ref, commit in resolved or []:
            print(f"  {ref} = {commit.sha} ({commit.date.isoformat()})", file=log)
    except TagError as e:
        print(f"::error::{e}" if args.github_output else f"ERROR: {e}", file=sys.stderr)
        return 1
    lines = "".join(f"{k}={v}\n" for k, v in _outputs(tag, resolved, refs).items())
    if args.github_output:
        with open(args.github_output, "a") as f:
            f.write(lines)
    else:
        sys.stdout.write(lines)
    return 0


if __name__ == "__main__":
    sys.exit(main())
