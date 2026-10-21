"""
`le doctor`: checks a project's build prerequisites up front and names each
missing one with how to get it, instead of leaving it to fail partway
through CMake's configure. A project on a release bundle needs only git,
tar and ssh-keygen; a source build needs the toolchain and the development
packages docs/BUILD.md lists - unless it builds with le's own toolchain
(`le toolchain`), which needs only an x86_64 host with CA certificates.
`le install` runs the source-build checks itself before a project's first
configure.
"""

import os
import platform
import re
import shutil
import subprocess
from dataclasses import dataclass
from pathlib import Path
from typing import Callable, Dict, List, Optional, Sequence, Tuple

from le import openssh, python_env, toolchain as toolchains

GIT_MINIMUM = (2, 34)
CMAKE_MINIMUM = (3, 25)  # the superbuild's cmake_minimum_required
SWIG_MINIMUM = (4, 0)
GCC_MINIMUM = 13
CLANG_MINIMUM = 17

# Package names per distribution family, for the fix hints.
PACKAGES: Dict[str, Tuple[str, str]] = {  # check name -> (apt, dnf)
    "git": ("git", "git"),
    "tar": ("tar", "tar"),
    "ssh-keygen": ("openssh-client", "openssh-clients"),
    "C++23 compiler": ("g++-13", "gcc-toolset-13-gcc-c++"),
    "cmake": ("cmake", "cmake"),
    "make": ("make", "make"),
    "bison": ("bison", "bison"),
    "swig": ("swig", "swig"),
    "Python": ("python3.11", "python3.11"),
    "Boost headers": ("libboost-dev", "boost-devel"),
    "zlib headers": ("zlib1g-dev", "zlib-devel"),
    "oneTBB": ("libtbb-dev", "tbb-devel"),
    "Tcl headers": ("tcl-dev", "tcl-devel"),
    "readline": ("libreadline-dev", "readline-devel"),
    "X11 headers": ("libx11-dev libxrandr-dev libxinerama-dev libxcursor-dev libxi-dev",
                    "libX11-devel libXrandr-devel libXinerama-devel libXcursor-devel libXi-devel"),
    "OpenGL headers": ("libgl-dev", "mesa-libGL-devel"),
    "Python venv": ("python3-venv", "python3"),
    "ccache": ("ccache", "ccache"),
    "CA certificates": ("ca-certificates", "ca-certificates"),
}

# Headers a compile probe includes, with any extra include directories to try.
HEADERS: Dict[str, Tuple[str, Sequence[str]]] = {
    "Boost headers": ("#include <boost/version.hpp>\n", ()),
    "oneTBB": ("#include <oneapi/tbb/version.h>\n", ()),
    "zlib headers": ("#include <zlib.h>\n", ()),  # the vendored LEF/DEF parsers' zlib readers
    "Tcl headers": ("#include <tcl.h>\n", ("/usr/include/tcl8.6", "/usr/include/tcl")),
    "readline": ("#include <cstdio>\n#include <readline/readline.h>\n#include <readline/history.h>\n", ()),
    "X11 headers": (
        "#include <X11/Xlib.h>\n#include <X11/extensions/Xrandr.h>\n#include <X11/extensions/Xinerama.h>\n"
        "#include <X11/Xcursor/Xcursor.h>\n#include <X11/extensions/XInput2.h>\n",
        (),
    ),
    "OpenGL headers": ("#include <GL/gl.h>\n", ()),
}

CXX23_PROBE = """\
#include <expected>
#if defined(__clang__)
#  if __clang_major__ < %d
#    error "Clang too old"
#  endif
#elif defined(__GNUC__) && __GNUC__ < %d
#  error "GCC too old"
#endif
int main() { std::expected<int, int> e = 0; return *e; }
""" % (CLANG_MINIMUM, GCC_MINIMUM)


@dataclass
class Check:
    name: str
    ok: bool
    detail: str = ""  # what was found, or what's wrong
    required: bool = True
    package: Optional[str] = None  # PACKAGES key for the fix, if not the name


