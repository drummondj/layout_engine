"""
The ssh-keygen `le` checks signatures with: `ssh-keygen -Y` (OpenSSH 8.1 and
later) verifies tags, commits and release bundles. A host whose ssh-keygen
lacks it - stock Rocky 8 has OpenSSH 8.0 - or that has none gets
conda-forge's OpenSSH instead, in <cache>/openssh/<key>/, made once by le's
pinned pixi from le's own locked manifest (openssh/pixi.toml, pixi.lock),
without root.
"""

import functools
import shutil
import subprocess
from pathlib import Path
from typing import Optional

from le import toolchain

MANIFEST = Path(__file__).resolve().parent / "openssh"


class OpenSSHError(Exception):
    pass


@functools.lru_cache(maxsize=None)
def supports_signatures(ssh_keygen: str) -> bool:
    """Whether `ssh_keygen` has -Y: an older one says "unknown option -- Y"."""
    try:
        result = subprocess.run([ssh_keygen, "-Y"], capture_output=True, text=True, timeout=30)
    except (OSError, subprocess.TimeoutExpired):
        return False
    output = result.stdout + result.stderr
    return "-- Y" in output and "unknown option" not in output


def host_ssh_keygen() -> Optional[str]:
    """The host's ssh-keygen, if it can check signatures."""
    path = shutil.which("ssh-keygen")
    return path if path is not None and supports_signatures(path) else None


def ssh_keygen() -> str:
    """An ssh-keygen with -Y: the host's, else le's own, made first if needed."""
    host = host_ssh_keygen()
    if host is not None:
        return host
    try:
        made = toolchain.environment(MANIFEST, "openssh", "an OpenSSH that can check signatures (this machine's has no ssh-keygen -Y)")
    except toolchain.ToolchainError as e:
        raise OpenSSHError(f"checking signatures needs ssh-keygen from OpenSSH 8.1 or later, and {e}") from e
    path = made.prefix / "bin" / "ssh-keygen"
    if not path.is_file():
        raise OpenSSHError(f"{made.root} has no bin/ssh-keygen")
    return str(path)
