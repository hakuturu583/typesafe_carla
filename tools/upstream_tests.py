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

ROOT = Path(__file__).resolve().parent.parent
UPSTREAM_DIR = ROOT / "tests" / "upstream"
MANIFEST = UPSTREAM_DIR / "expectations.yaml"
SHIM = UPSTREAM_DIR / "tsc_unittest.codon"
REPOSITORY = "https://github.com/carla-simulator/carla"
SUITES = ("unit", "smoke", "API", "top")   # top: the files directly in PythonAPI/test
SERVER_SUITES = ("smoke", "API", "top")
MODES = ("cpython", "codon")
RESULTS = UPSTREAM_DIR / "results"
GAPS = UPSTREAM_DIR / "GAPS.md"
DEFAULT_MOCK_REF = "ue5-dev"  # the mock mirrors LibCarla ue5-dev
REASON_MAX = 300
SCRIPT = "<script>"  # the one "test" of a file without TestCase classes

ANSI = re.compile(r"\x1b\[[0-9;]*m")
# `file.codon:12 (5-20): error: message`, possibly after a tree prefix (├─ ╰─).
ERROR_LINE = re.compile(r"^[\s│├╰─]*(?P<file>[^\s:]+):(?P<line>\d+)(?: \([0-9-]+\))?: error: (?P<msg>.*)$")
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
    to PythonAPI/test."""
    d = tests if suite == "top" else tests / suite
    return sorted((p.name if suite == "top" else f"{suite}/{p.name}")
                  for p in d.glob("*.py") if p.name != "__init__.py")


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
    path = tests / rel
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


@dataclass
class FileResult:
    path: str                      # e.g. unit/test_transform.py
    tests: list[TestResult] = field(default_factory=list)
    file_error: str | None = None  # the module does not compile at all
    log: str = ""


def _launcher(*args: str, timeout: float, env: dict[str, str] | None = None):
    return subprocess.run([sys.executable, "-m", "typesafe_carla.cli", *args],
                          capture_output=True, text=True, timeout=timeout,
                          env={**os.environ, **(env or {})})


def _error_chains(stderr: str) -> list[list[tuple[str, int, str]]]:
    """Compiler errors as chains: the error, then its `during the realization` lines."""
    chains: list[list[tuple[str, int, str]]] = []
    for raw in ANSI.sub("", stderr).splitlines():
        m = ERROR_LINE.match(raw)
        if not m:
            continue
        entry = (m["file"], int(m["line"]), m["msg"].strip())
        nested = raw.lstrip()[:1] in ("├", "╰", "│")
        if nested and chains:
            chains[-1].append(entry)
        else:
            chains.append([entry])
    return chains


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


def run_file(tests: Path, rel: str, work: Path, skip: set[str] = frozenset(),
             server: bool = False, timeout: float | None = None,
             compile_only: bool = False) -> FileResult:
    """Converts, compiles and runs one upstream file; `skip` ids are left out.

    With `compile_only`, the tests that compile are not run (outcome not-run).
    """
    result = FileResult(rel)
    ids = [t for t in discover(tests, rel) if t not in skip]
    script = not discover(tests, rel)
    out_dir = work / Path(rel).parent
    out_dir.mkdir(parents=True, exist_ok=True)
    shutil.copy(SHIM, out_dir / SHIM.name)
    module, offset = convert_source((tests / rel).read_text(), tests / rel, server)
    module_end = module.count("\n")
    source = out_dir / (Path(rel).stem + ".codon")
    modules = {source.name: (rel, offset, module_end)}
    init = tests / Path(rel).parent / "__init__.py"
    if init.is_file():
        text, init_offset = convert_source(init.read_text(), init, server)
        (out_dir / "__init__.codon").write_text(text)
        modules["__init__.codon"] = (f"{Path(rel).parent}/__init__.py", init_offset, text.count("\n"))
    exe = out_dir / Path(rel).stem
    if script:
        return _run_script_codon(result, module, modules, source, exe, out_dir, server,
                                 timeout, compile_only)
    compile_errors: dict[str, str] = {}
    included = list(ids)
    logs = []
    while True:
        runner, ranges = runner_source(included, module_end + 1)
        source.write_text(module + runner)
        build = _launcher("build", "-o", str(exe), str(source), timeout=900)
        logs.append(ANSI.sub("", build.stderr))
        if build.returncode == 0:
            break
        chains = _error_chains(build.stderr)
        culprits: dict[str, str] = {}
        file_error = None
        for chain in chains:
            primary = _located(chain, modules)
            hit = [t for t in included
                   if any(f == source.name and (ranges[t][0] <= ln <= ranges[t][1]
                                                or ranges[t][2] <= ln <= ranges[t][3])
                          for f, ln, _ in chain)]
            if not hit:
                file_error = file_error or primary
            for t in hit:
                culprits.setdefault(t, primary)
        if file_error or not culprits:
            result.file_error = _shorten(file_error or (ANSI.sub("", build.stderr).strip().splitlines() or ["compilation failed"])[-1])
            result.tests = [TestResult(t, "compile", result.file_error) for t in ids]
            result.log = "\n".join(logs)
            return result
        compile_errors.update(culprits)
        included = [t for t in included if t not in culprits]
    result.log = "\n".join(logs)
    timeout = timeout or (900 if server else 120)
    for test_id in ids:
        if test_id in compile_errors:
            result.tests.append(TestResult(test_id, "compile", _shorten(compile_errors[test_id])))
            continue
        if compile_only:
            result.tests.append(TestResult(test_id, "not-run"))
            continue
        result.tests.append(_run_one(exe, test_id, timeout, out_dir))
    return result


def _run_script_codon(result: FileResult, module: str, modules: dict, source: Path, exe: Path,
                      cwd: Path, server: bool, timeout: float | None,
                      compile_only: bool) -> FileResult:
    """A file without TestCase classes is a script: compiled and run whole."""
    source.write_text(module)
    build = _launcher("build", "-o", str(exe), str(source), timeout=900)
    result.log = ANSI.sub("", build.stderr)
    if build.returncode != 0:
        chains = _error_chains(build.stderr)
        result.file_error = _shorten(_located(chains[0], modules) if chains
                                     else (result.log.strip().splitlines() or ["compilation failed"])[-1])
        result.tests = [TestResult(SCRIPT, "compile", result.file_error)]
        return result
    if compile_only:
        result.tests = [TestResult(SCRIPT, "not-run")]
        return result
    timeout = timeout or (900 if server else 120)
    try:
        proc = subprocess.run([str(exe)], capture_output=True, text=True, timeout=timeout,
                              cwd=cwd, errors="replace")
    except subprocess.TimeoutExpired:
        result.tests = [TestResult(SCRIPT, "timeout", f"timed out after {timeout:g} s")]
        return result
    first = next((ln for ln in ANSI.sub("", proc.stderr).splitlines() if ln.strip()), "")
    outcome = "pass" if proc.returncode == 0 else ("crash" if proc.returncode < 0 else "error")
    result.tests = [TestResult(SCRIPT, outcome, "" if outcome == "pass"
                               else _shorten(f"exit {proc.returncode}: {first}"))]
    return result


def _run_one(exe: Path, test_id: str, timeout: float, cwd: Path) -> TestResult:
    try:
        proc = subprocess.run([str(exe), test_id], capture_output=True, text=True,
                              timeout=timeout, cwd=cwd, errors="replace")
    except subprocess.TimeoutExpired:
        return TestResult(test_id, "timeout", f"timed out after {timeout:g} s")
    lines = [m for m in map(RESULT_LINE.match, proc.stdout.splitlines()) if m]
    # Codon prints an uncaught error first, then a backtrace.
    first = next((ln for ln in ANSI.sub("", proc.stderr).splitlines() if ln.strip()), "")
    if proc.returncode < 0:
        sig = signal.Signals(-proc.returncode).name
        return TestResult(test_id, "crash", _shorten(f"{sig} {first}"))
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
# The tests must see the generated package, never an installed official one
# (an editable install's import hook would win over sys.path): load it first.
try:
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
        if re.match(r"^[A-Za-z_][\w.]*(Error|Exception|Exit|Interrupt|Warning)\b", lines[i]):
            return lines[i]
    return lines[-1] if lines else ""
if result.errors:
    tb = result.errors[0][1]
    out = {"status": "error",
           "detail": ("in tearDown: " if "in tearDown" in tb.split("Traceback")[-1] else "") + last(tb)}
elif result.failures:
    out = {"status": "fail", "detail": last(result.failures[0][1])}
elif result.skipped:
    out = {"status": "skip", "detail": result.skipped[0][1]}
elif result.testsRun == 0:
    out = {"status": "error", "detail": "no test ran"}
else:
    out = {"status": "pass", "detail": ""}
print("TSC-RESULT " + json.dumps(out), flush=True)
"""


