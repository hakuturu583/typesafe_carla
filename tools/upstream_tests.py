"""Runs CARLA's own PythonAPI test suite against typesafe_carla.

CARLA's tests (``PythonAPI/test/{unit,smoke,API}`` of carla-simulator/carla)
are fetched at the commit typesafe_carla's LibCarla was built from, converted
mechanically, compiled with ``typesafe-codon build`` and run test by test.
``tests/upstream/expectations.yaml`` says, per CARLA ref, which tests are
expected to pass; ``tests/test_upstream.py`` and the CI step fail on any
difference, an unexpected pass included, so the list only ever shrinks.

The conversion is generic, the same for every module (no per-test edits):

* ``import carla`` / ``from carla import ...`` -> ``typesafe_carla``, the
  Python-API compatibility path (its compile-time warnings are expected);
* ``import unittest`` -> ``tests/upstream/tsc_unittest.codon``, a minimal
  unittest (Codon's bundled one has no setUp/tearDown, skipTest or
  raising assertions);
* Python features Codon spells differently, with the same meaning:
  ``super(Class, self)`` -> ``super()``; ``from __future__`` imports dropped;
  ``__file__`` defined (the upstream path);
* smoke/API suites only: the server address (``('localhost', <port>)``
  literals and ``carla.Client()`` without arguments) -> TSC_CARLA_HOST /
  TSC_CARLA_PORT;
* a runner is appended: ``<exe> Class.test_method`` runs one test (setUp,
  the test, tearDown) and prints its outcome. Each test runs in its own
  process, so a crash fails one test, not the file.

Codon type-checks only functions that are called, so a test that does not
compile is dropped from the runner (and recorded with its first compiler
error) and the module is compiled again; an error outside every test (module
level code) fails the whole file.

    python -m tools.upstream_tests [--suite unit] [--summary FILE] [--update]

Environment: TSC_UPSTREAM_REF (the CARLA ref whose tests and expectations to
use; default: the ref the native library was built from, ue5-dev for the mock
backend), TSC_UPSTREAM_TESTS_DIR (a local PythonAPI/test instead of fetching),
TSC_UPSTREAM_CACHE (where fetched tests and builds go; default:
<build dir>/upstream-tests), TSC_CARLA_HOST / TSC_CARLA_PORT (the server for
smoke and API).
"""

from __future__ import annotations

import argparse
import ast
import concurrent.futures
import json
import os
import re
import shutil
import signal
import subprocess
import sys
import tempfile
from dataclasses import dataclass, field
from pathlib import Path

from tools.pycarla import ANSI, error_chains, typesafe_codon

ROOT = Path(__file__).resolve().parent.parent
UPSTREAM_DIR = ROOT / "tests" / "upstream"
MANIFEST = UPSTREAM_DIR / "expectations.yaml"
SHIM = UPSTREAM_DIR / "tsc_unittest.codon"
REPOSITORY = "https://github.com/carla-simulator/carla"
# top: the files directly in PythonAPI/test; ported: tests/upstream/ported
# (map-dependent tests ported to maps shipped in ue5-dev, issue #80).
SUITES = ("unit", "smoke", "API", "top", "ported")
SERVER_SUITES = ("smoke", "API", "top", "ported")
MODES = ("cpython", "codon")   # with expectations; `official` runs CARLA's own module
PORTED_DIR = UPSTREAM_DIR / "ported"
RESULTS = UPSTREAM_DIR / "results"
GAPS = UPSTREAM_DIR / "GAPS.md"
DEFAULT_MOCK_REF = "ue5-dev"  # the mock mirrors LibCarla ue5-dev
REASON_MAX = 300
SCRIPT = "<script>"  # the one "test" of a file without TestCase classes
# Time limits in seconds: one test (or script), one on a server, one compile.
TIMEOUT, SERVER_TIMEOUT, BUILD_TIMEOUT = 120, 900, 900

RESULT_LINE = re.compile(r"^TSC-RESULT (?P<status>\w+)(?: (?P<detail>.*))?$")

if str(ROOT / "python") not in sys.path:
    try:
        import typesafe_carla  # noqa: F401
    except ImportError:
        sys.path.insert(0, str(ROOT / "python"))


class UpstreamError(RuntimeError):
    """The upstream tests cannot be located or fetched."""


# ---------------------------------------------------------------------------
# Which CARLA tests: the ref and commit of the native library
# ---------------------------------------------------------------------------

@dataclass(frozen=True)
class Target:
    ref: str      # manifest key: a branch or tag name (ue5-dev, 0.10.0)
    sha: str      # commit the tests are fetched at
    backend: str  # libcarla or mock


def _is_sha(text: str) -> bool:
    return re.fullmatch(r"[0-9a-f]{40}", text or "") is not None


def resolve_ref(ref: str) -> str:
    """The commit SHA `ref` points at now (tools/resolve_carla_ref.sh)."""
    result = subprocess.run(["bash", str(ROOT / "tools" / "resolve_carla_ref.sh"), ref],
                            capture_output=True, text=True, timeout=120)
    if result.returncode != 0:
        raise UpstreamError(f"cannot resolve CARLA ref {ref!r}: {result.stderr.strip()}")
    return result.stdout.strip()


def _ref_of_commit(commit: str) -> str:
    """The manifest ref whose current commit is `commit` (a build from a local checkout)."""
    for ref in load_manifest():
        try:
            if resolve_ref(ref) == commit:
                return ref
        except UpstreamError:
            pass
    return ""


def resolve_target() -> Target:
    """The CARLA ref and commit whose tests apply to the native library in use.

    A LibCarla build is tested with the tests of the commit it was built from,
    under the ref name it recorded (TSC_CARLA_REF_NAME in CI; for a build from
    a local checkout, its CARLA version). TSC_UPSTREAM_REF overrides both.
    The mock backend uses ue5-dev's current tests.
    """
    from typesafe_carla import paths

    info = paths.native_info()
    backend = info.get("backend", "")
    override = os.environ.get("TSC_UPSTREAM_REF", "")
    if override and not re.fullmatch(r"[A-Za-z0-9][A-Za-z0-9._-]*", override):
        # It names result files (results/<mode>/<ref>.json) and git refs.
        raise SystemExit(f"TSC_UPSTREAM_REF={override!r}: not a ref name (letters, digits, . _ -)")
    if backend == "libcarla":
        built_ref = info.get("carla_git_ref", "")
        commit = info.get("carla_git_commit", "")
        named = built_ref not in ("", "local") and not built_ref.startswith("n/a") and not _is_sha(built_ref)
        ref = override or (built_ref if named else _ref_of_commit(commit) or info.get("libcarla_version", ""))
        if _is_sha(commit) and (not override or override == built_ref or not named):
            return Target(ref, commit, backend)
        return Target(ref, resolve_ref(ref), backend)
    ref = override or DEFAULT_MOCK_REF
    return Target(ref, ref if _is_sha(ref) else resolve_ref(ref), backend)


def cache_root() -> Path:
    explicit = os.environ.get("TSC_UPSTREAM_CACHE")
    if explicit:
        return Path(explicit).expanduser().resolve()
    from typesafe_carla import paths

    if paths._source_root() is not None:
        return paths.native_library().parent / "upstream-tests"
    return paths._cache_root() / "upstream-tests"


def _git(*args: str, cwd: Path) -> None:
    result = subprocess.run(["git", "-c", "advice.detachedHead=false", *args], cwd=cwd,
                            capture_output=True, text=True, timeout=600)
    if result.returncode != 0:
        raise UpstreamError(f"git {' '.join(args)} failed: {result.stderr.strip()}")


def fetch_tests(sha: str, cache: Path | None = None) -> Path:
    """PythonAPI/test of carla-simulator/carla at `sha`, cached per SHA.

    A shallow, blob-filtered, sparse fetch of PythonAPI/ and the few
    top-level files the unit tests check (as cmake/FetchCarla.cmake fetches
    LibCarla). TSC_UPSTREAM_TESTS_DIR names a local PythonAPI/test to use
    instead.
    """
    local = os.environ.get("TSC_UPSTREAM_TESTS_DIR")
    if local:
        path = Path(local).expanduser().resolve()
        if not (path / "unit").is_dir():
            raise UpstreamError(f"TSC_UPSTREAM_TESTS_DIR={local} is not a PythonAPI/test directory")
        return path
    cache = cache or cache_root()
    dest = cache / sha
    tests = dest / "PythonAPI" / "test"
    if (dest / ".complete-v2").is_file():
        return tests
    cache.mkdir(parents=True, exist_ok=True)
    tmp = Path(tempfile.mkdtemp(prefix=f"{sha}.", dir=cache))
    try:
        _git("init", "-q", ".", cwd=tmp)
        _git("remote", "add", "origin", REPOSITORY, cwd=tmp)
        # PythonAPI/ and the files some unit tests check (CMake options,
        # requirements); not the multi-GB rest.
        _git("sparse-checkout", "set", "--no-cone", "/PythonAPI/", "/CMake/", "/requirements.txt",
             cwd=tmp)
        _git("fetch", "-q", "--depth", "1", "--filter=blob:none", "origin", sha, cwd=tmp)
        _git("checkout", "-q", "FETCH_HEAD", cwd=tmp)
        shutil.rmtree(tmp / ".git")
        (tmp / ".complete-v2").write_text(sha + "\n")
        shutil.rmtree(dest, ignore_errors=True)
        tmp.rename(dest)
    finally:
        shutil.rmtree(tmp, ignore_errors=True)
    return tests


def suite_of(rel: str) -> str:
    return rel.split("/")[0] if "/" in rel else "top"


def test_files(tests: Path, suite: str) -> list[str]:
    """Every Python file of a suite (unittest modules and scripts), relative
    to PythonAPI/test (`ported/...` for the ported suite)."""
    if suite == "ported":
        return sorted(f"ported/{p.relative_to(PORTED_DIR).as_posix()}"
                      for p in PORTED_DIR.rglob("*.py") if p.name != "__init__.py")
    d = tests if suite == "top" else tests / suite
    return sorted((p.name if suite == "top" else f"{suite}/{p.name}")
                  for p in d.glob("*.py") if p.name != "__init__.py")


def locate(tests: Path, rel: str) -> tuple[Path, str]:
    """(root, path below it) of a test file: the upstream checkout, or
    tests/upstream/ported for `ported/...`."""
    if rel.startswith("ported/"):
        return PORTED_DIR, rel[len("ported/"):]
    return tests, rel


