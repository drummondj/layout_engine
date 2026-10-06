"""
Building a project: Layout Engine from source with the project's extensions
(LE_EXTENSION_DIRS), configured in .le/build and installed as a bundle in
.le/bundle. Fetched dependencies' sources are shared between projects in
the cache (or LE_DEPS_DIR), and ccache is used when it's on PATH.
"""

import json
import os
import re
import shutil
import subprocess
from pathlib import Path
from typing import List, Optional, Tuple

from le import sources


class BuildError(Exception):
    pass


def layout_engine_versions(source_dir: Path) -> Tuple[str, int]:
    """(project version, extension API version) declared by a Layout Engine source tree."""
    cmake = (source_dir / "CMakeLists.txt").read_text()
    version = re.search(r"^project\(layout_engine_backend VERSION (\d+\.\d+\.\d+)", cmake, re.M)
    api = re.search(r"^set\(LE_EXTENSION_API_VERSION (\d+)\)", cmake, re.M)
    if not version or not api:
        raise BuildError(f"{source_dir} doesn't look like a Layout Engine source tree with extension support (0.2 or later)")
    return version.group(1), int(api.group(1))


def _run(command: List[str], what: str) -> None:
    print(f"le: {what}", flush=True)
    result = subprocess.run(command)
    if result.returncode != 0:
        raise BuildError(f"{what} failed (exit {result.returncode}): {' '.join(command)}")


def build(
    le_source: Path,
    extension_dirs: List[Path],
    state_dir: Path,
    build_type: str,
    jobs: Optional[int],
    layout_engine_version: str,
    startup: Optional[Path],
) -> Path:
    """Configures, builds and installs; returns the bundle directory."""
    build_dir = state_dir / "build"
    bundle_dir = state_dir / "bundle"
    configure = [
        "cmake",
        "-S", str(le_source),
        "-B", str(build_dir),
        f"-DCMAKE_BUILD_TYPE={build_type}",
        "-DLE_EXTENSION_DIRS=" + ";".join(str(d) for d in extension_dirs),
        f"-DFETCHCONTENT_BASE_DIR={os.environ.get('LE_DEPS_DIR') or sources.cache_dir() / 'deps' / layout_engine_version}",
    ]
    if shutil.which("ccache"):
        configure += ["-DCMAKE_C_COMPILER_LAUNCHER=ccache", "-DCMAKE_CXX_COMPILER_LAUNCHER=ccache"]
    _run(configure, "configuring")
    _run(["cmake", "--build", str(build_dir), "--target", "le_shell", "le_tcl", "-j", str(jobs or os.cpu_count() or 2)], "building")
    if bundle_dir.exists():
        shutil.rmtree(bundle_dir)  # so a removed extension's files don't linger
    _run(["cmake", "--install", str(build_dir), "--component", "bundle", "--prefix", str(bundle_dir)], "installing")

    if startup is not None:
        index_path = bundle_dir / "extensions.json"
        index = json.loads(index_path.read_text())
        index["startup"] = os.path.relpath(startup, bundle_dir)
        index_path.write_text(json.dumps(index, indent=2) + "\n")
    return bundle_dir