# Run after each server test, with the generated package: a test that fails
# before its own clean-up (or whose tearDown fails, e.g. on a missing map)
# must not leave actors or synchronous mode behind for the next one.
_CLEANUP = r"""
import importlib.util, os, sys
pkg = os.environ["TSC_PYCARLA_PKG"]
spec = importlib.util.spec_from_file_location("carla", os.path.join(pkg, "__init__.py"),
                                              submodule_search_locations=[pkg])
carla = importlib.util.module_from_spec(spec); sys.modules["carla"] = carla; spec.loader.exec_module(carla)
client = carla.Client("127.0.0.1", 2000)  # redirected to the test server
client.set_timeout(30.0)
world = client.get_world()
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


def _server_cleanup(tests: Path, env: dict[str, str]) -> None:
    try:
        subprocess.run([_python(), "-c", _CLEANUP], cwd=tests, env=env, capture_output=True,
                       text=True, timeout=120)
    except subprocess.TimeoutExpired:
        pass


def pycarla_dir(build: bool = True) -> Path | None:
    """The generated `carla` package (tools/pycarla), built if missing or stale
    (unless `build` is False: then None)."""
    from tools import pycarla

    explicit = os.environ.get("TSC_PYCARLA_DIR")
    out = Path(explicit).resolve() if explicit else pycarla.default_out()
    so = out / "carla" / f"{pycarla.MODULE}.so"
    sources = [*pycarla.PACKAGE.glob("*.codon"), Path(pycarla.__file__), pycarla.RUNTIME]
    if not so.is_file() or so.stat().st_mtime < max(p.stat().st_mtime for p in sources):
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


def _cpython_env(pydir: Path, server: bool) -> dict[str, str]:
    from typesafe_carla import paths

    env = {**os.environ, "TSC_PYCARLA_PKG": str(pydir / "carla"),
           # the library this run is about (the package may have been built against another)
           "TYPESAFE_CARLA_LIB": os.environ.get("TYPESAFE_CARLA_LIB") or str(paths.native_library()),
           "PYTHONPATH": os.pathsep.join(
        [str(pydir)] + ([os.environ["PYTHONPATH"]] if os.environ.get("PYTHONPATH") else []))}
    env.pop("PYTHONHOME", None)
    if server:
        env["TSC_PYCARLA_REDIRECT"] = (f"{os.environ.get('TSC_CARLA_HOST', '127.0.0.1')}:"
                                       f"{os.environ.get('TSC_CARLA_PORT', '2000')}")
    return env


def run_file_cpython(tests: Path, rel: str, pydir: Path, skip: set[str] = frozenset(),
                     server: bool = False, timeout: float | None = None) -> FileResult:
    """Runs one upstream file unmodified under CPython, each test in its own process.

    A unittest module runs test by test; a script runs whole (`<script>`).
    """
    result = FileResult(rel)
    env = _cpython_env(pydir, server)
    timeout = timeout or (900 if server else 120)
    if rel in INTERACTIVE:
        result.tests = [TestResult(SCRIPT, "skip", INTERACTIVE[rel])]
        return result
    ids = discover(tests, rel)
    driver = cache_root() / "tsc_unittest_driver.py"
    driver.parent.mkdir(parents=True, exist_ok=True)
    if not driver.is_file() or driver.read_text() != _DRIVER:
        driver.write_text(_DRIVER)
    env = {**env, "PYTHONPATH": env["PYTHONPATH"] + os.pathsep + str(tests)}
    if not ids:
        result.tests = [_run_script_cpython(tests, rel, env, timeout, driver)]
        if server:
            _server_cleanup(tests, env)
        return result
    module = rel[:-3].replace("/", ".")
    for test_id in ids:
        if test_id in skip:
            continue
        try:
            proc = subprocess.run([_python(), str(driver), f"{module}.{test_id}"], cwd=tests, env=env,
                                  capture_output=True, text=True, timeout=timeout, errors="replace")
        except subprocess.TimeoutExpired:
            result.tests.append(TestResult(test_id, "timeout", f"timed out after {timeout:g} s"))
            if server:
                _server_cleanup(tests, env)
            continue
        line = next((ln for ln in reversed(proc.stdout.splitlines()) if ln.startswith("TSC-RESULT ")), None)
        if proc.returncode < 0:
            sig = signal.Signals(-proc.returncode).name
            first = next((ln for ln in proc.stderr.splitlines() if ln.strip()), "")
            result.tests.append(TestResult(test_id, "crash", _shorten(f"{sig} {first}")))
        elif line is None:
            tail = (proc.stderr.strip().splitlines() or [""])[-1]
            result.tests.append(TestResult(test_id, "error", _shorten(f"exit {proc.returncode}: {tail}")))
        else:
            out = json.loads(line[len("TSC-RESULT "):])
            result.tests.append(TestResult(test_id, out["status"], _shorten(out["detail"])))
        if server:
            _server_cleanup(tests, env)
    return result


def _run_script_cpython(tests: Path, rel: str, env: dict[str, str], timeout: float,
                        driver: Path) -> TestResult:
    try:
        proc = subprocess.run([_python(), str(driver), "--script", rel], cwd=tests, env=env,
                              capture_output=True, text=True, timeout=timeout, errors="replace")
    except subprocess.TimeoutExpired:
        return TestResult(SCRIPT, "timeout", f"timed out after {timeout:g} s")
    line = next((ln for ln in reversed(proc.stdout.splitlines()) if ln.startswith("TSC-RESULT ")), None)
    if line is not None:  # the harness check failed
        out = json.loads(line[len("TSC-RESULT "):])
        return TestResult(SCRIPT, out["status"], _shorten(out["detail"]))
    if proc.returncode == 0:
        return TestResult(SCRIPT, "pass")
    if proc.returncode < 0:
        return TestResult(SCRIPT, "crash", signal.Signals(-proc.returncode).name)
    lines = [ln for ln in proc.stderr.strip().splitlines() if ln.strip()]
    detail = next((ln for ln in reversed(lines) if not ln.startswith(" ")), lines[-1] if lines else "")
    return TestResult(SCRIPT, "error", _shorten(f"exit {proc.returncode}: {detail}"))


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


def parse_expectation(value) -> tuple[str, str]:
    """('pass' | 'xfail' | 'skip', reason)."""
    text = str(value).strip()
    kind, _, reason = text.partition(":")
    kind = kind.strip()
    if kind not in ("pass", "xfail", "skip"):
        raise ValueError(f"bad expectation {value!r}: use pass, 'xfail: <reason>' or 'skip: <reason>'")
    return kind, reason.strip()


def file_expectations(manifest: dict, ref: str, rel: str):
    """A file's entry for `ref`: None, a file-level string, or {test id: value}."""
    return (manifest.get(ref) or {}).get(rel)