# ---------------------------------------------------------------------------
# Discovery: TestCase classes and their test_* methods (from the Python AST)
# ---------------------------------------------------------------------------

def _classes(source: str) -> dict[str, tuple[list[str], list[str]]]:
    """name -> (base names, method names) of the module's top-level classes."""
    out = {}
    for node in ast.parse(source).body:
        if isinstance(node, ast.ClassDef):
            bases = [b.attr if isinstance(b, ast.Attribute) else getattr(b, "id", "")
                     for b in node.bases]
            methods = [n.name for n in node.body
                       if isinstance(n, (ast.FunctionDef, ast.AsyncFunctionDef))]
            out[node.name] = (bases, methods)
    return out


def _test_classes(classes: dict, known: dict) -> dict[str, list[str]]:
    """TestCase subclasses -> their test_* methods (inherited ones included)."""
    allc = {**known, **classes}

    def is_case(name: str, seen=()) -> bool:
        if name == "TestCase":
            return True
        if name not in allc or name in seen:
            return False
        return any(is_case(b, seen + (name,)) for b in allc[name][0])

    def methods(name: str, seen=()) -> list[str]:
        if name not in allc or name in seen:
            return []
        out = []
        for base in allc[name][0]:
            out += [m for m in methods(base, seen + (name,)) if m not in out]
        out += [m for m in allc[name][1] if m.startswith("test") and m not in out]
        return out

    return {name: methods(name) for name in classes if is_case(name)}


def discover(tests: Path, rel: str) -> list[str]:
    """Test ids (`Class.test_method`) of one upstream file, in source order."""
    root, rel = locate(tests, rel)
    path = root / rel
    init = path.parent / "__init__.py"
    known = _classes(init.read_text()) if init.is_file() else {}
    cases = _test_classes(_classes(path.read_text()), known)
    return [f"{cls}.{m}" for cls, ms in cases.items() for m in ms]


# ---------------------------------------------------------------------------
# Conversion
# ---------------------------------------------------------------------------

_REWRITES = [
    (re.compile(r"^(\s*)from\s+__future__\s+import\s.*$"), r""),
    (re.compile(r"^(\s*)import\s+carla(\s+as\s+\w+)?\s*(#.*)?$"),
     lambda m: f"{m[1]}import typesafe_carla{m[2] or ' as carla'}"),
    (re.compile(r"^(\s*)from\s+carla\s+import\s"), r"\1from typesafe_carla import "),
    (re.compile(r"^(\s*)import\s+unittest\s*(#.*)?$"), r"\1import tsc_unittest as unittest"),
    (re.compile(r"^(\s*)from\s+unittest\s+import\s"), r"\1from tsc_unittest import "),
    (re.compile(r"\bsuper\(\s*\w+\s*,\s*self\s*\)"), r"super()"),
]
_SERVER_REWRITES = [
    (re.compile(r"\(\s*(['\"])(?:localhost|127\.0\.0\.1)\1\s*,\s*\d+\s*\)"), r"(_TSC_HOST, _TSC_PORT)"),
    (re.compile(r"\bcarla\.Client\(\s*\)"), r"carla.Client(_TSC_HOST, _TSC_PORT)"),
    (re.compile(r"\bcarla\.Client\(\s*(['\"])(?:localhost|127\.0\.0\.1)\1\s*,\s*\d+\s*"),
     r"carla.Client(_TSC_HOST, _TSC_PORT"),
]


def _codon_str(text: str) -> str:
    return '"' + text.replace("\\", "\\\\").replace('"', '\\"') + '"'


def _egg_locator_lines(source: str) -> set[int]:
    """Lines (1-based) of the boilerplate that puts CARLA's egg on sys.path.

    `try: sys.path.append(glob.glob('../carla/dist/carla-*.egg')[0])
    except IndexError: pass` locates the `carla` module, which the conversion
    replaces with typesafe_carla. Its `import glob` goes too when nothing else
    uses glob (Codon has no glob module).
    """
    tree = ast.parse(source)

    def is_path_edit(stmt) -> bool:
        call = getattr(stmt, "value", None)
        func = getattr(call, "func", None)
        return (isinstance(stmt, ast.Expr) and isinstance(call, ast.Call)
                and isinstance(func, ast.Attribute) and func.attr in ("append", "insert")
                and isinstance(func.value, ast.Attribute) and func.value.attr == "path"
                and getattr(func.value.value, "id", None) == "sys")

    removed = [n for n in tree.body
               if isinstance(n, ast.Try) and n.body and all(map(is_path_edit, n.body))]
    lines = {ln for n in removed for ln in range(n.lineno, n.end_lineno + 1)}
    if removed:
        uses = [n for n in ast.walk(tree) if isinstance(n, ast.Name) and n.id == "glob"
                and n.lineno not in lines]
        if not uses:
            lines |= {n.lineno for n in tree.body if isinstance(n, ast.Import)
                      and [a.name for a in n.names] == ["glob"] and n.names[0].asname is None}
    return lines


def convert_source(source: str, origin: Path, server: bool) -> tuple[str, int]:
    """The converted module and the number of header lines put in front."""
    header = [f"__file__ = {_codon_str(str(origin))}"]
    if server:
        header += [f"_TSC_HOST = {_codon_str(os.environ.get('TSC_CARLA_HOST', 'localhost'))}",
                   f"_TSC_PORT = {int(os.environ.get('TSC_CARLA_PORT', '2000'))}"]
    drop = _egg_locator_lines(source)
    lines = []
    for number, line in enumerate(source.splitlines(), 1):
        if number in drop:
            lines.append("")
            continue
        for pattern, repl in _REWRITES + (_SERVER_REWRITES if server else []):
            line = pattern.sub(repl, line)
        lines.append(line)
    return "\n".join(header + lines) + "\n", len(header)


# Exceptions the runner reports by name; anything else is caught bare (Codon
# exceptions do not form a catchable hierarchy).
_EXCEPTIONS = ["ValueError", "IndexError", "KeyError", "TypeError", "RuntimeError",
               "AttributeError", "NotImplementedError", "ZeroDivisionError", "OSError",
               "StopIteration", "OverflowError", "_tsc_carla.CarlaError",
               "_tsc_carla.TimeoutError", "_tsc_carla.ActorTypeError",
               "_tsc_carla.VersionError"]


def runner_source(ids: list[str], first_line: int) -> tuple[str, dict[str, tuple[int, int]]]:
    """The appended runner and, per test, the line range of its code."""
    out = ["", "# ---- typesafe_carla upstream-test runner (tools/upstream_tests.py) ----",
           "import sys as _tsc_sys", "import tsc_unittest as _tsc_unittest",
           "import typesafe_carla as _tsc_carla", ""]
    ranges = {}
    for i, test_id in enumerate(ids):
        cls, method = test_id.split(".", 1)
        start = first_line + len(out)
        out += [f"def _tsc_t{i}():",
                f"    _tsc_case = {cls}()",
                "    _tsc_case.setUp()",
                "    try:",
                f"        _tsc_case.{method}()",
                "    finally:",
                "        _tsc_case.tearDown()",
                f"_TSC_TESTS_{i} = {_codon_str(test_id)}",
                ""]
        ranges[test_id] = (start, first_line + len(out) - 1)
    out += ["def _tsc_result(status: str, detail: str):",
            "    print('TSC-RESULT ' + status + ' ' + detail.replace('\\n', ' '))",
            "",
            "def _tsc_dispatch(name: str) -> int:",
            "    try:"]
    body = []
    for i, test_id in enumerate(ids):
        start = first_line + len(out) + len(body)
        body += [f"        {'if' if not body else 'elif'} name == _TSC_TESTS_{i}:",
                 f"            _tsc_t{i}()"]
        ranges[test_id] = ranges[test_id] + (start, start + 1)
    body += [f"        {'if' if not body else 'elif'} True:",
             "            _tsc_result('unknown', name)",
             "            return 3"]
    out += body
    out += ["    except _tsc_unittest.SkipTest as e:",
            "        _tsc_result('skip', e.message)",
            "        return 0",
            "    except AssertionError as e:",
            "        _tsc_result('fail', e.message)",
            "        return 1"]
    for exc in _EXCEPTIONS:
        out += [f"    except {exc} as e:",
                f"        _tsc_result('error', {_codon_str(exc.removeprefix('_tsc_carla.'))} + ': ' + e.message)",
                "        return 2"]
    out += ["    except:",
            "        _tsc_result('error', 'exception')",
            "        return 2",
            "    _tsc_result('pass', '')",
            "    return 0",
            "",
            "_tsc_sys.exit(_tsc_dispatch(_tsc_sys.argv[1] if len(_tsc_sys.argv) > 1 else ''))",
            ""]
    return "\n".join(out), ranges


# ---------------------------------------------------------------------------
# Compile and run
# ---------------------------------------------------------------------------

@dataclass
class TestResult:
    id: str
    outcome: str  # pass, fail, error, crash, timeout, skip, compile, not-run
    detail: str = ""
    trace: str = ""  # a failure's last traceback frames and full message


@dataclass
class FileResult:
    path: str                      # e.g. unit/test_transform.py
    tests: list[TestResult] = field(default_factory=list)
    file_error: str | None = None  # the module does not compile at all
    log: str = ""


def _timeout(timeout: float | None, server: bool) -> float:
    return timeout or (SERVER_TIMEOUT if server else TIMEOUT)


def _first_line(text: str) -> str:
    """The first non-blank line (Codon prints an uncaught error first, then a backtrace)."""
    return next((ln for ln in ANSI.sub("", text).splitlines() if ln.strip()), "")


def _last_line(stderr: str) -> str:
    return (ANSI.sub("", stderr).strip().splitlines() or ["compilation failed"])[-1]


def _execute(cmd: list[str], cwd: Path, timeout: float,
             env: dict[str, str] | None = None) -> subprocess.CompletedProcess | None:
    """Runs one test process; None if it timed out. The process gets its own
    session, and a time-out kills the whole group (a script's children too)."""
    with subprocess.Popen(cmd, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True, cwd=cwd,
                          env=env, errors="replace", start_new_session=True) as proc:
        try:
            out, err = proc.communicate(timeout=timeout)
        except subprocess.TimeoutExpired:
            try:
                os.killpg(proc.pid, signal.SIGKILL)
            except ProcessLookupError:
                pass
            proc.communicate()
            return None
    return subprocess.CompletedProcess(cmd, proc.returncode, out, err)