class Probe:
    """The system lookups the checks make; tests replace it."""

    def which(self, name: str) -> Optional[str]:
        return shutil.which(name)

    def run(self, command: List[str], stdin: Optional[str] = None) -> Tuple[int, str]:
        try:
            result = subprocess.run(command, input=stdin, capture_output=True, text=True, timeout=120)
        except (OSError, subprocess.TimeoutExpired) as e:
            return 1, str(e)
        return result.returncode, result.stdout + result.stderr

    def os_release(self) -> str:
        path = Path("/etc/os-release")
        return path.read_text() if path.is_file() else ""

    def base_python(self) -> Tuple[str, Tuple[int, int]]:
        return python_env.base_python()

    def machine(self) -> str:
        return platform.machine()

    def ca_store(self) -> Optional[str]:
        return toolchains.ca_store()

    def signs(self, ssh_keygen: str) -> bool:
        return openssh.supports_signatures(ssh_keygen)


def _version(text: str) -> Optional[Tuple[int, ...]]:
    found = re.search(r"(\d+)\.(\d+)(?:\.(\d+))?", text)
    return tuple(int(p) for p in found.groups() if p is not None) if found else None


def _dotted(version: Sequence[int]) -> str:
    return ".".join(map(str, version))


def _tool(probe: Probe, name: str, minimum: Optional[Tuple[int, int]] = None, version_flag: str = "--version",
          required: bool = True) -> Check:
    path = probe.which(name)
    label = f"{name} >= {_dotted(minimum)}" if minimum else name
    if path is None:
        return Check(label, False, "not found on PATH", required, name)
    if minimum is None:
        return Check(label, True, path, required, name)
    code, out = probe.run([path, version_flag])
    version = _version(out) if code == 0 else None
    if version is None:
        return Check(label, False, f"{path}: couldn't read its version", required, name)
    if version[:2] < minimum:
        return Check(label, False, f"{path} is {_dotted(version)}", required, name)
    return Check(label, True, f"{_dotted(version)} ({path})", required, name)


def compiler(cmake_args: Sequence[str] = ()) -> Optional[str]:
    """The C++ compiler CMake will pick: CMAKE_CXX_COMPILER in cmake_args, else $CXX, else c++."""
    for arg in cmake_args:
        key, _, value = arg.partition("=")
        if key.split(":", 1)[0] == "-DCMAKE_CXX_COMPILER":
            return value
    return os.environ.get("CXX") or shutil.which("c++") or shutil.which("g++") or shutil.which("clang++")


def _compiles(probe: Probe, cxx: str, source: str, include_dirs: Sequence[str] = ()) -> Tuple[bool, str]:
    for extra in [None, *include_dirs]:
        command = [cxx, "-std=c++23", "-fsyntax-only", "-x", "c++", "-"] + ([f"-I{extra}"] if extra else [])
        code, out = probe.run(command, stdin=source)
        if code == 0:
            return True, out
    return False, out


def _ssh_keygen(probe: Probe) -> Check:
    """An ssh-keygen that can check signatures (-Y, OpenSSH 8.1+): the host's,
    or le's own, which it makes when it can (x86_64 with CA certificates)."""
    label = "ssh-keygen (OpenSSH 8.1+)"
    path = probe.which("ssh-keygen")
    if path is not None and probe.signs(path):
        return Check(label, True, path, package="ssh-keygen")
    why = "not found" if path is None else f"{path} has no -Y (older than OpenSSH 8.1)"
    if probe.machine() in toolchains.PIXI_SHA256 and probe.ca_store() is not None:
        return Check(label, True, f"{why}; le makes its own on first use", package="ssh-keygen")
    return Check(label, False, why, package="ssh-keygen")


def basic_checks(probe: Probe) -> List[Check]:
    """What every project needs: fetching and checking signed sources."""
    return [
        _tool(probe, "git", GIT_MINIMUM),
        _tool(probe, "tar"),
        _ssh_keygen(probe),
    ]


