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

- **The date must be over.** A hand-cut tag's date must be before today
  (UTC); otherwise commits landing later that day would change what the tag
  resolves to.
- **A SHA ref must name its own commit.** If the ref is a commit SHA (7 to 40
  hex digits), it must resolve to that commit, and the tag date must be that
  commit's date. GitHub would otherwise answer with the SHA's newest ancestor
  on or before the date, and build a different commit.
- **Auto-release tags are not re-checked.** The `release` job tags the ref's
  latest commit with that commit's own date, which may be today. It passes the
  SHA straight to the build instead of resolving the tag again. For any
  release, the SHA in the annotated tag message and in the GitHub release
  notes is authoritative.

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
current package version and the ref's latest commit;
`python tools/release_tag.py unpublished` fails if the package version is
already on PyPI.

The PyPI version is `<version>` alone: the CARLA ref and date are not part of
it. PyPI accepts each version once, so a re-release — a new date or a
different CARLA ref — needs a version bump. The workflow enforces this before
building anything:

- `release` fails if the new version already has a tag (`<version>-*`) or is
  on PyPI;
- `resolve` fails if the version is already on PyPI (TestPyPI for a dry run
  with `publish=true`), except on a re-run (`run_attempt > 1`), which may be
  finishing a partial upload;
- `typesafe-carla` is uploaded without `skip-existing` (except on a re-run),
  so an existing file is an error rather than a silent no-op. Only the
  toolchain upload skips existing files, because its version follows Codon,
  not the release.

What a wheel was built from is
recorded in it: `typesafe_carla/_native/BUILD_INFO.json` and
`typesafe-codon info` show the CARLA ref (the resolved SHA, for a release)
and commit; the annotated tag and the GitHub release name the ref and SHA.

## One-time setup

1. Create the `typesafe-carla` and `typesafe-carla-toolchain` projects on
   PyPI and TestPyPI (a first manual upload, or a "pending publisher").