def _timed_out(test_id: str, timeout: float) -> TestResult:
    return TestResult(test_id, "timeout", f"timed out after {timeout:g} s")


def _signal(returncode: int) -> str:
    try:
        return signal.Signals(-returncode).name
    except ValueError:
        return f"signal {-returncode}"


def _build(source: Path, exe: Path) -> subprocess.CompletedProcess:
    return typesafe_codon("build", "-o", str(exe), str(source), timeout=BUILD_TIMEOUT)


def _shorten(text: str) -> str:
    text = " ".join(text.split())
    return text if len(text) <= REASON_MAX else text[:REASON_MAX - 3] + "..."


def _located(chain: list[tuple[str, int, str]], modules: dict[str, tuple[str, int, int]]) -> str:
    """The error's message at its innermost line of upstream code.

    `modules`: converted file name -> (upstream path, header lines, last line
    before the runner). An error inside typesafe_carla or the unittest shim is
    reported where the test's code reached it (`during the realization of`).
    """
    file, line, msg = chain[0]
    for f, ln, _ in chain:
        if f in modules and ln <= modules[f][2]:
            where = f"{modules[f][0]}:{ln - modules[f][1]}"
            return f"{where}: {msg}" if (f, ln) == (file, line) else f"{where}: {msg} [{file}:{line}]"
    return f"{file}:{line}: {msg}"


def _attribute(stderr: str, included: list[str], ranges: dict[str, tuple[int, ...]],
               source: str, modules: dict) -> tuple[str | None, dict[str, str]]:
    """A failed build's errors: (the first outside every test, {test: its first error})."""
    file_error, culprits = None, {}
    for chain in error_chains(stderr):
        primary = _located(chain, modules)
        hit = [t for t in included
               if any(f == source and (ranges[t][0] <= ln <= ranges[t][1]
                                       or ranges[t][2] <= ln <= ranges[t][3])
                      for f, ln, _ in chain)]
        if not hit:
            file_error = file_error or primary
        for t in hit:
            culprits.setdefault(t, primary)
    return file_error, culprits


def run_file(tests: Path, rel: str, work: Path, skip: set[str] = frozenset(),
             server: bool = False, timeout: float | None = None,
             compile_only: bool = False) -> FileResult:
    """Converts, compiles and runs one upstream file; `skip` ids are left out.

    With `compile_only`, the tests that compile are not run (outcome not-run).
    """
    result = FileResult(rel)
    discovered = discover(tests, rel)
    ids = [t for t in discovered if t not in skip]
    out_dir = work / Path(rel).parent
    out_dir.mkdir(parents=True, exist_ok=True)
    shutil.copy(SHIM, out_dir / SHIM.name)
    root, inner = locate(tests, rel)
    module, offset = convert_source((root / inner).read_text(), root / inner, server)
    module_end = module.count("\n")
    source = out_dir / (Path(rel).stem + ".codon")
    modules = {source.name: (rel, offset, module_end)}
    init = root / Path(inner).parent / "__init__.py"
    if init.is_file():
        text, init_offset = convert_source(init.read_text(), init, server)
        (out_dir / "__init__.codon").write_text(text)
        modules["__init__.codon"] = (f"{Path(rel).parent}/__init__.py", init_offset, text.count("\n"))
    exe = out_dir / Path(rel).stem
    timeout = _timeout(timeout, server)
    if not discovered:
        result = _run_script_codon(result, module, modules, source, exe, out_dir, timeout, compile_only)
        if server and not compile_only and not result.file_error:
            _server_cleanup(out_dir, None, _codon_cleanup(work))
        return result
    # A test that does not compile is dropped from the runner until the rest compiles.
    compile_errors: dict[str, str] = {}
    included = list(ids)
    logs = []
    while True:
        runner, ranges = runner_source(included, module_end + 1)
        source.write_text(module + runner)
        build = _build(source, exe)
        logs.append(ANSI.sub("", build.stderr))
        result.log = "\n".join(logs)
        if build.returncode == 0:
            break
        file_error, culprits = _attribute(build.stderr, included, ranges, source.name, modules)
        if file_error or not culprits:
            result.file_error = _shorten(file_error or _last_line(build.stderr))
            result.tests = [TestResult(t, "compile", result.file_error) for t in ids]
            return result
        compile_errors.update(culprits)
        included = [t for t in included if t not in culprits]
    for test_id in ids:
        if test_id in compile_errors:
            result.tests.append(TestResult(test_id, "compile", _shorten(compile_errors[test_id])))
        elif compile_only:
            result.tests.append(TestResult(test_id, "not-run"))
        else:
            result.tests.append(_run_one(exe, test_id, timeout, out_dir))
            if server:
                _server_cleanup(out_dir, None, _codon_cleanup(work))
    return result


def _run_script_codon(result: FileResult, module: str, modules: dict, source: Path, exe: Path,
                      cwd: Path, timeout: float, compile_only: bool) -> FileResult:
    """A file without TestCase classes is a script: compiled and run whole."""
    source.write_text(module)
    build = _build(source, exe)
    result.log = ANSI.sub("", build.stderr)
    if build.returncode != 0:
        chains = error_chains(build.stderr)
        result.file_error = _shorten(_located(chains[0], modules) if chains else _last_line(build.stderr))
        result.tests = [TestResult(SCRIPT, "compile", result.file_error)]
    elif compile_only:
        result.tests = [TestResult(SCRIPT, "not-run")]
    elif (proc := _execute([str(exe)], cwd, timeout)) is None:
        result.tests = [_timed_out(SCRIPT, timeout)]
    else:
        outcome = "pass" if proc.returncode == 0 else ("crash" if proc.returncode < 0 else "error")
        result.tests = [TestResult(SCRIPT, outcome, "" if outcome == "pass"
                                   else _shorten(f"exit {proc.returncode}: {_first_line(proc.stderr)}"))]
    return result


def _run_one(exe: Path, test_id: str, timeout: float, cwd: Path) -> TestResult:
    proc = _execute([str(exe), test_id], cwd, timeout)
    if proc is None:
        return _timed_out(test_id, timeout)
    lines = [m for m in map(RESULT_LINE.match, proc.stdout.splitlines()) if m]
    first = _first_line(proc.stderr)
    if proc.returncode < 0:
        return TestResult(test_id, "crash", _shorten(f"{_signal(proc.returncode)} {first}"))
    if not lines:
        return TestResult(test_id, "error", _shorten(f"exit {proc.returncode}: {first}"))
    status, detail = lines[-1]["status"], lines[-1]["detail"] or ""
    if status == "unknown":
        status = "error"
    return TestResult(test_id, status, _shorten(detail))


# ---------------------------------------------------------------------------
# CPython mode: the unmodified tests, with `import carla` = tools/pycarla
# ---------------------------------------------------------------------------

# Runs one unittest test by name and prints its outcome as JSON. It runs in
# the test interpreter, with PythonAPI/test as the working directory and the
# generated `carla` package first on sys.path.
_DRIVER = r"""
import json, os, sys, traceback, unittest
# Official mode: CARLA's own module, with every Client sent to the test server
# (as the generated package's TSC_PYCARLA_REDIRECT does).
if os.environ.get("TSC_UPSTREAM_OFFICIAL"):
    import carla
    target = os.environ.get("TSC_PYCARLA_REDIRECT")
    if target:
        _Client = carla.Client
        host, _, port = target.rpartition(":")

        class Client(_Client):
            def __init__(self, *args, **kwargs):
                kwargs = {k: v for k, v in kwargs.items() if k not in ("host", "port")}
                super().__init__(host, int(port), *args[2:], **kwargs)

        carla.Client = Client
    os.environ["TSC_PYCARLA_PKG"] = os.path.dirname(carla.__file__)
# The tests must see the generated package, never an installed official one
# (an editable install's import hook would win over sys.path): load it first.
try:
    if os.environ.get("TSC_UPSTREAM_OFFICIAL"):
        raise StopIteration
    import importlib.util
    pkg = os.environ["TSC_PYCARLA_PKG"]
    spec = importlib.util.spec_from_file_location(
        "carla", os.path.join(pkg, "__init__.py"), submodule_search_locations=[pkg])
    module = importlib.util.module_from_spec(spec)
    sys.modules["carla"] = module
    spec.loader.exec_module(module)
    import carla
    ok = os.path.realpath(carla.__file__).startswith(os.path.realpath(os.environ["TSC_PYCARLA_PKG"]))
    why = f"`import carla` found {carla.__file__}, not the generated package"
except StopIteration:
    ok, why = True, ""
except BaseException as e:
    ok, why = False, "import carla failed: " + "".join(traceback.format_exception_only(type(e), e)).strip()
if not ok:
    print("TSC-RESULT " + json.dumps({"status": "error", "detail": "harness: " + why}), flush=True)
    sys.exit(0)
# Untruncated assertion messages ('Tran[13 chars]...' hides the difference).
import unittest.util
unittest.util._MAX_LENGTH = 10 ** 6
name = sys.argv[1]
if name == "--script":
    import runpy
    sys.argv = [sys.argv[2]]
    runpy.run_path(sys.argv[0], run_name="__main__")
    sys.exit(0)
result = unittest.TestResult()
sys.argv = [name.split(".")[0]]  # a module parsing sys.argv must not see the driver's
try:
    unittest.defaultTestLoader.loadTestsFromName(name).run(result)
except BaseException as e:
    result.errors.append((None, traceback.format_exc()))
def last(tb):
    # The exception line (not the diff lines unittest appends after it).
    import re
    lines = [l for l in tb.strip().splitlines() if l.strip()]
    for i in range(len(lines) - 1, -1, -1):
        if re.match(r"%(EXC_LINE)s", lines[i]):
            return lines[i]
    return lines[-1] if lines else ""
def trace(tb):
    # The last frames and the whole message (an assertion's diff included).
    lines = tb.rstrip().splitlines()
    frames = [i for i, l in enumerate(lines) if l.startswith("  File ")]
    return "\n".join(lines[frames[-%(FRAMES)d:][0] if frames else 0:])[-%(CHARS)d:]
if result.errors:
    tb = result.errors[0][1]
    out = {"status": "error", "trace": trace(tb),
           "detail": ("in tearDown: " if "in tearDown" in tb.split("Traceback")[-1] else "") + last(tb)}
elif result.failures:
    out = {"status": "fail", "detail": last(result.failures[0][1]), "trace": trace(result.failures[0][1])}
elif result.skipped:
    out = {"status": "skip", "detail": result.skipped[0][1]}
elif result.testsRun == 0:
    out = {"status": "error", "detail": "no test ran"}
else:
    out = {"status": "pass", "detail": ""}
print("TSC-RESULT " + json.dumps(out), flush=True)
"""
# How much of a failure's traceback the results keep (TestResult.trace).
TRACE_FRAMES, TRACE_CHARS = 4, 4000
# An exception's line in a traceback (the driver's last() and _script_outcome).
EXC_LINE = r"^[A-Za-z_][\w.]*(?:Error|Exception|Exit|Interrupt|Warning|Empty)\b"
_DRIVER = (_DRIVER.replace("%(FRAMES)d", str(TRACE_FRAMES)).replace("%(CHARS)d", str(TRACE_CHARS))
           .replace("%(EXC_LINE)s", EXC_LINE))


