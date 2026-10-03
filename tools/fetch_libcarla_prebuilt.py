#!/usr/bin/env python3
"""Downloads CI's LibCarla prebuilt for a CARLA ref and this machine's compiler,
and prints the path of the unpacked prefix (for TSC_CARLA_PREBUILT_DIR).

    tools/fetch_libcarla_prebuilt.py <ref> [dest]
    tools/fetch_libcarla_prebuilt.py --print-name <ref>

CI's `libcarla` job publishes, from its runs on main (push and the daily
schedule), one workflow artifact per Ubuntu LTS leg and CARLA commit
(docs/releasing.md, "LibCarla prebuilt"), named

    libcarla-prebuilt-<os>-<compiler><major>-<ABI hash>-<CARLA commit SHA>

where the ABI hash is of `tools/libcarla_cache_guard.sh --abi` (compilers,
C++ standard library, glibc) for CC/CXX (default cc/c++). This script:

1. resolves <ref> to its commit (tools/resolve_carla_ref.sh);
2. forms the name for this machine (--print-name prints it; CI names its
   uploads with it);
3. finds the newest such artifact of a successful ci.yml run on main of the
   repository itself (`gh api`; artifacts need a token even for a public
   repository);
4. downloads it, checks the archive's digest, the manifest (commit, ABI) and
   the sha256 of every file, and unpacks it into
   ${XDG_CACHE_HOME:-~/.cache}/typesafe_carla/libcarla-prebuilt/<name>
   (or [dest]), where later calls find it without downloading.

Exit status: 0 and the prefix on stdout; 2 if there is no prebuilt to use
(no gh, not logged in, no matching artifact, network): build from source;
1 on anything else (a download that fails verification, bad usage).
Messages go to stderr.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import shutil
import subprocess
import sys
import tempfile
import zipfile
from pathlib import Path

TOOLS = Path(__file__).resolve().parent
ARTIFACT_REPO = "hakuturu583/typesafe_carla"
WORKFLOW_PATH = ".github/workflows/ci.yml"
EVENTS = {"push", "schedule"}
FORMAT = 1


class Unavailable(Exception):
    """No prebuilt to use; the caller builds from source (exit status 2)."""


def run(cmd: list[str], **kw) -> str:
    return subprocess.run(cmd, check=True, text=True, stdout=subprocess.PIPE, **kw).stdout


def resolve(ref: str, repository: str) -> str:
    try:
        return run(["bash", str(TOOLS / "resolve_carla_ref.sh"), ref, repository]).strip()
    except subprocess.CalledProcessError as e:
        raise Unavailable(f"cannot resolve CARLA {ref} (exit {e.returncode})") from e


def abi_fingerprint() -> str:
    # Trailing newline included: CI hashes the command's output.
    return run(["bash", str(TOOLS / "libcarla_cache_guard.sh"), "--abi"])


def os_label() -> str:
    fields = {}
    try:
        for line in Path("/etc/os-release").read_text().splitlines():
            key, _, value = line.partition("=")
            fields[key] = value.strip().strip('"')
    except OSError:
        pass
    return f"{fields.get('ID', 'linux')}-{fields.get('VERSION_ID', 'unknown')}"


def artifact_name(sha: str, abi: str) -> str:
    # "cxx=gcc 11.4.0, ..." -> "gcc11"
    cxx = next((l for l in abi.splitlines() if l.startswith("cxx=")), "cxx=unknown 0")
    compiler, _, version = cxx[4:].split(",")[0].partition(" ")
    major = version.split(".")[0]
    digest = hashlib.sha256(abi.encode()).hexdigest()[:12]
    return f"libcarla-prebuilt-{os_label()}-{compiler}{major}-{digest}-{sha}"


def gh(*args: str, binary: bool = False, out=None):
    try:
        if out is not None:
            subprocess.run(["gh", *args], check=True, stdout=out, stderr=subprocess.PIPE)
            return None
        res = subprocess.run(["gh", *args], check=True, stdout=subprocess.PIPE,
                             stderr=subprocess.PIPE)
    except subprocess.CalledProcessError as e:
        err = e.stderr.decode(errors="replace").strip() if e.stderr else ""
        raise Unavailable(f"gh {' '.join(args[:2])} failed: {err}") from e
    return res.stdout if binary else json.loads(res.stdout)


def find_artifact(repo: str, name: str) -> dict:
    if shutil.which("gh") is None:
        raise Unavailable("no `gh` (GitHub CLI) to download a LibCarla prebuilt with")
    if subprocess.run(["gh", "auth", "status"], stdout=subprocess.DEVNULL,
                      stderr=subprocess.DEVNULL).returncode != 0:
        raise Unavailable("`gh` is not logged in (`gh auth login`); GitHub serves workflow "
                          "artifacts only with a token")
    listing = gh("api", "-X", "GET", f"repos/{repo}/actions/artifacts",
                 "-f", f"name={name}", "-f", "per_page=100")
    candidates = []
    for art in listing.get("artifacts", []):
        run_ = art.get("workflow_run") or {}
        if (art.get("name") != name or art.get("expired")
                or run_.get("head_branch") != "main"
                or run_.get("head_repository_id") != run_.get("repository_id")):
            continue
        candidates.append(art)
    for art in sorted(candidates, key=lambda a: a["created_at"], reverse=True):
        info = gh("api", f"repos/{repo}/actions/runs/{art['workflow_run']['id']}")
        if (info.get("path") == WORKFLOW_PATH and info.get("event") in EVENTS
                and info.get("head_branch") == "main" and info.get("conclusion") == "success"):
            return art
    raise Unavailable(f"CI has no LibCarla prebuilt {name} (from a successful run on main)")


def sha256(path: Path) -> str:
    h = hashlib.sha256()
    with path.open("rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def check_manifest(prefix: Path, sha: str, abi: str, repository: str) -> dict:
    """Fails (ValueError) unless the prefix is the prebuilt asked for."""
    manifest = json.loads((prefix / "prebuilt.json").read_text())
    if manifest.get("format") != FORMAT:
        raise ValueError(f"prebuilt format {manifest.get('format')}, expected {FORMAT}")
    if manifest.get("carla_commit") != sha:
        raise ValueError(f"prebuilt is CARLA {manifest.get('carla_commit')}, expected {sha}")
    if manifest.get("abi", "") + "\n" != abi:
        raise ValueError(f"prebuilt ABI\n{manifest.get('abi')}\ndiffers from this compiler's\n{abi}")
    norm = lambda u: u.rstrip("/").removesuffix(".git")  # noqa: E731
    if norm(manifest.get("carla_repository", "")) != norm(repository):
        raise ValueError(f"prebuilt is from {manifest.get('carla_repository')}, not {repository}")
    return manifest


def verify_files(prefix: Path, manifest: dict) -> None:
    files = manifest.get("files") or {}
    present = {p.relative_to(prefix).as_posix() for p in prefix.rglob("*") if p.is_file()}
    present.discard("prebuilt.json")
    if present != set(files):
        extra, missing = sorted(present - set(files)), sorted(set(files) - present)
        raise ValueError(f"files differ from the manifest: extra {extra[:5]}, missing {missing[:5]}")
    for rel, want in files.items():
        if sha256(prefix / rel) != want:
            raise ValueError(f"{rel}: sha256 mismatch")


def download(repo: str, art: dict, dest: Path, sha: str, abi: str, repository: str) -> None:
    dest.parent.mkdir(parents=True, exist_ok=True)
    work = Path(tempfile.mkdtemp(prefix=".download-", dir=dest.parent))
    try:
        archive = work / "artifact.zip"
        size_mb = art.get("size_in_bytes", 0) / 1e6
        print(f"downloading {art['name']} ({size_mb:.0f} MB)", file=sys.stderr)
        with archive.open("wb") as f:
            gh("api", f"repos/{repo}/actions/artifacts/{art['id']}/zip", out=f)
        digest = art.get("digest") or ""
        if digest.startswith("sha256:") and sha256(archive) != digest[len("sha256:"):]:
            raise ValueError("the downloaded archive does not match its digest")
        unpacked = work / "prefix"
        with zipfile.ZipFile(archive) as z:
            for member in z.namelist():
                target = (unpacked / member).resolve()
                if not target.is_relative_to(unpacked.resolve()):
                    raise ValueError(f"archive member outside the prefix: {member}")
            z.extractall(unpacked)
        archive.unlink()
        manifest = check_manifest(unpacked, sha, abi, repository)
        verify_files(unpacked, manifest)
        try:
            unpacked.rename(dest)
        except OSError:
            if not (dest / "prebuilt.json").exists():  # not a concurrent download
                raise
    finally:
        shutil.rmtree(work, ignore_errors=True)


def cache_root() -> Path:
    base = os.environ.get("XDG_CACHE_HOME") or str(Path.home() / ".cache")
    return Path(base) / "typesafe_carla" / "libcarla-prebuilt"


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("ref", help="CARLA branch, tag or commit SHA")
    ap.add_argument("dest", nargs="?", type=Path,
                    help="where to unpack (default: the user's cache, keyed by name)")
    ap.add_argument("--repository", default="https://github.com/carla-simulator/carla",
                    help="the CARLA repository <ref> is in (prebuilts are only built from "
                         "carla-simulator/carla)")
    ap.add_argument("--artifact-repo", default=ARTIFACT_REPO,
                    help="the GitHub repository whose CI publishes prebuilts")
    ap.add_argument("--print-name", action="store_true",
                    help="only print the artifact name for this machine and exit")
    args = ap.parse_args(argv)

    try:
        sha = resolve(args.ref, args.repository)
        abi = abi_fingerprint()
        name = artifact_name(sha, abi)
        if args.print_name:
            print(name)
            return 0
        norm = args.repository.rstrip("/").removesuffix(".git")
        if norm != "https://github.com/carla-simulator/carla":
            raise Unavailable(f"CI builds prebuilts of carla-simulator/carla only, not {norm}")
        dest = args.dest or cache_root() / name
        if (dest / "prebuilt.json").exists():
            check_manifest(dest, sha, abi, args.repository)
            print(f"LibCarla prebuilt {name} (cached)", file=sys.stderr)
        else:
            art = find_artifact(args.artifact_repo, name)
            download(args.artifact_repo, art, dest, sha, abi, args.repository)
            print(f"LibCarla prebuilt {name} -> {dest}", file=sys.stderr)
        print(dest)
        return 0
    except Unavailable as e:
        print(f"no LibCarla prebuilt: {e}", file=sys.stderr)
        return 2
    except (ValueError, OSError, subprocess.CalledProcessError, zipfile.BadZipFile) as e:
        print(f"LibCarla prebuilt: {e}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())
