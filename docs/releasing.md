# Releasing to PyPI

typesafe_carla publishes two distributions:

| Distribution | Contents | Version | Built by |
|---|---|---|---|
| `typesafe-carla` | Codon sources, `libtypesafe_carla_ffi.so` (LibCarla linked in statically), the `typesafe-codon` launcher | `python/typesafe_carla/__init__.py` | scikit-build-core (+ cibuildwheel, manylinux_2_28) |
| `typesafe-carla-toolchain` | The pinned Codon compiler | `toolchain/pyproject.toml` (= Codon version, `.postN` for repackaging) | hatchling + `toolchain/hatch_build.py` |

Both wheels are Linux x86_64 only for now. `typesafe-carla` is tagged
`py3-none-manylinux_*` because it contains no CPython extension, so one
wheel serves every Python 3 version.

## Release tags

A release tag is `<version>-<CARLA ref>-<YYYYMMDD>`:

| Part | Meaning | Example |
|---|---|---|
| `<version>` | the package version, `__version__` in `python/typesafe_carla/__init__.py` (and `codon/typesafe_carla/__init__.codon`) | `0.1.0` |
| `<CARLA ref>` | the carla-simulator/carla branch, tag or commit SHA LibCarla is built from; may contain `-`, `.` and `/`; UE4 refs (`0.9.x`, `ue4-dev`) are rejected | `ue5-dev`, `0.10.0` |
| `<YYYYMMDD>` | the date (UTC, committer date) of the CARLA ref's **last commit** to build | `20260915` |

So `0.1.0-ue5-dev-20260915` means "typesafe_carla 0.1.0 with LibCarla from
`ue5-dev` as of its last commit on 2026-09-15", and `0.1.0-0.10.0-20250320`
pins the `0.10.0` tag. The workflow resolves the tag to a commit SHA — the
latest commit reachable from the ref with a committer date on or before the
end of that day (UTC) — and builds LibCarla from that SHA, so even a release
of the moving `ue5-dev` branch is reproducible. It fails if the ref does not
exist or has no commit by that date, and logs a warning if the commit is from
an earlier day (the ref had no commit on the tag date).

To find the date of a ref's last commit, in a CARLA checkout:

```sh
git fetch origin ue5-dev
git log -1 --format=%cd --date=format:%Y%m%d origin/ue5-dev
```

`--date=format:` prints the date in the committer's own time zone; the tag
date is UTC, so near midnight use
`TZ=UTC git log -1 --format=%cd --date=format-local:%Y%m%d origin/ue5-dev`.
`python tools/release_tag.py check <tag>` parses a tag and prints the commit
it resolves to; `python tools/release_tag.py make <ref>` forms the tag for the
current package version and the ref's latest commit.

The PyPI version is `<version>` alone: the CARLA ref and date are not part of
it. PyPI accepts each version once, so a re-release — a new date or a
different CARLA ref — needs a version bump. What a wheel was built from is
recorded in it: `typesafe_carla/_native/BUILD_INFO.json` and
`typesafe-codon info` show the CARLA ref (the resolved SHA, for a release)
and commit; the annotated tag and the GitHub release name the ref and SHA.

## One-time setup

1. Create the `typesafe-carla` and `typesafe-carla-toolchain` projects on
   PyPI and TestPyPI (a first manual upload, or a "pending publisher").
2. For each project on both indexes, add a **Trusted Publisher**:
   - owner/repository: `hakuturu583/typesafe_carla`
   - workflow: `release.yml`
   - environment: `pypi` (PyPI) or `testpypi` (TestPyPI)
3. In the GitHub repository settings, create the environments `pypi` and
   `testpypi`. Protecting `pypi` with required reviewers is recommended.
4. Create the labels `release:major`, `release:minor` and `release:patch`.
5. The `release` job pushes the version bump to `main` and the tag with
   `GITHUB_TOKEN`: if `main` is protected, allow GitHub Actions to push to it
   (or the release job fails before tagging).