def _trace(text: str) -> str:
    """A traceback's last TRACE_FRAMES frames and its whole message (as the
    driver's trace())."""
    lines = text.rstrip().splitlines()
    frames = [i for i, ln in enumerate(lines) if ln.startswith("  File ")]
    return "\n".join(lines[frames[-TRACE_FRAMES:][0] if frames else 0:])[-TRACE_CHARS:]


# Run after each server test, with the generated package: a test that fails
# before its own clean-up (or whose tearDown fails, e.g. on a missing map)
# must not leave actors or synchronous mode behind for the next one.
_CLEANUP = r"""
import importlib.util, os, sys
if os.environ.get("TSC_UPSTREAM_OFFICIAL"):
    import carla
    host, _, port = os.environ["TSC_PYCARLA_REDIRECT"].rpartition(":")
    client = carla.Client(host, int(port))
else:
    pkg = os.environ["TSC_PYCARLA_PKG"]
    spec = importlib.util.spec_from_file_location("carla", os.path.join(pkg, "__init__.py"),
                                                  submodule_search_locations=[pkg])
    carla = importlib.util.module_from_spec(spec); sys.modules["carla"] = carla; spec.loader.exec_module(carla)
    client = carla.Client("127.0.0.1", 2000)  # redirected to the test server
client.set_timeout(30.0)
try:
    world = client.get_world()
except Exception as e:
    print(f"cleanup: server unreachable: {e}", file=sys.stderr)
    sys.exit(%(UNREACHABLE)d)
try:
    client.get_trafficmanager().set_synchronous_mode(False)
except Exception:
    pass
settings = world.get_settings()
if settings.synchronous_mode:
    settings.synchronous_mode = False
    world.apply_settings(settings)
gone = 0
for actor in world.get_actors():
    if actor.type_id.split(".")[0] in ("vehicle", "walker", "sensor", "controller", "static"):
        try:
            actor.destroy()
            gone += 1
        except Exception:
            pass
print(f"cleanup: destroyed {gone} actors")
"""


# _CLEANUP's exit status when it cannot reach the server.
CLEANUP_UNREACHABLE = 3
_CLEANUP = _CLEANUP.replace("%(UNREACHABLE)d", str(CLEANUP_UNREACHABLE))

# The same clean-up for Codon mode, which has no CPython `carla`: a typesafe
# program, built once per work directory.
_CLEANUP_CODON = """\
import sys
import typesafe_carla as carla
client = carla.Client(%(HOST)s, %(PORT)d)
client.set_timeout(30.0)
try:
    w = client.get_world()
except:
    print("cleanup: server unreachable", file=sys.stderr)
    sys.exit(%(UNREACHABLE)d)
try:
    client.get_trafficmanager().set_synchronous_mode(False)
except:
    pass
settings = w.get_settings()
if settings.synchronous_mode:
    settings.synchronous_mode = False
    w.apply_settings(settings)
gone = 0
for actor in w.get_actors():
    if actor.type_id.split(".")[0] in ("vehicle", "walker", "sensor", "controller", "static"):
        try:
            actor.destroy()
            gone += 1
        except:
            pass
print("cleanup: destroyed " + str(gone) + " actors")
"""


def _codon_cleanup(work: Path) -> list[str]:
    """The command of Codon mode's clean-up program (built if it changed)."""
    source = work / "tsc_cleanup.codon"
    exe = work / "tsc_cleanup"
    text = (_CLEANUP_CODON.replace("%(HOST)s", _codon_str(os.environ.get("TSC_CARLA_HOST", "localhost")))
            .replace("%(PORT)d", str(int(os.environ.get("TSC_CARLA_PORT", "2000"))))
            .replace("%(UNREACHABLE)d", str(CLEANUP_UNREACHABLE)))
    if not exe.is_file() or not source.is_file() or source.read_text() != text:
        work.mkdir(parents=True, exist_ok=True)
        source.write_text(text)
        build = _build(source, exe)
        if build.returncode != 0:
            raise SystemExit(f"upstream tests: the Codon clean-up program does not build:\n{build.stderr}")
    return [str(exe)]


def _server_cleanup(cwd: Path, env: dict[str, str] | None, cmd: list[str] | None = None) -> None:
    """Resets the server after a test (actors, synchronous mode): `cmd` runs
    the clean-up (default: _CLEANUP under the test interpreter). A failure is
    reported; an unreachable server aborts the run rather than letting every
    remaining test time out."""
    cmd = cmd or [_python(), "-c", _CLEANUP]
    try:
        proc = subprocess.run(cmd, cwd=cwd, env=env, capture_output=True, text=True, timeout=120,
                              errors="replace")
    except subprocess.TimeoutExpired:
        print("  cleanup: timed out after 120 s", flush=True)
        return
    if proc.returncode == CLEANUP_UNREACHABLE:
        raise SystemExit(f"upstream tests: the CARLA server is unreachable ({_first_line(proc.stderr)}); "
                         "aborting the run")
    if proc.returncode != 0:
        print(f"  cleanup failed (exit {proc.returncode}): {_last_line(proc.stderr)}", flush=True)


def pycarla_dir(build: bool = True) -> Path | None:
    """The generated `carla` package (tools/pycarla), built if missing or stale
    (unless `build` is False: then None)."""
    from tools import pycarla

    explicit = os.environ.get("TSC_PYCARLA_DIR")
    out = Path(explicit).resolve() if explicit else pycarla.default_out()
    so = out / "carla" / f"{pycarla.MODULE}.so"
    if not pycarla.is_current(out):  # sources, pruned.json or toolchain changed (pycarla.STAMP)
        if explicit and so.is_file():
            return out
        if not build:
            return None
        print(f"building the carla CPython package in {out} (tools/pycarla, ~15 min) ...", flush=True)
        pycarla.build(out)
    return out


def _python() -> str:
    """The interpreter the unmodified tests run in (TSC_UPSTREAM_PYTHON)."""
    return os.environ.get("TSC_UPSTREAM_PYTHON") or sys.executable


def _cpython_env(pydir: Path | None, server: bool) -> dict[str, str]:
    """The test process's environment; `pydir` None runs CARLA's official
    module (TSC_UPSTREAM_PYTHON's own `carla`) instead of the generated one."""
    env = dict(os.environ)
    env.pop("PYTHONHOME", None)
    if pydir is None:
        env["TSC_UPSTREAM_OFFICIAL"] = "1"
        env.pop("TSC_PYCARLA_PKG", None)
        env.setdefault("PYTHONPATH", "")
    else:
        from typesafe_carla import paths

        env["TSC_PYCARLA_PKG"] = str(pydir / "carla")
        # the library this run is about (the package may have been built against another)
        env["TYPESAFE_CARLA_LIB"] = os.environ.get("TYPESAFE_CARLA_LIB") or str(paths.native_library())
        env["PYTHONPATH"] = os.pathsep.join(
            [str(pydir)] + ([os.environ["PYTHONPATH"]] if os.environ.get("PYTHONPATH") else []))
    if server:  # every carla.Client goes to the test server
        env["TSC_PYCARLA_REDIRECT"] = (f"{os.environ.get('TSC_CARLA_HOST', '127.0.0.1')}:"
                                       f"{os.environ.get('TSC_CARLA_PORT', '2000')}")
    return env


def _driver() -> Path:
    """_DRIVER as a file, rewritten when it changed."""
    driver = cache_root() / "tsc_unittest_driver.py"
    driver.parent.mkdir(parents=True, exist_ok=True)
    if not driver.is_file() or driver.read_text() != _DRIVER:
        driver.write_text(_DRIVER)
    return driver


def _driver_result(stdout: str) -> dict | None:
    """The driver's TSC-RESULT line, if it printed one."""
    line = next((ln for ln in reversed(stdout.splitlines()) if ln.startswith("TSC-RESULT ")), None)
    return None if line is None else json.loads(line[len("TSC-RESULT "):])


def run_file_cpython(tests: Path, rel: str, pydir: Path | None, skip: set[str] = frozenset(),
                     server: bool = False, timeout: float | None = None) -> FileResult:
    """Runs one upstream file unmodified under CPython, each test in its own process.

    A unittest module runs test by test; a script runs whole (`<script>`).
    """
    result = FileResult(rel)
    if rel in INTERACTIVE:
        result.tests = [TestResult(SCRIPT, "skip", INTERACTIVE[rel])]
        return result
    timeout = _timeout(timeout, server)
    ids = discover(tests, rel)
    root, inner = locate(tests, rel)
    env = _cpython_env(pydir, server)
    env["PYTHONPATH"] += os.pathsep + str(root)
    driver = str(_driver())
    module = inner[:-3].replace("/", ".")
    runs = ([(t, [driver, f"{module}.{t}"]) for t in ids if t not in skip] if ids
            else [(SCRIPT, [driver, "--script", inner])])
    for test_id, args in runs:
        proc = _execute([_python(), *args], root, timeout, env)
        if proc is None:
            result.tests.append(_timed_out(test_id, timeout))
        elif ids:
            result.tests.append(_test_outcome(test_id, proc))
        else:
            result.tests.append(_script_outcome(proc))
        if server:
            _server_cleanup(root, env)
    return result


def _test_outcome(test_id: str, proc: subprocess.CompletedProcess) -> TestResult:
    out = _driver_result(proc.stdout)
    if proc.returncode < 0:
        return TestResult(test_id, "crash", _shorten(f"{_signal(proc.returncode)} {_first_line(proc.stderr)}"))
    if out is None:
        tail = (proc.stderr.strip().splitlines() or [""])[-1]
        return TestResult(test_id, "error", _shorten(f"exit {proc.returncode}: {tail}"), _trace(proc.stderr))
    return TestResult(test_id, out["status"], _shorten(out["detail"]), out.get("trace", ""))


