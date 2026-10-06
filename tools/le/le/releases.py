"""
Prebuilt Layout Engine release bundles, for projects with no compiled
extensions: downloaded with their detached SSH signature, checked against
Layout Engine's release key, and unpacked once into the cache
(<cache>/releases/<tag>/).

LE_RELEASES_URL overrides where they come from (a template with {repo},
{tag} and {asset}; default GitHub releases), mainly for tests.
"""

import hashlib
import json
import os
import shutil
import subprocess
import tarfile
import urllib.request
from pathlib import Path
from typing import Optional, Tuple

from le import sources

SIGNATURE_NAMESPACE = "layout_engine-release"


class ReleaseError(Exception):
    pass


def asset_name(tag: str) -> str:
    return f"layout_engine-linux-x86_64-{tag}.tar.gz"


def release_url(repo: str, tag: str, asset: str) -> str:
    template = os.environ.get("LE_RELEASES_URL", "https://github.com/{repo}/releases/download/{tag}/{asset}")
    return template.format(repo=repo, tag=tag, asset=asset)


def _download(url: str, dest: Path) -> None:
    dest.parent.mkdir(parents=True, exist_ok=True)
    partial = dest.with_suffix(dest.suffix + ".part")
    try:
        with urllib.request.urlopen(url) as response, open(partial, "wb") as out:
            shutil.copyfileobj(response, out)
    except OSError as e:
        partial.unlink(missing_ok=True)
        raise ReleaseError(f"couldn't download {url}: {e}") from e
    partial.replace(dest)


def _sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with open(path, "rb") as f:
        for block in iter(lambda: f.read(1 << 20), b""):
            digest.update(block)
    return digest.hexdigest()


def _verify(tarball: Path, signature: Path, allowed_signers: Path, principal: str) -> None:
    if shutil.which("ssh-keygen") is None:
        raise ReleaseError("checking the release signature needs ssh-keygen (OpenSSH's client tools)")
    with open(tarball, "rb") as data:
        result = subprocess.run(
            ["ssh-keygen", "-Y", "verify", "-f", str(allowed_signers), "-I", principal, "-n", SIGNATURE_NAMESPACE, "-s", str(signature)],
            stdin=data,
            capture_output=True,
            text=True,
        )
    if result.returncode != 0:
        raise ReleaseError(f"{tarball.name} isn't signed by Layout Engine's release key: {(result.stderr or result.stdout).strip()}")


def fetch(repo: str, tag: str, allowed_signers: Path, principal: str, expected_sha256: Optional[str]) -> Tuple[Path, str]:
    """
    The unpacked, verified release bundle for `tag`, and its tarball's sha256.
    A cached download is reused only if it still matches `expected_sha256`
    (from the lock) when one is given.
    """
    root = sources.cache_dir() / "releases" / tag
    tarball = root / asset_name(tag)
    signature = root / (asset_name(tag) + ".sig")
    bundle = root / "bundle"
    if not tarball.is_file() or not signature.is_file():
        _download(release_url(repo, tag, asset_name(tag)), tarball)
        _download(release_url(repo, tag, asset_name(tag) + ".sig"), signature)
    _verify(tarball, signature, allowed_signers, principal)
    sha256 = _sha256(tarball)
    if expected_sha256 is not None and sha256 != expected_sha256:
        raise ReleaseError(f"release {tag} has sha256 {sha256}, but the lock has {expected_sha256} - run `le update` to accept it")

    marker = bundle / ".le_sha256"
    if not (marker.is_file() and marker.read_text().strip() == sha256):
        if bundle.exists():
            shutil.rmtree(bundle)
        bundle.mkdir(parents=True)
        with tarfile.open(tarball) as archive:
            try:
                archive.extractall(bundle, filter="data")
            except TypeError:  # Python before 3.11.4 has no extraction filters
                archive.extractall(bundle)
        marker.write_text(sha256 + "\n")
    if not (bundle / "extensions.json").is_file():
        raise ReleaseError(f"release {tag} predates extension support (no extensions.json) - use Layout Engine 0.3 or later")
    return bundle, sha256


def bundle_versions(bundle: Path) -> Tuple[str, int]:
    """(layout_engine version, extension API) a release bundle declares."""
    index = json.loads((bundle / "extensions.json").read_text())
    return index["layout_engine"], int(index["extension_api"])