def source_build_checks(probe: Probe, cmake_args: Sequence[str] = (), cxx: Optional[str] = None) -> List[Check]:
    """What building Layout Engine from source needs."""
    checks = [
        _tool(probe, "cmake", CMAKE_MINIMUM),
        _tool(probe, "make"),
        _tool(probe, "bison"),
        _tool(probe, "swig", SWIG_MINIMUM, version_flag="-version"),
    ]

    cxx = cxx or compiler(cmake_args)
    if cxx is None:
        checks.append(Check("C++23 compiler", False, "no c++, g++ or clang++ on PATH (or set CXX)"))
        checks += [Check(name, False, "needs a C++ compiler to check") for name in HEADERS]
    else:
        ok, out = _compiles(probe, cxx, CXX23_PROBE)
        why = "too old" if "too old" in out else "doesn't compile C++23 (std::expected)"
        checks.append(Check(f"C++23 compiler (GCC >= {GCC_MINIMUM} or Clang >= {CLANG_MINIMUM})", ok,
                            cxx if ok else f"{cxx} {why}", package="C++23 compiler"))
        if ok:
            for name, (source, include_dirs) in HEADERS.items():
                found, _ = _compiles(probe, cxx, source, include_dirs)
                checks.append(Check(name, found, "" if found else "headers not found"))

    own_python = next((a.partition("=")[2] for a in cmake_args if a.partition("=")[0].split(":", 1)[0] == "-DPython3_EXECUTABLE"), None)
    if own_python:
        code, _ = probe.run([own_python, "-c", "import sys, jinja2, click; sys.exit(sys.version_info[:2] < (3, 11))"])
        checks.append(Check("Python", code == 0, own_python if code == 0 else
                            f"{own_python} (from cmake_args) isn't Python >= 3.11 with jinja2 and click"))
    else:
        try:
            python, version = probe.base_python()
            checks.append(Check("Python", True, f"{_dotted(version)} ({python}); le installs codegen's packages"))
            code, _ = probe.run([python, "-c", "import venv, ensurepip"])
            checks.append(Check("Python venv", code == 0, "" if code == 0 else
                                f"{python} has no venv/ensurepip: le can't make codegen's environment, so it needs jinja2 "
                                "and click installed for it", required=False))
        except python_env.PythonEnvError:
            checks.append(Check("Python", False, "no Python >= 3.11, < 3.14 on PATH"))

    ccache = _tool(probe, "ccache", required=False)
    ccache.name = "ccache (faster rebuilds)"
    checks.append(ccache)
    return checks


def toolchain_checks(probe: Probe) -> List[Check]:
    """What making le's toolchain needs; it brings everything a source build needs itself."""
    machine = probe.machine()
    store = probe.ca_store()
    return [
        Check("x86_64 Linux", machine in toolchains.PIXI_SHA256, machine, package="none"),
        Check("CA certificates", store is not None, store or "none found (or set SSL_CERT_FILE)"),
    ]


def distro_family(os_release: str) -> Optional[str]:
    """'apt', 'dnf' or None, from /etc/os-release's ID and ID_LIKE."""
    ids = " ".join(re.findall(r"^(?:ID|ID_LIKE)=\"?([^\"\n]*)", os_release, re.M)).split()
    if any(i in ("debian", "ubuntu") for i in ids):
        return "apt"
    if any(i in ("rhel", "fedora", "centos", "rocky", "almalinux") for i in ids):
        return "dnf"
    return None


def fix(check: Check, family: Optional[str]) -> str:
    packages = PACKAGES.get(check.package or check.name)
    if packages is None:
        return ""
    if family == "apt":
        return f"apt install {packages[0]}"
    if family == "dnf":
        return f"dnf install {packages[1]}"
    return f"install {check.package or check.name} (Debian: {packages[0]}; RHEL: {packages[1]})"


def report(checks: List[Check], probe: Probe, out: Callable[[str], None] = print) -> bool:
    """Prints one line per check; returns whether every required one passed."""
    family = distro_family(probe.os_release())
    width = max(len(c.name) for c in checks)
    for c in checks:
        status = "ok" if c.ok else "missing" if c.required else "optional"
        line = f"{status:8} {c.name:{width}}"
        if c.detail:
            line += f"  {c.detail}"
        if not c.ok:
            hint = fix(c, family)
            if hint:
                line += f"  -> {hint}"
        out(line.rstrip())
    return not any(c.required and not c.ok for c in checks)


def run(source_build: bool, cmake_args: Sequence[str] = (), probe: Optional[Probe] = None,
        out: Callable[[str], None] = print, toolchain: bool = False) -> bool:
    """Runs and reports the checks a project needs (a source build with le's
    toolchain needs only toolchain_checks); returns whether it can build."""
    probe = probe or Probe()
    checks = basic_checks(probe)
    if source_build and toolchain:
        checks += toolchain_checks(probe)
    elif source_build:
        checks += source_build_checks(probe, cmake_args)
    ok = report(checks, probe, out)
    if not ok and source_build and not toolchain:
        out("Or let le provide the whole build toolchain, without root: `le toolchain`.")
    return ok