def _script_outcome(proc: subprocess.CompletedProcess) -> TestResult:
    out = _driver_result(proc.stdout)
    if out is not None:  # the harness check failed
        return TestResult(SCRIPT, out["status"], _shorten(out["detail"]))
    if proc.returncode == 0:
        return TestResult(SCRIPT, "pass")
    if proc.returncode < 0:
        return TestResult(SCRIPT, "crash", _signal(proc.returncode))
    # The exception line, not other output after it (a WARNING from LibCarla).
    lines = [ln for ln in proc.stderr.strip().splitlines() if ln.strip()]
    detail = next((ln for ln in reversed(lines) if re.match(EXC_LINE, ln)), lines[-1] if lines else "")
    return TestResult(SCRIPT, "error", _shorten(f"exit {proc.returncode}: {detail}"), _trace(proc.stderr))


# ---------------------------------------------------------------------------
# Expectations
# ---------------------------------------------------------------------------

def _manifest_data(path: Path = MANIFEST) -> dict:
    import yaml

    data = (yaml.safe_load(path.read_text()) if path.is_file() else None) or {}
    if "refs" in data:  # the format before modes: Codon mode only
        data = {"codon": data.pop("refs"), **data}
    return data


def load_manifest(mode: str = "cpython", path: Path = MANIFEST) -> dict:
    """{ref: {file: entry}} for one mode."""
    return _manifest_data(path).get(mode) or {}


def run_manifest(mode: str, path: Path = MANIFEST) -> dict:
    """What a mode's run follows. Official mode has no expectations: it runs
    what the cpython manifest runs, less the tests in the manifest's `official`
    section, which the official module cannot run (its own defects, recorded
    there with the evidence; typesafe_carla still runs them)."""
    if mode != "official":
        return load_manifest(mode, path)
    manifest = load_manifest("cpython", path)
    for ref, files in load_manifest("official", path).items():
        refs = manifest.setdefault(ref, {})
        for rel, tests in files.items():
            entry = refs.get(rel)
            if not not_run(entry):
                refs[rel] = {**(entry if isinstance(entry, dict) else {}), **tests}
    return manifest


def parse_expectation(value) -> tuple[str, str]:
    """('pass' | 'xfail' | 'skip' | 'selfskip' | 'exclude', reason).
    `skip`: not run (a hang, an interactive script). `selfskip`: run, and
    expected to skip itself (unittest's skipTest; so it is still checked).
    `exclude`: not run and not part of the target, e.g. a test ported to
    tests/upstream/ported."""
    text = str(value).strip()
    kind, _, reason = text.partition(":")
    kind = kind.strip()
    if kind not in ("pass", "xfail", "skip", "selfskip", "exclude"):
        raise ValueError(f"bad expectation {value!r}: use pass, 'xfail: <reason>', "
                         "'skip: <reason>', 'selfskip: <reason>' or 'exclude: <reason>'")
    return kind, reason.strip()


NOT_RUN = ("skip", "exclude")  # expectation kinds whose tests are not run


def known_suites(manifest: dict, ref: str) -> set[str]:
    """The suites `manifest` has expectations for at `ref`. A suite without is
    report-only: its failures are shown, not failed on (e.g. 0.10.0's server
    suites, recorded in codon mode only)."""
    return {suite_of(rel) for rel in manifest.get(ref) or {}}


def kind_of(value) -> str:
    return parse_expectation(value)[0]


def file_expectations(manifest: dict, ref: str, rel: str):
    """A file's entry for `ref`: None, a file-level string, or {test id: value}."""
    return (manifest.get(ref) or {}).get(rel)


def skipped_ids(entry, kinds: tuple[str, ...] = NOT_RUN) -> set[str]:
    """The ids of a per-test entry expected to be one of `kinds`."""
    if isinstance(entry, dict):
        return {t for t, v in entry.items() if kind_of(v) in kinds}
    return set()


def not_run(entry, kinds: tuple[str, ...] = NOT_RUN) -> bool:
    """A file-level skip or exclude (one of `kinds`)."""
    return isinstance(entry, str) and kind_of(entry) in kinds


FAILURES = ("fail", "error", "crash", "timeout", "compile")


@dataclass
class Check:
    passed: int = 0
    xfailed: int = 0
    skipped: int = 0
    excluded: int = 0
    problems: list[str] = field(default_factory=list)

    def counts(self) -> str:
        text = f"{self.passed} pass, {self.xfailed} xfail, {self.skipped} skip"
        text += f", {self.excluded} excluded" if self.excluded else ""
        return text + (f", {len(self.problems)} unexpected" if self.problems else "")


def check(result: FileResult | None, entry, ids: list[str] | None = None) -> Check:
    """Compares a file's results with its expectations.

    A file-level `skip` is not run (`result` is None). A file-level `xfail`
    expects the module not to compile. A test missing from the manifest is
    expected to pass.
    """
    c = Check()
    if isinstance(entry, str) or entry is None and result is None:
        kind, reason = parse_expectation(entry or "pass")
        if kind == "skip":
            c.skipped = len(ids or [])
            return c
        if kind == "exclude":
            c.excluded = len(ids or [])
            return c
        if kind == "xfail":
            if result.file_error:
                c.xfailed = len(result.tests)
            else:
                c.problems.append(f"{result.path}: compiles now (expected xfail: {reason}); "
                                  "list its tests in the manifest")
            return c
    if result is None:  # not run, and no file-level expectation says why
        c.skipped = len(ids or [])
        return c
    entry = entry if isinstance(entry, dict) else {}
    for t in result.tests:
        kind, reason = parse_expectation(entry.get(t.id, "pass"))
        if kind == "skip":
            c.skipped += 1
        elif kind == "exclude":
            c.excluded += 1
        elif t.outcome == "not-run":
            pass
        elif t.outcome == "skip":
            if kind == "selfskip":
                c.skipped += 1
            else:
                c.problems.append(f"{t.id}: skipped itself ({t.detail}); record it as selfskip")
        elif kind == "selfskip":
            c.problems.append(f"{t.id}: unexpected {t.outcome} (expected to skip itself: {reason})")
        elif kind == "pass" and t.outcome == "pass":
            c.passed += 1
        elif kind == "xfail" and t.outcome in FAILURES:
            c.xfailed += 1
        elif kind == "xfail":
            c.problems.append(f"{t.id}: unexpected pass (expected xfail: {reason}); mark it pass")
        else:
            c.problems.append(f"{t.id}: unexpected {t.outcome}: {t.detail}")
    for t in entry:
        if t not in {r.id for r in result.tests} and kind_of(entry[t]) not in NOT_RUN:
            c.problems.append(f"{t}: in the manifest but not in the upstream file")
    return c


def _reason(t: TestResult) -> str:
    return _shorten(f"{t.outcome}: {t.detail}" if t.detail else t.outcome)


def updated_entry(result: FileResult, entry):
    """The manifest entry that matches `result` (existing reasons kept)."""
    if result.file_error:
        if isinstance(entry, str) and kind_of(entry) == "xfail":
            return entry
        return f"xfail: compile: {result.file_error}"
    old = entry if isinstance(entry, dict) else {}
    new = {}
    for t in result.tests:
        prev = old.get(t.id)
        prev_kind = kind_of(prev) if prev is not None else None
        if prev_kind in NOT_RUN:
            new[t.id] = prev
        elif t.outcome == "not-run":
            if prev is not None:
                new[t.id] = prev
        elif t.outcome == "pass":
            new[t.id] = "pass"
        elif t.outcome == "skip":  # still run: it may stop skipping itself
            new[t.id] = f"selfskip: {t.detail}"
        elif prev_kind == "xfail":
            new[t.id] = prev
        else:
            new[t.id] = f"xfail: {_reason(t)}"
    for tid, prev in old.items():  # not run: skipped and excluded tests
        if tid not in new and kind_of(prev) in NOT_RUN:
            new[tid] = prev
    return new


_MANIFEST_HEADER = """\
# Expected results of CARLA's own PythonAPI tests (PythonAPI/test) against
# typesafe_carla, per mode and CARLA ref. See tests/upstream/README.md.
#
#   <mode: cpython | codon>:
#     <CARLA ref>:
#       <file>.py: "xfail: <reason>" | "skip: <reason>" | "exclude: <reason>"  # the whole file
#       <file>.py:
#         <Class>.<test_method> | <script>: pass | "xfail: <reason>" | "skip: <reason>"
#             | "selfskip: <reason>" | "exclude: <reason>"
#
# skip: not run (a hang, an interactive script). selfskip: run, expected to
# skip itself. exclude: not run and not part of the target (the ported
# originals, tests/upstream/ported).
#
# cpython: the unmodified tests under CPython, `import carla` = tools/pycarla.
# codon: the tests converted and compiled with typesafe-codon.
# official: only `exclude:` entries, for ported tests the official module
# cannot run (its own defects, with the evidence). Official runs leave them
# out; cpython and codon runs still run them.
# A test not listed is expected to pass. pytest (tests/test_upstream.py) and
# CI fail on an unexpected failure AND on an unexpected pass, so this list only
# shrinks. `python -m tools.upstream_tests --mode <mode> --suite <suite> --update`
# rewrites the current ref's entries from a run (existing reasons are kept).
"""


def file_order(name: str):
    return (SUITES.index(suite_of(name)), name)


def write_manifest(mode: str, refs: dict, path: Path = MANIFEST) -> None:
    import yaml

    data = _manifest_data(path)
    data[mode] = refs
    out = {m: {ref: {f: data[m][ref][f] for f in sorted(data[m][ref], key=file_order)}
               for ref in sorted(data[m])}
           for m in MODES + ("official",) if data.get(m)}
    text = yaml.safe_dump(out, sort_keys=False, width=1000, allow_unicode=True)
    path.write_text(_MANIFEST_HEADER + "\n" + text)


# ---------------------------------------------------------------------------
# Results and GAPS.md
# ---------------------------------------------------------------------------

def results_path(mode: str, ref: str) -> Path:
    return RESULTS / mode / f"{ref}.json"


def load_results(mode: str, ref: str) -> dict:
    p = results_path(mode, ref)
    return json.loads(p.read_text()) if p.is_file() else {"files": {}}


