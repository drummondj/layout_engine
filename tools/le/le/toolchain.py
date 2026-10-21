"""
`le toolchain`: Layout Engine's build toolchain from conda-forge - the
compilers, cmake, make, bison, SWIG, Tcl, Python with codegen's packages and
every library a source build needs - made without root from Layout Engine's
own toolchain/pixi.toml and pixi.lock. A pinned pixi, checked by sha256, makes
it in <cache>/toolchain/<key>/, keyed by those two files, so every project
on the same toolchain shares one environment.

A project with `[build] toolchain = true` builds inside it: every cmake, make
and ctest runs through `pixi run`, with the environment's own
target-prefixed compilers (so CMake also finds its ar/ranlib/objdump, not
the host's) and CMake looking for libraries in it only.
"""

import hashlib
import io
import os
import platform
import shutil
import subprocess
import tarfile
import tempfile
import urllib.request
from dataclasses import dataclass
from pathlib import Path
from typing import List, Optional

from le import sources

PIXI_VERSION = "0.81.0"
PIXI_URL = "https://github.com/prefix-dev/pixi/releases/download/v{version}/pixi-{machine}-unknown-linux-musl.tar.gz"
PIXI_SHA256 = {"x86_64": "7aa3ec39aecceff9062fa2ed4d42cbaa0bdc25ddea727d048e061cf188d434f6"}
TARGET = "x86_64-conda-linux-gnu"  # the environment's compilers' prefix; toolchain/pixi.toml is locked for linux-64 only
READY = "le_ready"  # written into an environment's directory once it's installed

# Where distributions keep the CA certificates pixi needs to download.
CA_STORES = (
    "/etc/ssl/certs/ca-certificates.crt",  # Debian, Ubuntu
    "/etc/pki/tls/certs/ca-bundle.crt",  # RHEL, Rocky, Fedora
    "/etc/ssl/ca-bundle.pem",  # SUSE
    "/etc/ssl/cert.pem",  # Alpine, Arch
)


class ToolchainError(Exception):
    pass


@dataclass
class Toolchain:
    """An installed toolchain environment."""

    root: Path  # holds pixi.toml, pixi.lock and .pixi/envs/default
    pixi: Path
    key: str

    @property
    def prefix(self) -> Path:
        return self.root / ".pixi" / "envs" / "default"

    @property
    def python(self) -> Path:
        return self.prefix / "bin" / "python3"

    @property
    def ccache(self) -> Optional[Path]:
        ccache = self.prefix / "bin" / "ccache"
        return ccache if ccache.exists() else None

    def wrap(self, command: List[str]) -> List[str]:
        """`command` run inside the environment, activated."""
        return [str(self.pixi), "run", "--manifest-path", str(self.root / "pixi.toml"), "--frozen", "--", *command]

    def cmake_args(self) -> List[str]:
        """What configuring with this toolchain needs: its compilers, and its libraries only."""
        prefix = str(self.prefix)
        return [
            f"-DCMAKE_C_COMPILER={prefix}/bin/{TARGET}-gcc",
            f"-DCMAKE_CXX_COMPILER={prefix}/bin/{TARGET}-g++",
            f"-DCMAKE_PREFIX_PATH={prefix}",
            f"-DCMAKE_FIND_ROOT_PATH={prefix}",
            "-DCMAKE_FIND_ROOT_PATH_MODE_LIBRARY=ONLY",
            "-DCMAKE_FIND_ROOT_PATH_MODE_INCLUDE=ONLY",
            "-DCMAKE_FIND_ROOT_PATH_MODE_PACKAGE=ONLY",
            f"-DPython3_EXECUTABLE={self.python}",
        ]


def manifest(le_source: Path) -> Path:
    """Layout Engine's toolchain/ directory (pixi.toml and pixi.lock)."""
    directory = le_source / "toolchain"
    if not (directory / "pixi.toml").is_file() or not (directory / "pixi.lock").is_file():
        raise ToolchainError(f"{le_source} has no toolchain/pixi.toml and pixi.lock - this Layout Engine version predates `le toolchain`")
    return directory


def manifest_key(directory: Path) -> str:
    """An environment's key: its manifest and lock."""
    digest = hashlib.sha256((directory / "pixi.toml").read_bytes() + b"\0" + (directory / "pixi.lock").read_bytes())
    return digest.hexdigest()[:16]


