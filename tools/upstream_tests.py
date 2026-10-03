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
SUITES = ("unit", "smoke", "API")
SERVER_SUITES = ("smoke", "API")
DEFAULT_MOCK_REF = "ue5-dev"  # the mock mirrors LibCarla ue5-dev
REASON_MAX = 240

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

    A shallow, blob-filtered, sparse fetch of that directory only (as
    cmake/FetchCarla.cmake fetches LibCarla). TSC_UPSTREAM_TESTS_DIR names a
    local PythonAPI/test to use instead.
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
    if (dest / ".complete").is_file():
        return tests
    cache.mkdir(parents=True, exist_ok=True)
    tmp = Path(tempfile.mkdtemp(prefix=f"{sha}.", dir=cache))
    try:
        _git("init", "-q", ".", cwd=tmp)
        _git("remote", "add", "origin", REPOSITORY, cwd=tmp)
        _git("sparse-checkout", "set", "--no-cone", "/PythonAPI/test/", cwd=tmp)
        _git("fetch", "-q", "--depth", "1", "--filter=blob:none", "origin", sha, cwd=tmp)
        _git("checkout", "-q", "FETCH_HEAD", cwd=tmp)
        shutil.rmtree(tmp / ".git")
        (tmp / ".complete").write_text(sha + "\n")
        shutil.rmtree(dest, ignore_errors=True)
        tmp.rename(dest)
    finally:
        shutil.rmtree(tmp, ignore_errors=True)
    return tests


def test_files(tests: Path, suite: str) -> list[str]:
    """`suite/test_*.py` paths, relative to PythonAPI/test."""
    return sorted(f"{suite}/{p.name}" for p in (tests / suite).glob("test_*.py"))


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
# Expectations
# ---------------------------------------------------------------------------

def load_manifest(path: Path = MANIFEST) -> dict:
    import yaml

    data = yaml.safe_load(path.read_text()) if path.is_file() else None
    return (data or {}).get("refs") or {}


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
# typesafe_carla, per CARLA ref. See tests/upstream/README.md.
#
#   refs:
#     <CARLA ref>:
#       <suite>/<file>.py: "xfail: <reason>" | "skip: <reason>"   # the whole file
#       <suite>/<file>.py:
#         <Class>.<test_method>: pass | "xfail: <reason>" | "skip: <reason>"
#
# A test not listed is expected to pass. pytest (tests/test_upstream.py) and
# CI fail on an unexpected failure AND on an unexpected pass, so this list only
# shrinks. `python -m tools.upstream_tests --suite <suite> --update` rewrites
# the current ref's entries from a run (existing reasons are kept).
"""


def write_manifest(refs: dict, path: Path = MANIFEST) -> None:
    import yaml

    def order(name: str):
        return (SUITES.index(name.split("/")[0]) if name.split("/")[0] in SUITES else 9, name)

    data = {"refs": {ref: {f: refs[ref][f] for f in sorted(refs[ref], key=order)}
                     for ref in sorted(refs)}}
    text = yaml.safe_dump(data, sort_keys=False, width=1000, allow_unicode=True)
    path.write_text(_MANIFEST_HEADER + "\n" + text)


# ---------------------------------------------------------------------------
# Command line (CI)
# ---------------------------------------------------------------------------

def _summary(target: Target, rows: list[tuple[str, Check]], known_ref: bool) -> str:
    total = Check()
    for _, c in rows:
        total.passed += c.passed
        total.xfailed += c.xfailed
        total.skipped += c.skipped
        total.problems += c.problems
    lines = [f"### CARLA PythonAPI tests vs typesafe_carla: {target.ref} @ {target.sha[:12]} ({target.backend})",
             "", f"**{total.counts()}**" + ("" if known_ref else f" (no expectations for {target.ref}: report only)"),
             "", "| file | pass | xfail | skip | unexpected |", "|---|---:|---:|---:|---:|"]
    for rel, c in rows:
        lines.append(f"| {rel} | {c.passed} | {c.xfailed} | {c.skipped} | {len(c.problems)} |")
    if total.problems:
        lines += ["", "Unexpected:", ""] + [f"- {p}" for p in total.problems]
    return "\n".join(lines) + "\n"


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(prog="python -m tools.upstream_tests", description=__doc__.split("\n\n")[0])
    parser.add_argument("--suite", action="append", choices=SUITES,
                        help="suite(s) to run (default: unit, plus smoke and API when TSC_CARLA_PORT is set)")
    parser.add_argument("--summary", help="append a Markdown summary to this file ($GITHUB_STEP_SUMMARY)")
    parser.add_argument("--update", action="store_true",
                        help="rewrite this ref's expectations from the results")
    parser.add_argument("--compile-only", action="store_true",
                        help="only compile (smoke and API then need no server); with --update, "
                             "records compile failures only")
    parser.add_argument("-j", "--jobs", type=int, default=min(4, os.cpu_count() or 1))
    parser.add_argument("-v", "--verbose", action="store_true", help="print every test's outcome")
    args = parser.parse_args(argv)
    suites = args.suite or (["unit"] + (list(SERVER_SUITES) if os.environ.get("TSC_CARLA_PORT") else []))
    if (any(s in SERVER_SUITES for s in suites) and not os.environ.get("TSC_CARLA_PORT")
            and not args.compile_only):
        parser.error("smoke and API need a CARLA server: set TSC_CARLA_PORT (and TSC_CARLA_HOST)")

    target = resolve_target()
    tests = fetch_tests(target.sha)
    manifest = load_manifest()
    known_ref = target.ref in manifest
    work = cache_root() / target.sha / "build"
    files = [rel for s in suites for rel in test_files(tests, s)]
    print(f"CARLA {target.ref} @ {target.sha} ({target.backend}): {len(files)} files", flush=True)

    def one(rel: str):
        entry = file_expectations(manifest, target.ref, rel)
        ids = discover(tests, rel)
        if isinstance(entry, str) and parse_expectation(entry)[0] == "skip":
            return rel, None, entry, ids
        server = rel.split("/")[0] in SERVER_SUITES
        return rel, run_file(tests, rel, work, skipped_ids(entry), server,
                             compile_only=args.compile_only), entry, ids

    rows = []
    with concurrent.futures.ThreadPoolExecutor(max_workers=max(1, args.jobs)) as pool:
        for rel, result, entry, ids in pool.map(one, files):
            c = check(result, entry, ids)
            rows.append((rel, c))
            print(f"{rel}: {c.counts()}", flush=True)
            if result and (args.verbose or c.problems):
                if result.file_error:
                    print(f"  (file does not compile) {result.file_error}")
                for t in result.tests:
                    print(f"  {t.id}: {t.outcome}{': ' + t.detail if t.detail else ''}")
            for p in c.problems:
                print(f"  UNEXPECTED {p}")
            if args.update and result is not None:
                manifest.setdefault(target.ref, {})[rel] = updated_entry(result, entry)
    if args.update:
        write_manifest(manifest)
        print(f"updated {MANIFEST.relative_to(ROOT)} for {target.ref}")
    text = _summary(target, rows, known_ref)
    if args.summary:
        with open(args.summary, "a") as f:
            f.write(text)
    failed = known_ref and not args.update and not args.compile_only and any(c.problems for _, c in rows)
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