def record_results(mode: str, target: Target, results: list[FileResult]) -> None:
    """Merges a run's per-test outcomes into tests/upstream/results/<mode>/<ref>.json."""
    data = load_results(mode, target.ref)
    # Each file keeps the CARLA commit and backend it ran with: a later run of
    # other files must not relabel it. (Files recorded before this kept them
    # only at the top level.)
    old = {"sha": data.pop("sha", None), "backend": data.pop("backend", None)}
    for f in data["files"].values():
        for k, v in old.items():
            if v is not None:
                f.setdefault(k, v)
    data["ref"] = target.ref
    for r in results:
        data["files"][r.path] = {"sha": target.sha, "backend": target.backend, "file_error": r.file_error,
                                 "tests": {t.id: [t.outcome, t.detail] for t in r.tests}}
        traces = {t.id: t.trace for t in r.tests if t.trace}
        if traces:  # failures' last frames and full messages, for diagnosis
            data["files"][r.path]["traces"] = traces
    data["files"] = {k: data["files"][k] for k in sorted(data["files"], key=file_order)}
    p = results_path(mode, target.ref)
    p.parent.mkdir(parents=True, exist_ok=True)
    p.write_text(json.dumps(data, indent=1) + "\n")


# Root cause of a failure, from its outcome and message: (category, cause).
_CAUSES = [
    (r"^harness: (.*)", "pycarla", lambda m: f"harness: {m[1]}"),
    # Geo-projection round trips: typesafe_carla keeps doubles where LibCarla
    # rounds to float32, so large coordinates differ by up to ~0.5.
    (r"-?\d+\.\d{4,} != -?\d+\.\d{1,3} within [\d.]+ delta", "behaviour",
     lambda m: "geo round trip keeps double precision where the official module rounds to float32 "
               "(differs by up to ~0.5 at millions of metres)"),
    # The test server's content (a local package), not typesafe_carla: the
    # official module fails the same way there.
    (r"tsc_client_load_world: std::exception \[carla\.Client\.load_world\('(?:Town10HD_Opt|Town15|Mine_01|"
     r"EmptyMap|OpenDriveMap|RoadgenCross)'", "server",
     lambda m: "load_world of a shipped map exceeds the client's default 5 s timeout on this server "
               "(Town10HD_Opt takes ~8 s; the official module fails the same way)"),
    (r"tsc_client_reload_world: std::exception", "server",
     lambda m: "reload_world fails on this ue5-dev server build (OpenDRIVE parse error in its log; "
               "the official module fails the same way)"),
    (r"tsc_client_load_world: std::exception|tsc_world_get_map: std::exception", "server",
     lambda m: "map not in the server's package (load_world/get_map fail; the official module too)"),
    (r"tsc_blueprint_library_find: no blueprint with id '([\w.]+)'", "server",
     lambda m: f"blueprint `{m[1]}` not in the server's blueprint library"),
    (r"tsc_actor_blueprint_set_attribute: blueprint '([\w.]+)' has no attribute '(\w+)'", "server",
     lambda m: f"blueprint `{m[1]}` has no attribute `{m[2]}` on this server"),
    (r"error: the following arguments are required", "infrastructure",
     lambda m: "a script that needs command-line arguments (API/Tests.md documents them)"),
    (r"_queue\.Empty|queue\.Empty", "behaviour",
     lambda m: "sensor data did not arrive: callbacks run at dispatch points (World.tick, "
               "wait_for_tick, dispatch_sensor_callbacks), not on LibCarla's threads"),
    (r"unsupported operand type\(s\) for ([^:]+): '(\w+)' and '(\w+)'", "signature",
     lambda m: f"`carla.{m[2]} {m[1]} carla.{m[3]}` is not supported"),
    (r"NotImplementedError: pycarla: (\S+) not wrapped: result type Ptr\[(\w+)\]", "signature",
     lambda m: f"`carla.{m[1]}` returns a raw pointer (Ptr[{m[2]}]), not a buffer / Python object"),
    (r"NotImplementedError: pycarla: (\S+) not wrapped: (.*)", "pycarla",
     lambda m: f"pycarla cannot wrap `{m[1]}` ({m[2]})"),
    (r"ModuleNotFoundError: No module named '(\w+)'", "infrastructure",
     lambda m: f"test dependency `{m[1]}` not installed in the test interpreter"),
    (r"AttributeError: module 'carla(?:\.(\w+))?' has no attribute '(\w+)'", "missing",
     lambda m: f"missing `carla.{(m[1] + '.') if m[1] else ''}{m[2]}`"),
    (r"AttributeError: type object '(\w+)' has no attribute '(\w+)'", "missing",
     lambda m: f"missing `carla.{m[1]}.{m[2]}`"),
    (r"AttributeError: '(\w+)' object has no attribute '(\w+)'", "missing",
     lambda m: f"missing `carla.{m[1]}.{m[2]}`"),
    (r"AttributeError: property '(\w+)' of '(\w+)' object has no setter", "missing",
     lambda m: f"`carla.{m[2]}.{m[1]}` is read-only"),
    (r"TypeError: carla\.(\w+) cannot be created from Python", "missing",
     lambda m: f"`carla.{m[1]}(...)` cannot be constructed"),
    (r"TypeError: could not find callable method '(\w+?)__(\w+?)(?:__set)?' for given arguments", "signature",
     lambda m: f"`carla.{m[1]}.{'__init__' if m[2] == 'new' else m[2]}`: these arguments are not accepted"),
    (r"TypeError: could not find callable method '_f__(\w+)' for given arguments", "signature",
     lambda m: f"`carla.{m[1]}`: these arguments are not accepted"),
    (r"TypeError: (\w+?)__(\w+)\(\) got an unexpected keyword argument '(\w+)'", "signature",
     lambda m: f"`carla.{m[1]}.{'__init__' if m[2] == 'new' else m[2]}` has no keyword `{m[3]}`"),
    (r"CalledProcessError: Command '\['git'", "infrastructure",
     lambda m: "the test needs a CARLA git checkout (`git describe`)"),
    (r"AssertionError: (?P<a>.+) != (?P=a)$", "missing",
     lambda m: f"no value equality: equal-looking `{_class_of(m['a'])}` objects compare unequal (missing `__eq__`?)"),
    (r"AssertionError: '(\w+)\(.*' != '\1\(.*'$", "behaviour",
     lambda m: f"`str(carla.{m[1]})` differs from CARLA's (number formatting?)"),
    (r"RuntimeError: (.*)", "behaviour", lambda m: f"RuntimeError: {re.sub(r'[0-9]+', 'N', m[1])[:120]}"),
]


def _class_of(text: str) -> str:
    m = re.match(r"(\w+)(?:\[\d+ chars\])?", text)
    return ("carla." + m[1]) if m and "[" not in text[:len(m[0])] else text[:40]


# Upstream tests that fail with CARLA's official module too (checked against
# the module built from the same CARLA tree): not typesafe_carla gaps.
UPSTREAM_STALE = {
    "unit/test_vehicle.py::TestVehiclePhysicsControl.test_named_args":
        "uses UE4-era fields (tire_friction, radius, moi, use_gear_autobox); the official "
        "constructors ignore unknown keywords, then reading pc.wheels[i].tire_friction "
        "raises AttributeError",
}
# Tests that fail the same way with the official module on the test server
# (checked by running them with it): the server's behaviour or content.
_LOAD_TIMEOUT = ("load_world('Town10HD_Opt') exceeds the client's default 5 s timeout on this "
                 "server (it takes ~8 s; the official module fails the same way)")
SERVER_ALSO_FAILS = {
    "API/test_sync_mode.py::TestSyncMode.test_sync_mode_set_transform":
        "the prop does not move after set_transform + tick in synchronous mode; the official "
        "module fails the same way on this server",
    "API/test_spawn_vehicles.py::TestVehiclesSpawnTest.test_vehicle_spawn": _LOAD_TIMEOUT,
    "API/test_spawn_walkers.py::TestWalkersSpawn.test_walker_spawn": _LOAD_TIMEOUT,
}
# Files that load Town10HD_Opt with the default 5 s client timeout, like
# test_spawn_vehicles: a load_world failure there is the same timeout.
LOAD_TIMEOUT_FILES = {"API/test_spawn_vehicles.py", "API/test_spawn_walkers.py",
                      "API/test_sensor_recording.py", "API/test_sensor_recording_fast.py",
                      "API/test_no_rendering_mode.py"}
# Scripts that do not terminate on their own (interactive, pygame loops).
INTERACTIVE = {
    "test_raycast_sensor.py": "an interactive script (a pygame loop; the official module "
                              "also runs until the time-out)",
}

# Causes already filed as typesafe_carla issues: (pattern on the detail, issue).
KNOWN_ISSUES = [
    (r"_f__\w+' for given arguments \['B_(?:Vehicle|Walker|WalkerAIController|Actor|Sensor|TrafficLight|TrafficSign)'", 76),
    (r"unsupported operand type\(s\) for [+-]: '(?:Location|Vector3D)' and '(?:Location|Vector3D)'", 77),
    # raw_data was a method, not the buffer property CARLA's API has.
    (r"a bytes-like object is required, not 'method'", 78),
    (r"has no attribute 'ApplyVehiclePhysicsControl'", 79),
    (r"'CustomV2XMessage' object is not subscriptable", 85),
    # Sensor callbacks run only at typesafe_carla's dispatch points, so a test
    # blocking on a queue that a callback fills starves.
    (r"^_?queue\.Empty\b", 86),
    (r"-?\d+\.\d{4,} != -?\d+\.\d{1,3} within [\d.]+ delta", 81),
]


def root_cause(mode: str, outcome: str, detail: str) -> tuple[str, str]:
    """(category, cause) of a failed test, with the issue it is filed as."""
    cat, cause = _root_cause(mode, outcome, detail)
    for pattern, issue in KNOWN_ISSUES:
        if re.search(pattern, detail):
            cause += f" (#{issue})"
            break
    return cat, cause


def _root_cause(mode: str, outcome: str, detail: str) -> tuple[str, str]:
    """(category, cause) of a failed test. Categories: missing, signature,
    behaviour (typesafe_carla); server (the test server's content); pycarla,
    codon (the harness); infrastructure."""
    if outcome == "compile":
        cause = re.sub(r"^\S+:\d+: ", "", detail)
        cause = re.sub(r" \[\S+:\d+\]$", "", cause)
        return "codon", f"does not compile: {cause}"
    if outcome in ("timeout", "crash"):
        return "behaviour", outcome + (f": {detail.split(':')[0]}" if outcome == "crash" else "")
    for pattern, cat, fmt in _CAUSES:
        m = re.search(pattern, detail)
        if m:
            return cat, fmt(m)
    if outcome == "fail":
        return "behaviour", f"assertion: {detail[:140]}"
    exc = re.match(r"(?:exit -?\d+: )?(\w+(?:Error|Exception))\b", detail)
    return "behaviour", (f"{exc[1]}: {detail[len(exc[0]):].strip(': ')[:120]}" if exc
                         else f"{outcome}: {detail[:140]}")


