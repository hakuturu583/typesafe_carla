"""CARLA's own PythonAPI tests (PythonAPI/test) run against typesafe_carla.

One case per upstream test file, run by tools/upstream_tests.py from the
CARLA commit the native library was built from and checked against
tests/upstream/expectations.yaml: an unexpected failure and an unexpected
pass both fail. `unit` needs no server; `smoke` and `API` run only with a
server (TSC_CARLA_PORT, TSC_CARLA_HOST) and the libcarla backend, like
tests/test_integration.py. Skipped when the tests cannot be fetched
(offline). See tests/upstream/README.md.
"""

from __future__ import annotations

import os

import pytest

from tools import upstream_tests as up

MANIFEST = up.load_manifest()
# Every file any ref lists, so that collection needs neither the network nor
# the native library; a file the current ref lacks is skipped.
FILES = sorted({rel for entries in MANIFEST.values() for rel in entries},
               key=lambda rel: (up.SUITES.index(rel.split("/")[0]), rel))


@pytest.fixture(scope="session")
def upstream(backend):
    try:
        target = up.resolve_target()
        tests = up.fetch_tests(target.sha)
    except (up.UpstreamError, OSError) as e:
        pytest.skip(f"CARLA's PythonAPI tests are not available: {e}")
    return target, tests, up.cache_root() / target.sha / "build"


def _runnable(suite: str, backend: str) -> None:
    if suite in up.SERVER_SUITES:
        if not os.environ.get("TSC_CARLA_PORT"):
            pytest.skip(f"{suite} needs a CARLA server: set TSC_CARLA_PORT (and TSC_CARLA_HOST)")
        if backend != "libcarla":
            pytest.skip(f"{suite} needs the libcarla backend (built: {backend})")


def _run(upstream, rel: str, record_property) -> up.Check:
    target, tests, work = upstream
    entry = up.file_expectations(MANIFEST, target.ref, rel)
    ids = up.discover(tests, rel)
    if isinstance(entry, str) and up.parse_expectation(entry)[0] == "skip":
        result = None
    else:
        result = up.run_file(tests, rel, work, up.skipped_ids(entry),
                             server=rel.split("/")[0] in up.SERVER_SUITES)
    c = up.check(result, entry, ids)
    known = "" if target.ref in MANIFEST else f" (no expectations for {target.ref})"
    line = f"{rel} [{target.ref} @ {target.sha[:12]}]: {c.counts()}{known}"
    print(line)
    for t in result.tests if result else []:
        print(f"  {t.id}: {t.outcome}{': ' + t.detail if t.detail else ''}")
    record_property("upstream_counts", line)
    return c


@pytest.mark.parametrize("rel", FILES)
def test_upstream(upstream, backend, rel, record_property):
    _runnable(rel.split("/")[0], backend)
    target, tests, _ = upstream
    if not (tests / rel).is_file():
        pytest.skip(f"not in CARLA {target.ref} @ {target.sha[:12]}")
    c = _run(upstream, rel, record_property)
    if target.ref in MANIFEST:
        assert not c.problems, "\n".join(c.problems)


@pytest.mark.parametrize("suite", up.SUITES)
def test_upstream_unlisted(upstream, backend, suite, record_property):
    """Upstream files the manifest does not list yet: their tests must pass."""
    _runnable(suite, backend)
    target, tests, _ = upstream
    new = [rel for rel in up.test_files(tests, suite) if rel not in FILES]
    if not new:
        pytest.skip(f"every {suite} file is listed")
    problems = [p for rel in new for p in _run(upstream, rel, record_property).problems]
    if target.ref in MANIFEST:
        assert not problems, "\n".join(problems)


def test_manifest_is_well_formed():
    assert MANIFEST, "tests/upstream/expectations.yaml has no refs"
    for ref, entries in MANIFEST.items():
        for rel, entry in entries.items():
            assert rel.split("/")[0] in up.SUITES and rel.endswith(".py"), (ref, rel)
            values = [entry] if isinstance(entry, str) else list(entry.values())
            for value in values:
                up.parse_expectation(value)


def test_check_flags_unexpected_failures_and_passes():
    result = up.FileResult("unit/x.py", [up.TestResult("A.test_a", "pass"),
                                         up.TestResult("A.test_b", "fail", "1 != 2"),
                                         up.TestResult("A.test_c", "compile", "no")])
    ok = up.check(result, {"A.test_a": "pass", "A.test_b": "xfail: r", "A.test_c": "xfail: r"})
    assert (ok.passed, ok.xfailed, ok.problems) == (1, 2, [])
    bad = up.check(result, {"A.test_a": "xfail: r", "A.test_c": "skip: r"})
    assert bad.skipped == 1
    assert [p.split(":")[0] for p in bad.problems] == ["A.test_a", "A.test_b"]
    whole = up.FileResult("unit/x.py", [up.TestResult("A.test_a", "compile", "e")], "e")
    assert not up.check(whole, "xfail: e").problems
    assert up.check(up.FileResult("unit/x.py", [up.TestResult("A.test_a", "pass")]),
                    "xfail: e").problems