6. Optionally set the repository variable `CARLA_RELEASE_REF` (Settings →
   Secrets and variables → Actions → Variables) to the CARLA ref automatic
   releases build from. The default is `ue5-dev`.
7. Choose a license and add a `LICENSE` file. It is packaged automatically
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

Releases set `CARLA_GIT_REF` to the commit SHA resolved from the release tag,
so for them the recorded ref and commit are both that SHA.

## Cutting a release

`.github/workflows/release.yml` has three entry points:

```
push to main ──► release ──┐  (bump, tag, push, GitHub release; only with a release:* label)
push a tag ────────────────┤
workflow_dispatch ─────────┴─► resolve ──► verify ──┬─► sdist ─────┐
                                                    ├─► wheel ─────┼─► publish
                                                    └─► toolchain ─┘
```

It is one workflow because a tag pushed with `GITHUB_TOKEN` starts no new
workflow runs: the auto-release tag is built and published by the run that
cut it.

### Everyday: merge a PR labelled `release:patch`

Give the PR exactly one of `release:major`, `release:minor` or
`release:patch` and merge it. On the push to `main`, the `release` job:

1. reads the merged PR's labels (none: nothing happens; more than one: error);
2. bumps `__version__` in `python/typesafe_carla/__init__.py` **and**
   `codon/typesafe_carla/__init__.codon` with `tools/bump_version.py <level>`;
3. resolves the latest commit of `CARLA_RELEASE_REF` (default `ue5-dev`)
   and forms the tag `<new version>-<ref>-<date of that commit>`, failing if
   it already exists;
4. commits `chore(release): <tag>` as github-actions[bot], creates the
   annotated tag (its message names the CARLA SHA), pushes `main` and the
   tag, and creates a GitHub release with generated notes.

The rest of the run builds and publishes that tag, from the CARLA SHA the
`release` job resolved. Bump `TSC_ABI_VERSION_*` in `ffi.h` and `_ffi.codon`
in the PR itself if the C ABI changed.

### Hand-cut tag: another CARLA ref or date

1. Make sure `__version__` on the commit to tag is a version not yet on PyPI
   (`python tools/bump_version.py patch|minor|major|X.Y.Z` bumps both files;
   merge that without a release label).
2. Tag and push, for example:
   ```sh
   git tag -a 0.2.0-0.10.0-20250320 -m "Release 0.2.0-0.10.0-20250320"
   git push origin 0.2.0-0.10.0-20250320
   ```
   The `release` job is skipped; `resolve` checks the tag
   (`tools/release_tag.py check`) and resolves its CARLA commit.

### Dry run

Run **Release** manually (workflow_dispatch) with the `carla_ref` you want
(built as given, not resolved to a SHA). It builds and checks every artifact
and uploads them as workflow artifacts; with `publish=true` it also uploads to
TestPyPI. `tests/distribution/clean_env.sh <dir with the downloaded wheels>
<port>` then checks the design's success criteria (section 44) in a clean
container against a running CARLA server: `uv sync`,
`typesafe-codon build -release main.py`, `./main`, a compile-time rejection,
and no Codon, CARLA Python package or `CODON_PATH` set up by hand.

### What every release run checks

- `verify`: the tag's version equals both `__version__`s, and the mock suite
  (C++ build, `ctest`, `pytest`) passes on the tagged commit;
- builds the sdist;
- builds the wheel in manylinux_2_28 with `TSC_BACKEND=libcarla`, then runs
  `tools/check_wheel.py`. That fails the release if the backend is not
  `libcarla`, the CARLA ref or commit differs from the resolved one, libpython
  is linked, or the library exports anything besides `tsc_*`;
- builds the toolchain wheel and checks that the bundled Codon runs;
- `twine check --strict`, then publishes everything to PyPI. An
  already-published toolchain version is skipped.

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