_CATEGORY_TITLES = {
    "missing": "typesafe_carla lacks this (missing API)",
    "signature": "typesafe_carla has it, with an incompatible signature",
    "behaviour": "behaviour differs (assertions, errors, crashes)",
    "server": "the test server's content (maps, blueprints), not typesafe_carla",
    "pycarla": "the pycarla wrapper cannot express this yet (harness)",
    "codon": "Codon-direct mode: does not compile",
    "upstream": "the upstream test itself (fails with the official module too)",
    "infrastructure": "infrastructure",
}

CODON_PYEXT_LIMITS = """\
Found while building tools/pycarla with Codon 0.19 `--pyext`. They shape the
wrapper; they are not typesafe_carla gaps. Each is worked around as noted:

- Only the compiled module's own classes and functions are exported, every
  one of them (private ones, lambdas and top-level code included). Imported
  classes are not exported, so the wrapper defines one box class per library
  class.
- "Python extension types cannot be polymorphic": no class of a dynamic
  inheritance hierarchy (Vector3D/Location, Actor/Vehicle/..., ...) can be
  exported. Hence the opaque boxes, and the Python classes of
  carla/__init__.py, which restore inheritance.
- A function or method with a static parameter (`T: type`, `Literal[str]`,
  the `self: S, S: type` pattern) crashes the compiler (segfault in
  transformStaticFnWrapCallArgs) or does not compile. Such members are not
  exported (listed as pycarla stubs).
- An untyped parameter is exported as `pyobj`: its attributes can be read
  but not assigned, and `isinstance`/`hasattr` see a pyobj. The wrapper
  exports one overload per type the parameter accepts (`GENERIC_PARAMS`). It
  drops the combinations the library rejects at compile time
  (`python/typesafe_carla/pycarla/pruned.json`).
- Top-level functions do not overload (a redefinition shadows), so each
  overload gets its own name and the runtime tries them in order.
- The exporter's default for a parameter typed `NoneType` fails to unpack
  ("optional unpack failed"); `Optional[NoneType]` works.
- A call through a virtual method's dispatch thunk rejects keyword arguments,
  and with Actor's Python-API shortcuts an instance call can be ambiguous
  ("cannot typecheck"), so the wrapper calls methods positionally through
  their class.
- The virtual-method tables are filled only from what is realized while the
  module is type-checked; the exporter realizes its functions later, and a
  virtual call made only from them crashes. The wrapper realizes every
  library call in a function that never runs.
- The exporter regenerates `__to_py__`/`__from_py__` of exported classes; a
  call realized against its version returns NULL, and so does a virtual
  `__to_py__` on a polymorphic library class. The wrapper converts results
  itself (`_c_<Class>`, with an RTTI dispatch to the concrete class).
- A property setter is not exported (attributes are read-only), so setters
  are exported as functions.
- `Static[Exception]` classes reach Python as plain BaseException; the
  runtime re-raises them as RuntimeError, as CARLA's Python API does.
- `--pyext` emits an object file, to be linked with `cc -shared ... -lcodonrt`.
- The module is large: it takes about 15 minutes and 7 GB to compile.
"""


GAP_MODES = MODES + ("official",)


def _cause(m: str, rel: str, tid: str, o: str, detail: str, official: dict) -> tuple[str, str]:
    """The root cause of a failure in mode `m`, given the official module's results.

    The static rules (stale upstream, server limits) and "fails with the
    official module too" apply only where the official module fails the same
    way, or has no result. A typesafe failure where official passes, or fails
    differently (another exception), keeps typesafe's own cause, with the
    official failure noted.
    """
    key = f"{rel}::{tid}"
    off = official.get(rel, {}).get("tests", {}).get(tid) if m != "official" else None
    if o == "compile":  # Codon-direct: the harness's limit, whatever official does
        return root_cause(m, o, detail)
    if off and off[0] not in FAILURES:
        return root_cause(m, o, detail)  # official passes (or skips): typesafe's own
    if off and not _same_failure(o, detail, off[0], off[1]):
        cat, cause = root_cause(m, o, detail)
        _, off_cause = root_cause("official", off[0], off[1])
        return cat, f"{cause} (official also fails, differently: {off_cause})"
    if key in UPSTREAM_STALE:
        return "upstream", f"stale upstream test: {UPSTREAM_STALE[key]}"
    if key in SERVER_ALSO_FAILS:
        return "server", SERVER_ALSO_FAILS[key]
    if rel in LOAD_TIMEOUT_FILES and "load_world" in detail:
        return "server", _LOAD_TIMEOUT
    if rel in INTERACTIVE:
        return "infrastructure", INTERACTIVE[rel]
    if off:
        cat, cause = root_cause("official", off[0], off[1])
        return ("server" if cat == "server" else "upstream"), f"fails with the official module too: {cause}"
    return root_cause(m, o, detail)


# typesafe_carla's Codon exceptions, as the Python API names them.
_PYTHON_EXCEPTION = {"CarlaError": "RuntimeError", "TimeoutError": "RuntimeError"}


def _failure_kind(outcome: str, detail: str) -> str:
    """What a failure is, for comparing two modes: the outcome, and for an
    error its exception class ('error ?' when the message names none)."""
    if outcome != "error":
        return outcome
    exc = re.search(r"\b(\w+(?:Error|Exception|Exit)|_?queue\.Empty)\b", detail)
    return f"error {_PYTHON_EXCEPTION.get(exc[1], exc[1]) if exc else '?'}"


def _same_failure(o: str, detail: str, off_o: str, off_detail: str) -> bool:
    mine, theirs = _failure_kind(o, detail), _failure_kind(off_o, off_detail)
    return mine == theirs or "error ?" in (mine, theirs) and mine.startswith("error") \
        and theirs.startswith("error")


_GAPS_INTRO = """\
# typesafe_carla vs CARLA's own PythonAPI tests: gaps

Generated by `python -m tools.upstream_tests --gaps` from
`tests/upstream/results/`; do not edit. See tests/upstream/README.md.

Modes: **cpython**, the unmodified tests under CPython with `import carla` =
typesafe_carla through tools/pycarla (primary); **codon**, the tests compiled
with typesafe-codon; **official**, CARLA's own module on the same server, for
comparison. A failure the official module shares is not a typesafe_carla gap.
*not run*: no result recorded (e.g. needs a server). Suite `ported`: tests
ported to maps shipped in ue5-dev (tests/upstream/ported, issue #80).
"""

_GAPS_INFRASTRUCTURE = """\
## Infrastructure limits

- CI has no CARLA server: it runs only `unit`; `smoke`, `API`, the top-level
  files and `ported` need `TSC_CARLA_PORT` (results here come from local runs).
- No ue5-dev package ships Town03, Town05(_Opt), Town01, Town11 or Town12: the
  tests needing them are excluded and ported (issue #80).
"""


def _drop_excluded(data: dict) -> None:
    """Drops excluded tests (and results recorded before their exclusion): they do not count."""
    for (m, r), d in data.items():
        manifest = run_manifest(m).get(r) or {}
        for rel in list(d["files"]):
            entry = manifest.get(rel)
            if not_run(entry, ("exclude",)):
                del d["files"][rel]
            elif isinstance(entry, dict):
                tests = d["files"][rel]["tests"]
                for tid in skipped_ids(entry, ("exclude",)) & tests.keys():
                    del tests[tid]


def _gaps_summary(refs: list[str], data: dict) -> list[str]:
    lines = ["", "## Summary", "", "| ref | mode | suite | commit | pass | fail | skip | not run |",
             "|---|---|---|---|---:|---:|---:|---:|"]
    for r in refs:
        for m in GAP_MODES:
            d = data[(m, r)]
            for suite in SUITES:
                files = [f for rel, f in d["files"].items() if suite_of(rel) == suite]
                outs = [o for f in files for o, _ in f["tests"].values()]
                shas = ", ".join(sorted({(f.get("sha") or d.get("sha") or "?")[:10] for f in files}))
                if outs:
                    lines.append(f"| {r} | {m} | {suite} | {shas} | {outs.count('pass')} | "
                                 f"{sum(o in FAILURES for o in outs)} | {outs.count('skip')} | "
                                 f"{outs.count('not-run')} |")
    return lines


def _gaps_root_causes(refs: list[str], data: dict) -> list[str]:
    """The failures of each ref and mode, grouped by root cause (each a candidate issue)."""
    lines = []
    for r in refs:
        official = data[("official", r)]["files"]
        for m in GAP_MODES:
            groups: dict[tuple[str, str], list[str]] = {}
            for rel, f in data[(m, r)]["files"].items():
                for tid, (o, detail) in f["tests"].items():
                    if o in FAILURES:
                        groups.setdefault(_cause(m, rel, tid, o, detail, official), []).append(f"{rel}::{tid}")
            if not groups:
                continue
            lines += ["", f"## Root causes: {r}, {m} mode", ""]
            for cat in _CATEGORY_TITLES:
                items = sorted(((cause, tests) for (c, cause), tests in groups.items() if c == cat),
                               key=lambda x: (-len(x[1]), x[0]))
                if not items:
                    continue
                lines += [f"### {_CATEGORY_TITLES[cat]} ({len(items)} causes, "
                          f"{sum(len(t) for _, t in items)} tests)", ""]
                for cause, tests in items:
                    shown = ", ".join(f"`{t}`" for t in tests[:8]) + (f", +{len(tests) - 8} more" if len(tests) > 8 else "")
                    lines.append(f"- **{cause}**: {len(tests)} tests: {shown}")
                lines.append("")
    return lines


def _gaps_table(title: str, intro: list[str], rows: list[tuple[str, str, str, str]]) -> list[str]:
    if not rows:
        return []
    return (["", f"## {title}", "", *intro, "", "| ref | file | test | why |", "|---|---|---|---|"]
            + [f"| {r} | `{rel}` | {t} | {why} |" for r, rel, t, why in rows])


