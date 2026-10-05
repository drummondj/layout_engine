"""
Fetching and checking sources: bare mirrors of GitHub repos in the cache, tag
and commit resolution, SSH signature checks, and exporting one commit's tree.

Environment overrides (mainly for tests):
- LE_CACHE_DIR: the cache (default ~/.layout_engine/cache)
- LE_GITHUB_URL: a URL template with {repo} (default https://github.com/{repo}.git)
"""

import os
import re
import shutil
import subprocess
from pathlib import Path
from typing import List, Optional, Tuple

_GOOD_SIGNATURE = re.compile(r'Good "git" signature for (\S+) with \S+ key (SHA256:\S+)')


class SourceError(Exception):
    pass


def cache_dir() -> Path:
    return Path(os.environ.get("LE_CACHE_DIR", Path.home() / ".layout_engine" / "cache"))


def github_url(repo: str) -> str:
    return os.environ.get("LE_GITHUB_URL", "https://github.com/{repo}.git").format(repo=repo)


def git(*args: str, cwd: Optional[Path] = None, check: bool = True) -> subprocess.CompletedProcess:
    result = subprocess.run(["git", *args], cwd=cwd, capture_output=True, text=True)
    if check and result.returncode != 0:
        raise SourceError(f"git {' '.join(args)} failed: {result.stderr.strip() or result.stdout.strip()}")
    return result


def mirror(repo: str) -> Path:
    """A bare mirror of GitHub `repo` in the cache, fetched up to date."""
    path = cache_dir() / "git" / f"{repo}.git"
    if path.is_dir():
        git("remote", "update", "--prune", cwd=path)
    else:
        path.parent.mkdir(parents=True, exist_ok=True)
        git("clone", "--mirror", "--quiet", github_url(repo), str(path))
    return path


def commit_of(repo_dir: Path, ref: str) -> str:
    result = git("rev-parse", "--verify", "--quiet", f"{ref}^{{commit}}", cwd=repo_dir, check=False)
    if result.returncode != 0:
        raise SourceError(f"{ref} not found in {repo_dir.name}")
    return result.stdout.strip()


def write_allowed_signers(path: Path, principal: str, keys: List[str]) -> Path:
    """An allowed_signers file trusting `keys` (SSH public key lines) for git signatures by `principal`."""
    lines = []
    for key in keys:
        fields = key.split()
        if len(fields) < 2:
            raise SourceError(f"{key!r} isn't an SSH public key")
        lines.append(f'{principal} namespaces="git" {fields[0]} {fields[1]}')
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text("\n".join(lines) + "\n")
    return path


def verify(repo_dir: Path, ref: str, is_tag: bool, allowed_signers: Path, principal: str) -> str:
    """
    Checks `ref` (a tag, or a commit for a revision pin) carries a good SSH
    signature by `principal` under `allowed_signers`; returns the signing
    key's fingerprint.
    """
    command = ["verify-tag", ref] if is_tag else ["verify-commit", ref]
    result = git("-c", f"gpg.ssh.allowedSignersFile={allowed_signers}", *command, cwd=repo_dir, check=False)
    output = result.stderr + result.stdout
    match = _GOOD_SIGNATURE.search(output)
    if result.returncode != 0 or not match:
        what = f"tag {ref}" if is_tag else f"commit {ref}"
        detail = output.strip().splitlines()[-1] if output.strip() else "no signature"
        raise SourceError(f"{repo_dir.name}: {what} isn't signed by a trusted key of {principal} ({detail})")
    if match.group(1) != principal:
        raise SourceError(f"{repo_dir.name}: {ref} is signed by {match.group(1)}, not {principal}")
    return match.group(2)


def export(repo_dir: Path, commit: str, dest: Path) -> None:
    """Writes `commit`'s tree to `dest` (replacing it), recording the commit in dest/.le_rev."""
    marker = dest / ".le_rev"
    if marker.is_file() and marker.read_text().strip() == commit:
        return
    if dest.exists():
        shutil.rmtree(dest)
    dest.mkdir(parents=True)
    archive = subprocess.Popen(["git", "archive", "--format=tar", commit], cwd=repo_dir, stdout=subprocess.PIPE)
    untar = subprocess.run(["tar", "-x", "-C", str(dest)], stdin=archive.stdout, capture_output=True, text=True)
    archive.stdout.close()
    if archive.wait() != 0 or untar.returncode != 0:
        raise SourceError(f"couldn't export {commit} from {repo_dir.name}: {untar.stderr.strip()}")
    marker.write_text(commit + "\n")


def resolve(repo_dir: Path, tag: Optional[str], rev: Optional[str]) -> Tuple[str, str, bool]:
    """(commit, the ref to verify, whether it's a tag) for a tag or revision pin."""
    if tag is not None:
        return commit_of(repo_dir, f"refs/tags/{tag}"), f"refs/tags/{tag}", True
    commit = commit_of(repo_dir, rev)
    return commit, commit, False