def key(le_source: Path) -> str:
    return manifest_key(manifest(le_source))


def environment_dir(le_source: Path) -> Path:
    return sources.cache_dir() / "toolchain" / key(le_source)


def ca_store() -> Optional[str]:
    """The CA certificates pixi will use, if this host has any."""
    if os.environ.get("SSL_CERT_FILE") and Path(os.environ["SSL_CERT_FILE"]).is_file():
        return os.environ["SSL_CERT_FILE"]
    return next((path for path in CA_STORES if Path(path).is_file()), None)


def check_host() -> None:
    """What making the environment needs from the host, beyond git, tar and ssh-keygen."""
    machine = platform.machine()
    if machine not in PIXI_SHA256:
        raise ToolchainError(f"le toolchain supports {', '.join(PIXI_SHA256)} Linux only, not {machine}")
    if ca_store() is None:
        raise ToolchainError(
            "this machine has no CA certificates, which downloading the toolchain needs: install them "
            "(Debian/Ubuntu: apt install ca-certificates; RHEL/Rocky: dnf install ca-certificates), "
            "or point SSL_CERT_FILE at a PEM bundle"
        )


def _download(url: str) -> bytes:
    with urllib.request.urlopen(url, timeout=300) as response:
        return response.read()


def pixi_binary() -> Path:
    """The pinned pixi, downloaded into <cache>/pixi/<version>/ once and checked by sha256."""
    machine = platform.machine()
    if machine not in PIXI_SHA256:
        raise ToolchainError(f"le toolchain supports {', '.join(PIXI_SHA256)} Linux only, not {machine}")
    target = sources.cache_dir() / "pixi" / PIXI_VERSION / "pixi"
    if target.is_file():
        return target
    url = PIXI_URL.format(version=PIXI_VERSION, machine=machine)
    print(f"le: downloading pixi {PIXI_VERSION}", flush=True)
    try:
        archive = _download(url)
    except OSError as e:
        raise ToolchainError(f"couldn't download {url}: {e}") from e
    digest = hashlib.sha256(archive).hexdigest()
    if digest != PIXI_SHA256[machine]:
        raise ToolchainError(f"{url} has sha256 {digest}, not the pinned {PIXI_SHA256[machine]} - refusing to run it")
    target.parent.mkdir(parents=True, exist_ok=True)
    with tarfile.open(fileobj=io.BytesIO(archive), mode="r:gz") as tar:
        member = tar.extractfile("pixi")
        if member is None:
            raise ToolchainError(f"{url} has no pixi binary")
        data = member.read()
    descriptor, name = tempfile.mkstemp(prefix=".pixi-", dir=target.parent)
    os.close(descriptor)  # an executable still open for writing can't be run
    staging = Path(name)
    staging.write_bytes(data)
    staging.chmod(0o755)
    staging.replace(target)  # whole, so a concurrent `le` never runs half of it
    return target


def _install(pixi: Path, root: Path) -> None:
    result = subprocess.run([str(pixi), "install", "--frozen", "--manifest-path", str(root / "pixi.toml")])
    if result.returncode != 0:
        raise ToolchainError(f"making the environment in {root} failed (pixi install exit {result.returncode})")


def environment(directory: Path, kind: str, what: str) -> Toolchain:
    """The environment for the pixi.toml and pixi.lock in `directory`, made in
    <cache>/<kind>/<key> by the pinned pixi first if needed; `what` names it
    in the progress message."""
    env_key = manifest_key(directory)
    root = sources.cache_dir() / kind / env_key
    pixi = pixi_binary()
    made = Toolchain(root=root, pixi=pixi, key=env_key)
    if (root / READY).is_file() and made.prefix.is_dir():
        return made
    check_host()
    root.mkdir(parents=True, exist_ok=True)
    for name in ("pixi.toml", "pixi.lock"):
        shutil.copyfile(directory / name, root / name)
    print(f"le: making {what} in {root}", flush=True)
    _install(pixi, root)
    (root / READY).write_text(env_key + "\n")
    return made


def ensure(le_source: Path) -> Toolchain:
    """The toolchain environment for `le_source`'s toolchain/, made first if needed."""
    return environment(manifest(le_source), "toolchain", "the build toolchain (the first time takes a minute or two)")