def skipped_ids(entry) -> set[str]:
    if isinstance(entry, dict):
        return {t for t, v in entry.items() if parse_expectation(v)[0] == "skip"}
    return set()


FAILURES = ("fail", "error", "crash", "timeout", "compile")


@dataclass
class Check:
    passed: int = 0
    xfailed: int = 0
    skipped: int = 0
    problems: list[str] = field(default_factory=list)

    def counts(self) -> str:
        text = f"{self.passed} pass, {self.xfailed} xfail, {self.skipped} skip"
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
        if kind == "xfail":
            if result.file_error:
                c.xfailed = len(result.tests)
            else:
                c.problems.append(f"{result.path}: compiles now (expected xfail: {reason}); "
                                  "list its tests in the manifest")
            return c
    entry = entry if isinstance(entry, dict) else {}
    for t in result.tests:
        kind, reason = parse_expectation(entry.get(t.id, "pass"))
        if kind == "skip":
            c.skipped += 1
        elif t.outcome == "not-run":
            pass
        elif t.outcome == "skip":
            c.problems.append(f"{t.id}: skipped itself ({t.detail}); record it as skip")
        elif kind == "pass" and t.outcome == "pass":
            c.passed += 1
        elif kind == "xfail" and t.outcome in FAILURES:
            c.xfailed += 1
        elif kind == "xfail":
            c.problems.append(f"{t.id}: unexpected pass (expected xfail: {reason}); mark it pass")
        else:
            c.problems.append(f"{t.id}: unexpected {t.outcome}: {t.detail}")
    for t in entry:
        if t not in {r.id for r in result.tests} and parse_expectation(entry[t])[0] != "skip":
            c.problems.append(f"{t}: in the manifest but not in the upstream file")
    return c