2. For each project on both indexes, add a **Trusted Publisher**:
   - owner/repository: `hakuturu583/typesafe_carla`
   - workflow: `release.yml`
   - environment:

     | | PyPI | TestPyPI |
     |---|---|---|
     | `typesafe-carla` | `pypi` | `testpypi` |
     | `typesafe-carla-toolchain` | `pypi-toolchain` | `testpypi-toolchain` |

   The two projects need different environments, and every publisher must
   name one: a publisher with no environment matches both jobs. Trusted
   publishers match on (repository, workflow, environment). With identical
   claims, which project's pending publisher a token exchange reifies is
   arbitrary, and the token is scoped to that one project only, so a run can
   upload one package and fail the other ("400 Non-user identities cannot
   create new projects"). Separate environments make the lookup unambiguous.

   **Migrating from a single environment:** on each index, delete any
   `typesafe-carla-toolchain` publisher (pending or active) registered with
   environment `pypi` / `testpypi` or with no environment, then add it with
   `pypi-toolchain` / `testpypi-toolchain`. PyPI looks up pending publishers
   first, so a stale one would match `publish`'s token exchange and fail it.
   A `typesafe-carla` publisher with `pypi` / `testpypi` is already correct.
3. In the GitHub repository settings, create the environments `pypi`,
   `pypi-toolchain`, `testpypi` and `testpypi-toolchain` **before the first
   release**: GitHub creates a missing environment on first use, without any
   protection. Protecting `pypi` and `pypi-toolchain` with required reviewers
   is recommended; a release then needs two approvals in sequence (the
   toolchain job, then `typesafe-carla`). If they have
   deployment branch/tag rules, they must allow both `main` (the auto-release
   run is a push to `main`) and, for hand-cut tags, the release tags
   (`[0-9]*.[0-9]*.[0-9]*-*`).
4. Create the labels `release:major`, `release:minor` and `release:patch`.
5. The `release` job pushes the version bump to `main` and the tag with
   `GITHUB_TOKEN`: if `main` is protected, allow GitHub Actions to push to it
   (or the release job fails before tagging).
6. Optionally set the repository variable `CARLA_RELEASE_REF` (Settings →
   Secrets and variables → Actions → Variables) to the CARLA ref automatic
   releases build from. The default is `ue5-dev`.

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

Releases build from the commit SHA resolved from the release tag (a dry
run, from the current commit of its `carla_ref`) and pass the ref itself as
`TSC_CARLA_REF_NAME`, so a wheel records e.g. ref `ue5-dev` and commit
`1360bb9…`; `tools/check_wheel.py --carla-ref <ref> --carla-commit <sha>`
checks both. `tools.bindgen validate` matches `missing_in` against that ref
name.

### LibCarla build cache

Both CI's `libcarla` job and the release `wheel` job cache the LibCarla build
with `actions/cache`, keyed by the resolved CARLA commit SHA:

- CI resolves the ref first (`tools/resolve_carla_ref.sh`, i.e.
  `git ls-remote`; a tag resolves to the commit it tags), builds from that
  SHA, and caches the whole CMake build directory (`build/`: the CARLA sparse
  checkout, the fetched and built dependencies under `_deps/`, LibCarla under
  `carla/`, and the shim) under `libcarla-<os>-<arch>-<runner>-gcc-<version>-
  <toolchain hash>-<CARLA SHA>-<hash of CMakeLists.txt and cmake/>`, one per
  Ubuntu LTS and CARLA ref of the matrix. The toolchain hash
  is of `tools/libcarla_cache_guard.sh --toolchain`: compilers, CMake and
  Ninja. A run on an unchanged commit restores it and only the shim is
  recompiled; a new `ue5-dev` commit misses and builds from scratch. When
  only our CMake files changed, the newest entry of the same SHA and
  toolchain is restored without its `CMakeCache.txt`, so the configure starts
  afresh while make keeps the up-to-date objects. Pull requests restore but
  never save: their entries would be visible to that pull request only and
  push `main`'s out.
- The release `wheel` job keys by the cibuildwheel version (pinned exactly,
  as each release pins its own manylinux image and compiler), the manylinux
  image, the exact SHA it builds and the hash of `CMakeLists.txt`, `cmake/`
  and `pyproject.toml`
  (`libcarla-wheel-<image>-cibuildwheel-<version>-<CARLA SHA>-<hash>`), with
  no fallback key. The cached directory is a host directory bind-mounted into
  the cibuildwheel container at `/libcarla-build` and used as
  scikit-build-core's `build-dir`. Runs on `main` (auto-release, dry runs)
  read and save `main`'s entries; a hand-cut tag's run can read `main`'s
  entries but saves to the tag's own scope, where no later run looks.
- A CMake build directory is not relocatable, so it is always restored to
  the same absolute path. Before configuring, `tools/libcarla_cache_guard.sh`
  compares the CARLA SHA, compilers and CMake recorded in the directory with
  the current ones and empties it on any difference, so a release never
  reuses objects from another CARLA commit or toolchain. A miss is a clean
  build, the same as without the cache.
- CARLA fetches rpclib and recastnavigation from branch archives
  (`archive/refs/heads/<branch>.zip`), not pinned commits, so an entry
  freezes them as they were when it was built; a fresh build of the same
  CARLA SHA may get newer ones.

An entry is about 275 MB compressed (1.1 GB unpacked, mostly Boost sources),
so CI's six legs hold about 1.7 GB; a new `ue5-dev` commit adds three.
GitHub evicts the least recently used entries beyond 10 GB per repository.

The wheel also ships `_native/THIRD_PARTY_NOTICES`, the license notices of
everything LibCarla links statically. They are collected from the fetched
sources at configure time by `cmake/ThirdPartyNotices.cmake`. If a ref links
a new library (directly or transitively), vendors something new in
`LibCarla/source/third-party/`, or drops a license text, configuring fails.
Copyright holders other than CVC in LibCarla's sources are listed
automatically; one in a file that does not say MIT also fails. Add the component's notice there
before releasing that ref.

## Cutting a release

`.github/workflows/release.yml` has three entry points:

```
push to main ──► release ──┐  (bump, tag, push, draft GitHub release; only with a release:* label)
push a tag ────────────────┤
workflow_dispatch ─────────┴─► resolve ──► verify ──┬─► sdist ─────┐
                                                    ├─► wheel ─────┼─► publish-toolchain ──► publish ──► github-release
                                                    └─► toolchain ─┘
```

`publish-toolchain` (environment `<index>-toolchain`) uploads
`typesafe-carla-toolchain`; `publish` (environment `<index>`) then uploads
`typesafe-carla`. They are separate jobs so each project has its own trusted
publisher (see One-time setup).

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
   the version is already tagged or on PyPI;
4. commits `chore(release): <tag>` as github-actions[bot] on top of the
   merged commit, creates the annotated tag (its message names the CARLA
   SHA), pushes `main` (without force: if `main` moved meanwhile, the push
   fails) and the tag, and drafts a GitHub release with generated notes.

The rest of the run builds and publishes that tag, from the CARLA SHA the
`release` job resolved; `github-release` publishes the draft once the
packages are on PyPI. Release cuts never run concurrently. Bump `TSC_ABI_VERSION_*` in `ffi.h` and `_ffi.codon`
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
   (`tools/release_tag.py check`) and resolves its CARLA commit. The date
   must be before today (UTC). After the upload, `github-release` creates a
   GitHub release for the tag.

### Dry run

Run **Release** manually (workflow_dispatch) with the `carla_ref` you want
(resolved to its current commit SHA, which is what is built). It builds and checks every artifact
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
  is linked, the library exports anything besides `tsc_*`, or
  `_native/LICENSE.CARLA` or `_native/THIRD_PARTY_NOTICES` is missing or
  empty;
- builds the toolchain wheel and checks that the bundled Codon runs;
- `publish-toolchain` runs `twine check --strict` on every artifact (sdist,
  wheel, toolchain), so nothing is uploaded unless all of them pass, then
  publishes the toolchain (an already-published toolchain version is
  skipped); `publish` then publishes `typesafe-carla` (an existing file
  fails);
- `github-release` publishes (or, for a hand-cut tag, creates) the GitHub
  release.

### When a release fails after the tag is pushed

The tag (and for the auto path, the version bump on `main` and a draft GitHub
release) already exist, but PyPI may have nothing or only part of the files.

- **Failure before `publish-toolchain`** (verify, build or wheel checks): nothing was
  uploaded. If it was transient (a runner, network or GitHub outage), use
  **Re-run failed jobs**. If the commit or the CARLA ref is broken, abandon
  the version: delete the draft GitHub release and the tag
  (`git push origin :refs/tags/<tag>`), fix it on `main`, and release again.
  The auto path bumps to the next version, and the bump commit stays on
  `main`; that is harmless.
- **Failure in `publish-toolchain`**: `typesafe-carla` was not uploaded.
  **Re-run failed jobs**; the toolchain upload always skips existing files.
- **Failure during `publish`** (some files uploaded): **Re-run failed jobs**.
  On a re-run (`run_attempt > 1`), `resolve` lets an already-published version
  through and `typesafe-carla` is uploaded with `skip-existing`, so the
  missing files are added. Never delete a tag whose version reached PyPI: the
  version cannot be uploaded again.
- **Failure in `github-release`**: re-run it, or publish the draft by hand
  (`gh release edit <tag> --draft=false`).
- **Failure in `release` after pushing** (for example, when drafting the
  GitHub release): the tag exists, but this run built nothing. Re-running
  `release` fails because the tag exists. Delete the tag (and any draft
  release), then push it again by hand: the hand-cut path builds and
  publishes it, provided its date is before today (UTC). Otherwise, wait a
  day. The re-pushed tag resolves to the last commit of that day, which may
  be later than the SHA first recorded.

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
