"""CARLA's own PythonAPI tests (PythonAPI/test) run against typesafe_carla.

One case per mode and upstream file, run by tools/upstream_tests.py from the
CARLA commit the native library was built from, and checked against
tests/upstream/expectations.yaml: an unexpected failure and an unexpected
pass both fail.

* cpython mode: the unmodified tests under CPython, `import carla` being
  typesafe_carla through tools/pycarla (skipped unless that package is built
  and up to date, or TSC_UPSTREAM_BUILD_PYCARLA=1 builds it);
* codon mode: the tests converted and compiled with typesafe-codon.

`unit` needs no server; `smoke`, `API` and the top-level files run only with a
server (TSC_CARLA_PORT, TSC_CARLA_HOST) and the libcarla backend, like
tests/test_integration.py. Skipped when the tests cannot be fetched
(offline). See tests/upstream/README.md.
"""

from __future__ import annotations

import os

import pytest

from tools import upstream_tests as up

MANIFESTS = {mode: up.load_manifest(mode) for mode in up.MODES}
# Every file any ref lists, so that collection needs neither the network nor
# the native library; a file the current ref lacks is skipped.
FILES = {mode: sorted({rel for entries in MANIFESTS[mode].values() for rel in entries},
                      key=up._file_order) for mode in up.MODES}
CASES = [(mode, rel) for mode in up.MODES for rel in FILES[mode]]


@pytest.fixture(scope="session")
def upstream(backend):
    try:
        target = up.resolve_target()
        tests = up.fetch_tests(target.sha)
    except (up.UpstreamError, OSError) as e:
        pytest.skip(f"CARLA's PythonAPI tests are not available: {e}")
    return target, tests, up.cache_root() / target.sha / "build"


@pytest.fixture(scope="session")
def pycarla(upstream):
    # Building the package takes ~15 minutes: pytest uses an up-to-date one,
    # and builds it only on request (CI's libcarla legs build it through the CLI).
    build = os.environ.get("TSC_UPSTREAM_BUILD_PYCARLA") == "1"
    pydir = up.pycarla_dir(build=build)
    if pydir is None:
        pytest.skip("cpython mode needs the carla package: run `python -m tools.pycarla` "
                    "or set TSC_UPSTREAM_BUILD_PYCARLA=1")
    return pydir


def _runnable(suite: str, backend: str) -> None:
    if suite in up.SERVER_SUITES:
        if not os.environ.get("TSC_CARLA_PORT"):
            pytest.skip(f"{suite} needs a CARLA server: set TSC_CARLA_PORT (and TSC_CARLA_HOST)")
        if backend != "libcarla":
            pytest.skip(f"{suite} needs the libcarla backend (built: {backend})")


def _run(request, mode: str, upstream, rel: str, record_property) -> up.Check:
    target, tests, work = upstream
    manifest = MANIFESTS[mode]
    entry = up.file_expectations(manifest, target.ref, rel)
    ids = up.discover(tests, rel) or [up.SCRIPT]
    server = up.suite_of(rel) in up.SERVER_SUITES
    if isinstance(entry, str) and up.parse_expectation(entry)[0] == "skip":
        result = None
    elif mode == "cpython":
        result = up.run_file_cpython(tests, rel, request.getfixturevalue("pycarla"),
                                     up.skipped_ids(entry), server)
    else:
        result = up.run_file(tests, rel, work, up.skipped_ids(entry), server)
    c = up.check(result, entry, ids)
    known = "" if target.ref in manifest else f" (no expectations for {target.ref})"
    line = f"[{mode}] {rel} [{target.ref} @ {target.sha[:12]}]: {c.counts()}{known}"
    print(line)
    for t in result.tests if result else []:
        print(f"  {t.id}: {t.outcome}{': ' + t.detail if t.detail else ''}")
    record_property("upstream_counts", line)
    return c


@pytest.mark.parametrize("mode,rel", CASES, ids=[f"{m}-{r}" for m, r in CASES])
def test_upstream(request, upstream, backend, mode, rel, record_property):
    _runnable(up.suite_of(rel), backend)
    target, tests, _ = upstream
    if not (tests / rel).is_file():
        pytest.skip(f"not in CARLA {target.ref} @ {target.sha[:12]}")
    c = _run(request, mode, upstream, rel, record_property)
    if target.ref in MANIFESTS[mode]:
        assert not c.problems, "\n".join(c.problems)


@pytest.mark.parametrize("mode", up.MODES)
@pytest.mark.parametrize("suite", up.SUITES)
def test_upstream_unlisted(request, upstream, backend, mode, suite, record_property):
    """Upstream files the manifest does not list yet: their tests must pass."""
    _runnable(suite, backend)
    target, tests, _ = upstream
    new = [rel for rel in up.test_files(tests, suite) if rel not in FILES[mode]]
    if not new:
        pytest.skip(f"every {suite} file is listed")
    problems = [p for rel in new for p in _run(request, mode, upstream, rel, record_property).problems]
    if target.ref in MANIFESTS[mode]:
        assert not problems, "\n".join(problems)


def test_manifest_is_well_formed():
    assert any(MANIFESTS.values()), "tests/upstream/expectations.yaml has no entries"
    for mode, refs in MANIFESTS.items():
        for ref, entries in refs.items():
            for rel, entry in entries.items():
                assert up.suite_of(rel) in up.SUITES and rel.endswith(".py"), (mode, ref, rel)
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


def test_root_causes():
    rc = up.root_cause
    assert rc("cpython", "error", "AttributeError: 'Location' object has no attribute 'foo'") == \
        ("missing", "missing `carla.Location.foo`")
    assert rc("cpython", "error", "NotImplementedError: pycarla: World.on_tick not wrapped: x")[0] == "pycarla"
    assert rc("cpython", "error", "ModuleNotFoundError: No module named 'numpy'")[0] == "infrastructure"
    assert rc("codon", "compile", "unit/a.py:3: no module named 'ast'") == \
        ("codon", "does not compile: no module named 'ast'")
    assert rc("cpython", "fail", "AssertionError: 1 != 2")[0] == "behaviour"