def _reason(t: TestResult) -> str:
    return _shorten(f"{t.outcome}: {t.detail}" if t.detail else t.outcome)


def updated_entry(result: FileResult, entry):
    """The manifest entry that matches `result` (existing reasons kept)."""
    if result.file_error:
        if isinstance(entry, str) and parse_expectation(entry)[0] == "xfail":
            return entry
        return f"xfail: compile: {result.file_error}"
    old = entry if isinstance(entry, dict) else {}
    new = {}
    for t in result.tests:
        prev = old.get(t.id)
        prev_kind = parse_expectation(prev)[0] if prev is not None else None
        if prev_kind == "skip":
            new[t.id] = prev
        elif t.outcome == "not-run":
            if prev is not None:
                new[t.id] = prev
        elif t.outcome == "pass":
            new[t.id] = "pass"
        elif t.outcome == "skip":
            new[t.id] = f"skip: {t.detail}"
        elif prev_kind == "xfail":
            new[t.id] = prev
        else:
            new[t.id] = f"xfail: {_reason(t)}"
    return new


_MANIFEST_HEADER = """\
# Expected results of CARLA's own PythonAPI tests (PythonAPI/test) against
# typesafe_carla, per mode and CARLA ref. See tests/upstream/README.md.
#
#   <mode: cpython | codon>:
#     <CARLA ref>:
#       <file>.py: "xfail: <reason>" | "skip: <reason>"     # the whole file
#       <file>.py:
#         <Class>.<test_method> | <script>: pass | "xfail: <reason>" | "skip: <reason>"
#
# cpython: the unmodified tests under CPython, `import carla` = tools/pycarla.
# codon: the tests converted and compiled with typesafe-codon.
# A test not listed is expected to pass. pytest (tests/test_upstream.py) and
# CI fail on an unexpected failure AND on an unexpected pass, so this list only
# shrinks. `python -m tools.upstream_tests --mode <mode> --suite <suite> --update`
# rewrites the current ref's entries from a run (existing reasons are kept).
"""


