# CARLA's own PythonAPI tests against typesafe_carla

`tools/upstream_tests.py` runs every Python file of CARLA's test suite
(`PythonAPI/test`: `unit/`, `smoke/`, `API/` and the top-level files) against
typesafe_carla. It uses the tests of the CARLA commit the native library was
built from, in two modes:

| mode | what runs | shows |
|---|---|---|
| **cpython** (primary) | the unmodified tests under CPython. `import carla` is typesafe_carla, built as a CPython extension by `tools/pycarla.py` | real API gaps: missing classes and methods, signatures, behaviour |
| **codon** | the tests converted mechanically and compiled with `typesafe-codon` | whether the same code compiles statically |

`expectations.yaml` records the expected result per mode, CARLA ref and test.
`tests/test_upstream.py` (pytest) and the `libcarla` CI legs fail on any
difference from it, an unexpected pass included, so the list only shrinks.
[`GAPS.md`](GAPS.md) is generated from the recorded results
(`results/<mode>/<ref>.json`). It lists every test with its outcome in both
modes, and groups the failures by root cause; each group is a candidate issue.

| suite | needs | runs in |
|---|---|---|
| `unit` | nothing | pytest (any backend; the mock uses ue5-dev's tests); every `libcarla` CI leg in codon mode, the ubuntu-24.04 leg of each ref in cpython mode too |
| `smoke`, `API`, `top` (the top-level files) | a CARLA server and the `libcarla` backend | pytest and the CLI, only with `TSC_CARLA_PORT` set |
| `ported` ([`ported/`](ported)) | a ue5-dev server | as `smoke`: the tests that need a map no ue5-dev package ships, ported to shipped maps (see below) |

A third mode, `--mode official`, runs any suite with CARLA's own `carla`
module (`TSC_UPSTREAM_PYTHON`'s), every `carla.Client` redirected to the test
server like the other modes. It runs what the cpython manifest runs (not
`skip:` or `exclude:` entries, nor the manifest's `official:` section). With
`--update` its results go to `results/official/`, each file with the CARLA
commit it ran, and GAPS.md attributes a failure the official module shares
(the same outcome and exception) to the server or the upstream test, not to
typesafe_carla. A failure where official passes, or fails differently, stays
a typesafe_carla gap, with the official failure noted.

A suite without expectations for the ref (e.g. 0.10.0's server suites,
recorded in codon mode only; verification is primarily on ue5-dev) is
report-only: its failures are shown, not failed on. `ported` is skipped for a
ref without ported expectations.

Before a server suite and after each server test, every mode resets the
server (actors, synchronous mode; Codon mode with a small typesafe program).
An unreachable server aborts the run.

## Running

```sh
# unit, both modes, against whatever library the checkout uses (build/ = mock by default)
uv run python -m tools.upstream_tests --mode all --suite unit -v
uv run pytest tests/test_upstream.py

# against a real LibCarla build and a server
export TYPESAFE_CARLA_BUILD_DIR=build-carla TSC_CARLA_HOST=127.0.0.1 TSC_CARLA_PORT=2000
uv run python -m tools.upstream_tests --mode all --suite smoke --suite API --suite top -j1 -v

# codon mode without a server: compile only
uv run python -m tools.upstream_tests --mode codon --suite smoke --suite API --suite top --compile-only
```

Server tests run one file at a time, because they load maps and change world
settings on the shared server. Use `127.0.0.1` rather than `localhost` if
`localhost` resolves to `::1` and the server listens on IPv4 only.

In cpython mode the tests run in `TSC_UPSTREAM_PYTHON` (default: the
harness's interpreter). Several smoke tests need numpy and opencv
(`PythonAPI/test/requirements.txt`); a missing one shows up in GAPS.md as an
infrastructure cause. The `carla` package is built into
`<build dir>/pycarla` on first use and rebuilt when typesafe_carla or the
generator changes; `TSC_PYCARLA_DIR` names another one.

| variable | meaning |
|---|---|
| `TSC_UPSTREAM_REF` | the CARLA ref whose expectations (and, if it differs from the build's, tests) to use. Default: the ref the library was built from (`TSC_CARLA_REF_NAME`). For a build from a local checkout, it is the manifest ref whose current commit matches, else the CARLA version. The mock uses `ue5-dev`. |
| `TSC_UPSTREAM_TESTS_DIR` | use a local `PythonAPI/test` instead of fetching one |
| `TSC_UPSTREAM_CACHE` | where fetched tests and builds go (default `<build dir>/upstream-tests/<sha>`) |
| `TSC_UPSTREAM_PYTHON` | the interpreter of cpython mode |
| `TSC_PYCARLA_DIR` | a prebuilt `carla` package (tools/pycarla output) |
| `TSC_CARLA_HOST`, `TSC_CARLA_PORT` | the server for `smoke`, `API` and `top` |

The tests are fetched once per commit SHA, as a shallow, blob-filtered, sparse
checkout of `PythonAPI/test` only.

## Updating the expectations and GAPS.md

```sh
uv run python -m tools.upstream_tests --mode all --suite unit --update
uv run python -m tools.upstream_tests --mode all --suite smoke --suite API --suite top --update -j1   # with a server
uv run python -m tools.upstream_tests --gaps     # regenerate GAPS.md only
```

`--update` rewrites the current ref's entries for the files it ran, keeping
the reasons of tests that still fail. It records the outcomes in `results/`
and regenerates `GAPS.md`. With `--compile-only` (codon mode), it records
only compile failures. A test the manifest does not list is expected to pass.

```yaml
cpython:
  ue5-dev:
    unit/test_transform.py:
      TestTransform.test_values: pass
      TestTransform.test_print: "xfail: fail: AssertionError: ..."
    test_connection.py:
      <script>: pass                              # a file without TestCase classes
codon:
  0.10.0:
    unit/test_client.py: "xfail: compile: ..."    # the whole module does not compile
    smoke/test_x.py:
      TestX.test_y: "skip: crashes the server"    # never run
      TestX.test_z: "selfskip: needs numpy"       # run; expected to skip itself
    ported/smoke/test_x.py:
      TestX.test_w: "exclude: ..."                # not run, not part of the target
```

`skip:` is for a test that must not run (a hang, an interactive script); a
test that calls `skipTest` is recorded as `selfskip:` and keeps running.
`exclude:` is only for the ported originals (below).

## Ported tests (issue #80)

About 55 upstream tests load Town03, Town05(_Opt) or Town01, mostly through
`SmokeTest.tearDown`. No ue5-dev package ships those maps, and the official
module fails these tests the same way. `tools/port_upstream_tests.py` writes
ported copies to [`ported/`](ported), from the upstream tests at the test
server's commit, by mechanical rules. Each file's header records its source,
commit and changes:

- the ported `smoke/__init__.py` reloads a shipped map (Town10HD_Opt) in
  `tearDown`. Smoke tests that fail only through it are copied unchanged, so
  that `from . import SmokeTest` uses the ported base;
- explicit loads of a missing map load a shipped one: Town10HD_Opt, or Town15
  / Mine_01 for the tests that need long straight roads. The test is skipped
  when the server has none of them;
- coordinates written for a straight road of Town03 / Town05 / Town01 go
  through a `MapFrame`, which places that road on the longest straight driving
  lane of the loaded map, keeping distances and angles. Readings (`.y`,
  velocities, yaw) map back, so the assertions are unchanged.

The originals are `exclude:` in expectations.yaml: they are not run, not
counted as failures, and listed in their own GAPS.md section. Each ported test
must first pass with the official module on a ue5-dev server:

```sh
TYPESAFE_CARLA_BUILD_DIR=build-carla TSC_CARLA_HOST=127.0.0.1 TSC_CARLA_PORT=2000 \
  TSC_UPSTREAM_PYTHON=/path/to/venv-with-carla/bin/python \
  uv run python -m tools.upstream_tests --mode official --suite ported --update -j1 -v
python -m tools.port_upstream_tests --exclude-originals   # regenerate after a rule change
```

A ported test that still fails with the official module after a reasonable
port, because of the server's content or physics (`CONTENT_LIMITS`,
`NEEDS_RELOAD` in tools/port_upstream_tests.py), stays measured: `xfail:` in
the typesafe modes (`skip:` for the one that hangs intermittently, `FLAKY`),
and official mode runs it, recording the official failure as the evidence.
When the official module's own bindings are at fault (`OFFICIAL_DEFECTS`), it
goes in the manifest's `official:` section: official runs leave it out,
typesafe runs keep it, and GAPS.md lists it under "Not run with the official
module". Failures record their last traceback frames and full message in
`results/<mode>/<ref>.json` (`traces`), and `-v` prints them.

## cpython mode: tools/pycarla

Codon 0.19's `--pyext` cannot export typesafe_carla's classes directly; the
reasons are under "Codon `--pyext` limitations" in GAPS.md. So
`tools/pycarla.py` generates two layers from the Codon sources (parsed with
Python's `ast`), with no per-class code:

- an extension module, which imports typesafe_carla unchanged. It holds one
  opaque box class per public class and one exported function per method,
  property, field and constructor, with the library's own types and defaults.
  For an untyped library parameter, it exports one overload per type the
  parameter accepts (`GENERIC_PARAMS`, by parameter name). Combinations the
  library rejects at compile time are pruned automatically
  (`compat/pycarla/pruned.json`; only the library's own rejections, `PRUNABLE`:
  any other compile error fails the build). Two library patterns get generic
  treatment: a command's `actor_id` / `actor` pair with the `_MISSING` sentinel
  (exported with an int id; the runtime passes an Actor's id), and the V2X
  dict views (#85), whose `__getitem__(key: Static[str])` cannot be exported:
  their literal keys, read from the source, give each a `_tsc_py()` building
  the Python API's nested dict, which `get()` returns.
- `carla/__init__.py` with [`compat/pycarla/_runtime.py`](../../compat/pycarla/_runtime.py):
  Python classes mirroring the CARLA Python API, built from the generated
  `_spec.json`. They give the class hierarchy (`isinstance(v, carla.Actor)`),
  the enumerations, `carla.command`, callbacks receiving wrapped objects, and
  errors as RuntimeError. As in CARLA's Python API, `listen` / `on_tick`
  callbacks run on a background thread as soon as their data arrives (issue
  #89): at import the runtime turns typesafe_carla's dispatch points off
  (`set_auto_dispatch(False, lock=False)`; the GIL serializes the calls) and
  starts a daemon thread that runs `dispatch_callbacks()` whenever the native
  queue signal changes, waiting for it through ctypes without the GIL. An
  exception in a callback is printed and delivery goes on.

A member the generator cannot wrap raises
`NotImplementedError("pycarla: <Class>.<member> not wrapped: <reason>")`, and
GAPS.md lists it as a harness limitation, not a typesafe_carla gap.
`python -m tools.pycarla --report` lists them.

`TSC_PYCARLA_REDIRECT=host:port` (set by the harness for server suites) points
every `carla.Client` at the test server, since the unmodified tests hard-code
their address (`('localhost', 3654)`, `carla.Client()`).

## codon mode: the conversion

The conversion is generic, the same for every module, with no per-test edits:

- `import carla` / `from carla import ...` become `typesafe_carla`, the
  Python-API compatibility path; its compile-time warnings are expected.
- `import unittest` becomes [`tsc_unittest.codon`](tsc_unittest.codon). Codon's
  bundled `unittest` only prints a failed assertion, and has no
  setUp/tearDown, `skipTest` or discovery.
- Rewrites with the same meaning:
  - `super(Class, self)` becomes `super()`;
  - `from __future__` imports are dropped;
  - `__file__` is defined;
  - the egg-locator boilerplate, and its `import glob`, is dropped.
- For server suites only, the server address is redirected to
  `TSC_CARLA_HOST`/`TSC_CARLA_PORT`.
- A runner is appended, and each `Class.test_method` runs in its own process.
  A file without TestCase classes is a script: it is compiled and run whole.

Codon type-checks only functions that are called. A test that does not compile
is dropped from the runner and the module is compiled again. An error in
module-level code fails the whole file.