def _gaps_not_run(refs: list[str]) -> list[str]:
    """The tests expectations.yaml excludes, and those the official module cannot run."""
    excluded = []
    for r in refs:
        for rel, entry in (load_manifest("cpython").get(r) or {}).items():
            if not_run(entry, ("exclude",)):
                excluded.append((r, rel, "(all its tests)", parse_expectation(entry)[1]))
            elif isinstance(entry, dict):
                excluded += [(r, rel, t, parse_expectation(v)[1]) for t, v in entry.items()
                             if kind_of(v) == "exclude"]
    unmeasured = [(r, rel, t, parse_expectation(v)[1])
                  for r in refs for rel, tests in (load_manifest("official").get(r) or {}).items()
                  for t, v in tests.items()]
    return (_gaps_table("Excluded from the target",
                        ["Not run, and not counted as failures (expectations.yaml `exclude:`)."], excluded)
            + _gaps_table("Not run with the official module",
                          ["The official module cannot run these (its own defects or this server's",
                           "behaviour; expectations.yaml `official:`), so typesafe_carla is not compared",
                           "with it here. typesafe_carla still runs them: a pass is a pass, a failure",
                           "has no official baseline."], unmeasured))


def _gaps_every_test(refs: list[str], data: dict) -> list[str]:
    lines = []
    for r in refs:
        lines += [f"## Every test: {r}", "", "| test | cpython | codon | official | root cause |",
                  "|---|---|---|---|---|"]
        official = data[("official", r)]["files"]
        files = sorted(set().union(*(data[(m, r)]["files"] for m in GAP_MODES)), key=file_order)
        for rel in files:
            results = {m: data[(m, r)]["files"].get(rel, {}).get("tests", {}) for m in GAP_MODES}
            for tid in dict.fromkeys(t for m in GAP_MODES for t in results[m]):
                cells, cause = [], ""
                for m in GAP_MODES:
                    o, detail = results[m].get(tid, ["not run", ""])
                    cells.append(o)
                    if not cause and o in FAILURES:
                        cause = _cause(m, rel, tid, o, detail, official)[1]
                lines.append(f"| `{rel}::{tid}` | {' | '.join(cells)} | {cause.replace('|', '/')} |")
        lines.append("")
    return lines


def write_gaps(primary: str = "ue5-dev") -> Path:
    """GAPS.md from tests/upstream/results: every test, its outcome per mode,
    and the failures grouped by root cause (each group a candidate issue)."""
    refs = sorted({p.stem for m in GAP_MODES for p in (RESULTS / m).glob("*.json")},
                  key=lambda r: (r != primary, r))
    data = {(m, r): load_results(m, r) for m in GAP_MODES for r in refs}
    _drop_excluded(data)
    lines = [*_GAPS_INTRO.splitlines(), *_gaps_summary(refs, data), *_gaps_root_causes(refs, data),
             *_gaps_not_run(refs),
             "", "## Codon `--pyext` limitations (harness, not typesafe_carla gaps)", "", CODON_PYEXT_LIMITS,
             *_GAPS_INFRASTRUCTURE.splitlines(), "", *_gaps_every_test(refs, data)]
    GAPS.write_text("\n".join(lines) + "\n")
    return GAPS


# ---------------------------------------------------------------------------
# Command line (CI)
# ---------------------------------------------------------------------------

def _report_only(rows: list[tuple[str, Check]], known: set[str], ref: str) -> str:
    unknown = sorted({suite_of(rel) for rel, _ in rows} - known, key=SUITES.index)
    return f" (no expectations for {ref} in {', '.join(unknown)}: report only)" if unknown else ""


def _summary(mode: str, target: Target, rows: list[tuple[str, Check]], known: set[str]) -> str:
    total = Check(sum(c.passed for _, c in rows), sum(c.xfailed for _, c in rows),
                  sum(c.skipped for _, c in rows), sum(c.excluded for _, c in rows),
                  problems=[p for _, c in rows for p in c.problems])
    lines = [f"### CARLA PythonAPI tests vs typesafe_carla ({mode} mode): "
             f"{target.ref} @ {target.sha[:12]} ({target.backend})",
             "", f"**{total.counts()}**" + _report_only(rows, known, target.ref),
             "", "| file | pass | xfail | skip | excluded | unexpected |", "|---|---:|---:|---:|---:|---:|"]
    for rel, c in rows:
        lines.append(f"| {rel} | {c.passed} | {c.xfailed} | {c.skipped} | {c.excluded} | {len(c.problems)} |")
    if total.problems:
        lines += ["", "Unexpected:", ""] + [f"- {p}" for p in total.problems]
    return "\n".join(lines) + "\n"


def run_mode(mode: str, target: Target, tests: Path, files: list[str], args) -> tuple[list, set[str]]:
    """Runs `files` in one mode; returns (rows, the suites with expectations)."""
    # Official mode compares with CARLA's own module: no expectations, but the
    # cpython ones decide what is excluded.
    manifest = run_manifest(mode)
    known = known_suites(manifest, target.ref) if mode != "official" else set()
    work = cache_root() / target.sha / "build"
    pydir = pycarla_dir() if mode == "cpython" else None

    def one(rel: str):
        entry = file_expectations(manifest, target.ref, rel)
        ids = discover(tests, rel) or [SCRIPT]
        if not_run(entry):
            return rel, None, entry, ids
        server = suite_of(rel) in SERVER_SUITES
        if mode in ("cpython", "official"):
            # Official mode leaves out what the typesafe modes leave out
            # (skip: a hang or an interactive script, exclude:).
            skip = skipped_ids(entry, NOT_RUN)
            return rel, run_file_cpython(tests, rel, pydir, skip, server), entry, ids
        return rel, run_file(tests, rel, work, skipped_ids(entry), server,
                             compile_only=args.compile_only), entry, ids

    if any(suite_of(f) in SERVER_SUITES for f in files) and not args.compile_only:
        # Fail fast on an unreachable server (and start from a clean one).
        if mode == "codon":
            _server_cleanup(tests, None, _codon_cleanup(work))
        else:
            _server_cleanup(tests, _cpython_env(pydir, True))
    rows, results = [], []
    # Server tests share one server: one file at a time.
    jobs = 1 if any(suite_of(f) in SERVER_SUITES for f in files) and not args.compile_only else args.jobs
    with concurrent.futures.ThreadPoolExecutor(max_workers=max(1, jobs)) as pool:
        for rel, result, entry, ids in pool.map(one, files):
            # Official mode has no expectations of its own, but files the
            # cpython manifest does not run (result None) keep their entry.
            c = check(result, entry if mode != "official" or result is None else None, ids)
            rows.append((rel, c))
            print(f"[{mode}] {rel}: {c.counts()}", flush=True)
            if result and (args.verbose or c.problems):
                if result.file_error:
                    print(f"  (file does not compile) {result.file_error}")
                for t in result.tests:
                    print(f"  {t.id}: {t.outcome}{': ' + t.detail if t.detail else ''}")
                    if t.trace and args.verbose:
                        print("    | " + t.trace.replace("\n", "\n    | "))
            for p in c.problems:
                print(f"  UNEXPECTED {p}")
            if result is not None:
                results.append(result)
                if args.update and mode != "official":
                    manifest.setdefault(target.ref, {})[rel] = updated_entry(result, entry)
    if args.update:
        if mode != "official":
            write_manifest(mode, manifest)
        record_results(mode, target, results)
        print(f"updated {MANIFEST.relative_to(ROOT)} and {results_path(mode, target.ref).relative_to(ROOT)}")
    return rows, known


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(prog="python -m tools.upstream_tests", description=__doc__.split("\n\n")[0])
    parser.add_argument("--mode", choices=MODES + ("all", "official"), default="cpython",
                        help="cpython: unmodified tests, carla = tools/pycarla (default); "
                             "codon: converted and compiled with typesafe-codon; all: both; "
                             "official: CARLA's own module (TSC_UPSTREAM_PYTHON's), for comparison")
    parser.add_argument("--suite", action="append", choices=SUITES,
                        help="suite(s) to run (default: unit, plus smoke, API and top when TSC_CARLA_PORT is set)")
    parser.add_argument("--file", action="append", help="only these files (relative to PythonAPI/test)")
    parser.add_argument("--summary", help="append a Markdown summary to this file ($GITHUB_STEP_SUMMARY)")
    parser.add_argument("--update", action="store_true",
                        help="rewrite this ref's expectations and recorded results from the run, "
                             "and regenerate GAPS.md")
    parser.add_argument("--gaps", action="store_true", help="only regenerate GAPS.md from the recorded results")
    parser.add_argument("--compile-only", action="store_true",
                        help="codon mode: only compile (smoke/API/top then need no server)")
    parser.add_argument("-j", "--jobs", type=int, default=min(4, os.cpu_count() or 1))
    parser.add_argument("-v", "--verbose", action="store_true", help="print every test's outcome")
    args = parser.parse_args(argv)
    if args.gaps:
        print(write_gaps())
        return 0
    modes = list(MODES) if args.mode == "all" else [args.mode]
    suites = args.suite or (["unit"] + (list(SERVER_SUITES) if os.environ.get("TSC_CARLA_PORT") else []))
    if (any(s in SERVER_SUITES for s in suites) and not os.environ.get("TSC_CARLA_PORT")
            and not (args.compile_only and modes == ["codon"])):
        parser.error("smoke, API, top and ported need a CARLA server: set TSC_CARLA_PORT (and TSC_CARLA_HOST)")
    if "official" in modes and not os.environ.get("TSC_UPSTREAM_PYTHON"):
        parser.error("official mode needs TSC_UPSTREAM_PYTHON: an interpreter with CARLA's carla module")

    target = resolve_target()
    tests = fetch_tests(target.sha)
    ported = {suite_of(rel) for m in MODES for rel in load_manifest(m).get(target.ref) or {}}
    if "ported" in suites and "ported" not in ported:
        print(f"ported: skipped, no ported expectations for {target.ref} (the ports target ue5-dev's maps)")
        suites = [s for s in suites if s != "ported"]
    files = [rel for s in suites for rel in test_files(tests, s)
             if not args.file or rel in args.file]
    print(f"CARLA {target.ref} @ {target.sha} ({target.backend}): {len(files)} files", flush=True)
    failed = False
    for mode in modes:
        rows, known = run_mode(mode, target, tests, files, args)
        text = _summary(mode, target, rows, known)
        if args.summary:
            with open(args.summary, "a") as f:
                f.write(text)
        failed |= not args.update and not args.compile_only and any(
            c.problems for rel, c in rows if suite_of(rel) in known)
    if args.update:
        print(write_gaps())
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