def _file_order(name: str):
    return (SUITES.index(suite_of(name)), name)


def write_manifest(mode: str, refs: dict, path: Path = MANIFEST) -> None:
    import yaml

    data = _manifest_data(path)
    data[mode] = refs
    out = {m: {ref: {f: data[m][ref][f] for f in sorted(data[m][ref], key=_file_order)}
               for ref in sorted(data[m])}
           for m in MODES if data.get(m)}
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
    data.update({"ref": target.ref, "sha": target.sha, "backend": target.backend})
    for r in results:
        data["files"][r.path] = {"file_error": r.file_error,
                                 "tests": {t.id: [t.outcome, t.detail] for t in r.tests}}
    data["files"] = {k: data["files"][k] for k in sorted(data["files"], key=_file_order)}
    p = results_path(mode, target.ref)
    p.parent.mkdir(parents=True, exist_ok=True)
    p.write_text(json.dumps(data, indent=1) + "\n")


# Root cause of a failure, from its outcome and message: (category, cause).
_CAUSES = [
    (r"^harness: (.*)", "pycarla", lambda m: f"harness: {m[1]}"),
    # The test server's content (a local package), not typesafe_carla: the
    # official module fails the same way there.
    (r"tsc_client_(?:load|reload)_world: std::exception|tsc_world_get_map: std::exception", "server",
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
SERVER_ALSO_FAILS = {
    "API/test_sync_mode.py::TestSyncMode.test_sync_mode_set_transform":
        "the prop does not move after set_transform + tick in synchronous mode; the official "
        "module fails the same way on this server",
}
# Scripts that do not terminate on their own (interactive, pygame loops).
INTERACTIVE = {
    "test_raycast_sensor.py": "an interactive script (a pygame loop; the official module "
                              "also runs until the time-out)",
}

# Causes already filed as typesafe_carla issues: (pattern on the detail, issue).
KNOWN_ISSUES = [
    (r"_f__\w+' for given arguments \['B_(?:Vehicle|Walker|WalkerAIController|Actor|Sensor|TrafficLight|TrafficSign)'", 76),
    (r"unsupported operand type\(s\) for [+-]: '(?:Location|Vector3D)' and '(?:Location|Vector3D)'", 77),
    (r"raw_data", 78),
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
  (`compat/pycarla/pruned.json`).
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


def write_gaps(primary: str = "ue5-dev") -> Path:
    """GAPS.md from tests/upstream/results: every test, its outcome per mode,
    and the failures grouped by root cause (each group a candidate issue)."""
    refs = sorted({p.stem for m in MODES for p in (RESULTS / m).glob("*.json")},
                  key=lambda r: (r != primary, r))
    lines = ["# typesafe_carla vs CARLA's own PythonAPI tests: gaps", "",
             "Generated by `python -m tools.upstream_tests --gaps` from",
             "`tests/upstream/results/`; do not edit. See tests/upstream/README.md.", "",
             "Modes: **cpython**, the unmodified tests under CPython with `import carla` =",
             "typesafe_carla through tools/pycarla (primary); **codon**, the tests compiled",
             "with typesafe-codon. *not run*: no result recorded (e.g. needs a server).", "",
             "## Summary", "", "| ref | mode | commit | pass | fail | skip | not run |",
             "|---|---|---|---:|---:|---:|---:|"]
    data = {(m, r): load_results(m, r) for m in MODES for r in refs}
    for r in refs:
        for m in MODES:
            d = data[(m, r)]
            outs = [o for f in d["files"].values() for o, _ in f["tests"].values()]
            lines.append(f"| {r} | {m} | {d.get('sha', '')[:10]} | {outs.count('pass')} | "
                         f"{sum(o in FAILURES for o in outs)} | {outs.count('skip')} | "
                         f"{outs.count('not-run')} |")
    for r in refs:
        for m in MODES:
            groups: dict[tuple[str, str], list[str]] = {}
            for rel, f in data[(m, r)]["files"].items():
                for tid, (o, detail) in f["tests"].items():
                    if o in FAILURES:
                        key = f"{rel}::{tid}"
                        if m == "cpython" and key in UPSTREAM_STALE:
                            cause = ("upstream", f"stale upstream test: {UPSTREAM_STALE[key]}")
                        elif m == "cpython" and key in SERVER_ALSO_FAILS:
                            cause = ("server", SERVER_ALSO_FAILS[key])
                        elif rel in INTERACTIVE and m == "cpython":
                            cause = ("infrastructure", INTERACTIVE[rel])
                        else:
                            cause = root_cause(m, o, detail)
                        groups.setdefault(cause, []).append(f"{rel}::{tid}")
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
    lines += ["", "## Codon `--pyext` limitations (harness, not typesafe_carla gaps)", "", CODON_PYEXT_LIMITS]
    lines += ["## Infrastructure limits", "",
              "- CI has no CARLA server: it runs only `unit`; `smoke`, `API` and the top-level",
              "  files need `TSC_CARLA_PORT` (results here come from local runs).",
              "- A test that also fails on its own ref's official Python API, or needs a",
              "  server feature the test server lacks, is listed under its cause above; the",
              "  0.10.0 server's limitations are those failing only in the 0.10.0 results.", ""]
    for r in refs:
        lines += [f"## Every test: {r}", "", "| test | cpython | codon | root cause (cpython, else codon) |",
                  "|---|---|---|---|"]
        files = sorted(set(data[("cpython", r)]["files"]) | set(data[("codon", r)]["files"]), key=_file_order)
        for rel in files:
            ids = list(dict.fromkeys(list(data[("cpython", r)]["files"].get(rel, {}).get("tests", {}))
                                     + list(data[("codon", r)]["files"].get(rel, {}).get("tests", {}))))
            for tid in ids:
                cells, cause = [], ""
                for m in MODES:
                    o, detail = data[(m, r)]["files"].get(rel, {}).get("tests", {}).get(tid, ["not run", ""])
                    cells.append(o)
                    if not cause and o in FAILURES:
                        cause = root_cause(m, o, detail)[1]
                lines.append(f"| `{rel}::{tid}` | {cells[0]} | {cells[1]} | {cause.replace('|', '/')} |")
        lines.append("")
    GAPS.write_text("\n".join(lines) + "\n")
    return GAPS


# ---------------------------------------------------------------------------
# Command line (CI)
# ---------------------------------------------------------------------------

def _summary(mode: str, target: Target, rows: list[tuple[str, Check]], known_ref: bool) -> str:
    total = Check()
    for _, c in rows:
        total.passed += c.passed
        total.xfailed += c.xfailed
        total.skipped += c.skipped
        total.problems += c.problems
    lines = [f"### CARLA PythonAPI tests vs typesafe_carla ({mode} mode): "
             f"{target.ref} @ {target.sha[:12]} ({target.backend})",
             "", f"**{total.counts()}**" + ("" if known_ref else f" (no expectations for {target.ref}: report only)"),
             "", "| file | pass | xfail | skip | unexpected |", "|---|---:|---:|---:|---:|"]
    for rel, c in rows:
        lines.append(f"| {rel} | {c.passed} | {c.xfailed} | {c.skipped} | {len(c.problems)} |")
    if total.problems:
        lines += ["", "Unexpected:", ""] + [f"- {p}" for p in total.problems]
    return "\n".join(lines) + "\n"


def run_mode(mode: str, target: Target, tests: Path, files: list[str], args) -> tuple[list, bool]:
    """Runs `files` in one mode; returns (rows, known_ref)."""
    manifest = load_manifest(mode)
    known_ref = target.ref in manifest
    work = cache_root() / target.sha / "build"
    pydir = pycarla_dir() if mode == "cpython" else None

    def one(rel: str):
        entry = file_expectations(manifest, target.ref, rel)
        ids = discover(tests, rel) or [SCRIPT]
        if isinstance(entry, str) and parse_expectation(entry)[0] == "skip":
            return rel, None, entry, ids
        server = suite_of(rel) in SERVER_SUITES
        if mode == "cpython":
            return rel, run_file_cpython(tests, rel, pydir, skipped_ids(entry), server), entry, ids
        return rel, run_file(tests, rel, work, skipped_ids(entry), server,
                             compile_only=args.compile_only), entry, ids

    rows, results = [], []
    # Server tests share one server: one file at a time.
    jobs = 1 if any(suite_of(f) in SERVER_SUITES for f in files) and not args.compile_only else args.jobs
    with concurrent.futures.ThreadPoolExecutor(max_workers=max(1, jobs)) as pool:
        for rel, result, entry, ids in pool.map(one, files):
            c = check(result, entry, ids)
            rows.append((rel, c))
            print(f"[{mode}] {rel}: {c.counts()}", flush=True)
            if result and (args.verbose or c.problems):
                if result.file_error:
                    print(f"  (file does not compile) {result.file_error}")
                for t in result.tests:
                    print(f"  {t.id}: {t.outcome}{': ' + t.detail if t.detail else ''}")
            for p in c.problems:
                print(f"  UNEXPECTED {p}")
            if result is not None:
                results.append(result)
                if args.update:
                    manifest.setdefault(target.ref, {})[rel] = updated_entry(result, entry)
    if args.update:
        write_manifest(mode, manifest)
        record_results(mode, target, results)
        print(f"updated {MANIFEST.relative_to(ROOT)} and {results_path(mode, target.ref).relative_to(ROOT)}")
    return rows, known_ref


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(prog="python -m tools.upstream_tests", description=__doc__.split("\n\n")[0])
    parser.add_argument("--mode", choices=MODES + ("all",), default="cpython",
                        help="cpython: unmodified tests, carla = tools/pycarla (default); "
                             "codon: converted and compiled with typesafe-codon; all: both")
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
        parser.error("smoke, API and top need a CARLA server: set TSC_CARLA_PORT (and TSC_CARLA_HOST)")

    target = resolve_target()
    tests = fetch_tests(target.sha)
    files = [rel for s in suites for rel in test_files(tests, s)
             if not args.file or rel in args.file]
    print(f"CARLA {target.ref} @ {target.sha} ({target.backend}): {len(files)} files", flush=True)
    failed = False
    for mode in modes:
        rows, known_ref = run_mode(mode, target, tests, files, args)
        text = _summary(mode, target, rows, known_ref)
        if args.summary:
            with open(args.summary, "a") as f:
                f.write(text)
        failed |= known_ref and not args.update and not args.compile_only and any(c.problems for _, c in rows)
    if args.update:
        print(write_gaps())
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
