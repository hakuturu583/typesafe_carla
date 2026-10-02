# Releasing to PyPI

typesafe_carla publishes two distributions:

| Distribution | Contents | Version | Built by |
|---|---|---|---|
| `typesafe-carla` | Codon sources, `libtypesafe_carla_ffi.so` (LibCarla linked in statically), the `typesafe-codon` launcher | `python/typesafe_carla/__init__.py` | scikit-build-core (+ cibuildwheel, manylinux_2_28) |
| `typesafe-carla-toolchain` | The pinned Codon compiler | `toolchain/pyproject.toml` (= Codon version, `.postN` for repackaging) | hatchling + `toolchain/hatch_build.py` |

Both wheels are Linux x86_64 only for now. `typesafe-carla` is tagged
`py3-none-manylinux_*` because it contains no CPython extension, so one
wheel serves every Python 3 version.

## One-time setup

1. Create the `typesafe-carla` and `typesafe-carla-toolchain` projects on
   PyPI and TestPyPI (a first manual upload, or a "pending publisher").
2. For each project on both indexes, add a **Trusted Publisher**:
   - owner/repository: `hakuturu583/typesafe_carla`
   - workflow: `release.yml`
   - environment: `pypi` (PyPI) or `testpypi` (TestPyPI)
3. In the GitHub repository settings, create the environments `pypi` and
   `testpypi`. Protecting `pypi` with required reviewers is recommended.
4. Choose a license and add a `LICENSE` file. It is packaged automatically
   through `wheel.license-files`.

No API tokens are needed: `release.yml` authenticates through OIDC.

## Choosing the CARLA ref

LibCarla is compiled from CARLA UE5 sources while the wheel is built. The ref
comes from, in order of precedence:

1. `-C cmake.define.TSC_CARLA_GIT_REF=<ref>` (scikit-build config setting)
   or `-DTSC_CARLA_GIT_REF=<ref>` (plain CMake);
2. the `CARLA_GIT_REF` environment variable;
3. the default, `ue5-dev`.

`CARLA_SOURCE_DIR` (or `TSC_CARLA_SOURCE_DIR`) builds from a local checkout
instead of fetching. A ref may be a branch, a tag (e.g. `0.10.0`) or a commit
SHA. UE4 refs (`0.9.x`, `ue4-dev`) are rejected.

`ue5-dev` moves, so every build records what it actually used:

- `typesafe-codon info`, or `carla.libcarla_git_ref()` /
  `carla.libcarla_git_commit()` in Codon;
- `typesafe_carla/_native/BUILD_INFO.json` inside the wheel.

For a reproducible release, run the release workflow with `carla_ref` set to
a tag or SHA. The tag-triggered release uses `ue5-dev`.

## Cutting a release

1. Bump `__version__` in `python/typesafe_carla/__init__.py` **and**
   `codon/typesafe_carla/__init__.codon` (a test checks they agree). Bump
   `TSC_ABI_VERSION_*` in `ffi.h` and `_ffi.codon` if the C ABI changed.
2. Dry run: run **Release** manually (workflow_dispatch) with the CARLA ref
   you want. It builds and checks every artifact and uploads them as workflow
   artifacts; with `publish=true` it also uploads to TestPyPI.
   `tests/distribution/clean_env.sh <dir with the downloaded wheels> <port>`
   then checks the design's success criteria (section 44) in a clean
   container against a running CARLA server: `uv sync`,
   `typesafe-codon build -release main.py`, `./main`, a compile-time
   rejection, and no Codon, CARLA Python package or `CODON_PATH` set up by
   hand.
3. Tag and push: `git tag v0.1.0 && git push origin v0.1.0`. The workflow:
   - checks that the tag matches `__version__`;
   - builds the sdist;
   - builds the wheel in manylinux_2_28 with `TSC_BACKEND=libcarla`, then
     runs `tools/check_wheel.py`. That fails the release if the backend is
     not `libcarla`, the CARLA ref differs, libpython is linked, or the
     library exports anything besides `tsc_*`;
   - builds the toolchain wheel and checks that the bundled Codon runs;
   - publishes everything to PyPI. An already-published toolchain version is
     skipped.

## Updating Codon

1. Update `CODON_VERSION`, the archive SHA-256 and `LICENSE_SHA256` in
   `toolchain/hatch_build.py`, `version` in `toolchain/pyproject.toml`,
   `CODON_VERSION` in `toolchain/src/typesafe_carla_toolchain/__init__.py`,
   and `SUPPORTED_CODON_SERIES` in `python/typesafe_carla/toolchain.py`.
2. Adjust the `typesafe-carla-toolchain` requirement in `pyproject.toml`.
3. Run the compile-pass/fail suites: their expected error messages are Codon's
   wording and may change between releases.

## Building from source with a different CARLA ref

Users who need a specific CARLA ref can build the sdist themselves:

```sh
CARLA_GIT_REF=<branch|tag|sha> pip install --no-binary typesafe-carla --force-reinstall typesafe-carla
```

or, in a uv project, as described in [usage.md](usage.md#which-carla).

This needs git, a C++20 compiler and network access to GitHub. CMake is
installed automatically if the system one is older than 3.27.2.
