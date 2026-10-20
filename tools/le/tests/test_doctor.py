"""`le doctor`'s checks, against a stubbed system."""

import unittest
from typing import Dict, List, Optional, Tuple

from le import doctor, python_env

UBUNTU = 'NAME="Ubuntu"\nID=ubuntu\nID_LIKE=debian\n'
ROCKY = 'NAME="Rocky Linux"\nID="rocky"\nID_LIKE="rhel centos fedora"\n'


class FakeProbe(doctor.Probe):
    """A machine with every prerequisite, which each test then takes things away from."""

    def __init__(self):
        self.tools: Dict[str, str] = {name: f"/usr/bin/{name}" for name in ("git", "tar", "ssh-keygen", "cmake", "make", "bison", "swig", "ccache")}
        self.versions = {"git": "git version 2.43.0", "cmake": "cmake version 3.28.3", "swig": "\nSWIG Version 4.2.0\n"}
        self.release = UBUNTU
        self.compiler_error: Optional[str] = None  # the C++23 probe's error, if it fails
        self.missing_headers: List[str] = []  # header names a compile can't find
        self.include_dirs_needed: Dict[str, str] = {}  # header -> the -I it needs
        self.python: Optional[Tuple[str, Tuple[int, int]]] = ("/usr/bin/python3.12", (3, 12))
        self.python_ok = True  # the base Python has venv; a cmake_args Python has the packages
        self.commands: List[List[str]] = []

    def which(self, name):
        return self.tools.get(name)

    def run(self, command, stdin=None):
        self.commands.append(command)
        name = command[0].rsplit("/", 1)[-1]
        if name in self.versions:
            return 0, self.versions[name]
        if stdin is not None:  # a compile probe
            if "<expected>" in stdin:
                return (1, f"<stdin>:1: error: {self.compiler_error}") if self.compiler_error else (0, "")
            for header in self.missing_headers:
                if header in stdin:
                    return 1, f"fatal error: {header}: No such file or directory"
            for header, include in self.include_dirs_needed.items():
                if header in stdin and f"-I{include}" not in command:
                    return 1, f"fatal error: {header}: No such file or directory"
            return 0, ""
        if "-c" in command:  # a Python probe
            return (0, "") if self.python_ok else (1, "ModuleNotFoundError")
        return 0, ""

    def os_release(self):
        return self.release

    def base_python(self):
        if self.python is None:
            raise python_env.PythonEnvError("none")
        return self.python


class TestDoctor(unittest.TestCase):
    def setUp(self):
        self.probe = FakeProbe()

    def run_doctor(self, source_build=True, cmake_args=()):
        self.lines = []
        return doctor.run(source_build, cmake_args, self.probe, out=self.lines.append)

    def line(self, name):
        return next(line for line in self.lines if line.split()[1:2] == name.split()[:1] and name in line)

    def test_everything_present_passes_with_one_line_each(self):
        self.assertTrue(self.run_doctor())
        self.assertTrue(all(line.startswith("ok ") for line in self.lines), self.lines)
        for name in ("git", "cmake", "swig", "C++23", "Boost", "zlib", "oneTBB", "Tcl", "readline", "X11", "OpenGL", "Python", "ccache"):
            self.assertTrue(any(name in line for line in self.lines), name)

    def test_a_release_project_needs_only_git_tar_and_ssh_keygen(self):
        self.probe.tools.pop("cmake")
        self.probe.compiler_error = "GCC too old"
        self.assertTrue(self.run_doctor(source_build=False))
        self.assertEqual([line.split()[1] for line in self.lines], ["git", "tar", "ssh-keygen"])

    def test_missing_and_too_old_tools_name_the_fix(self):
        self.probe.tools.pop("swig")
        self.probe.versions["git"] = "git version 2.30.1"
        self.assertFalse(self.run_doctor())
        self.assertIn("missing  git >= 2.34", self.line("git"))
        self.assertIn("is 2.30.1", self.line("git"))
        self.assertIn("-> apt install git", self.line("git"))
        self.assertIn("not found on PATH  -> apt install swig", self.line("swig"))

    def test_a_compiler_without_cxx23_is_reported_and_headers_go_unchecked(self):
        self.probe.compiler_error = "GCC too old"
        self.assertFalse(self.run_doctor(cmake_args=["-DCMAKE_CXX_COMPILER=/opt/gcc11/bin/g++"]))
        self.assertIn("/opt/gcc11/bin/g++ too old", self.line("C++23"))
        self.assertFalse(any("oneTBB" in line for line in self.lines))

    def test_missing_headers_are_named(self):
        self.probe.missing_headers = ["oneapi/tbb/version.h", "X11/extensions/Xinerama.h"]
        self.assertFalse(self.run_doctor())
        self.assertIn("-> apt install libtbb-dev", self.line("oneTBB"))
        self.assertTrue(self.line("X11").startswith("missing"))
        self.assertTrue(self.line("Boost").startswith("ok"))

    def test_tcl_headers_are_found_in_their_versioned_directory(self):
        self.probe.include_dirs_needed = {"tcl.h": "/usr/include/tcl8.6"}
        self.assertTrue(self.run_doctor())

    def test_rhel_gets_dnf_packages_and_the_rootless_script(self):
        self.probe.release = ROCKY
        self.probe.missing_headers = ["readline/readline.h"]
        self.assertFalse(self.run_doctor())
        self.assertIn("-> dnf install readline-devel", self.line("readline"))
        self.assertIn("rocky8-bootstrap.sh", self.lines[-1])

    def test_an_unknown_distribution_gets_both_package_names(self):
        self.probe.release = "ID=arch\n"
        self.probe.tools.pop("bison")
        self.run_doctor()
        self.assertIn("Debian: bison; RHEL: bison", self.line("bison"))

    def test_missing_ccache_is_optional(self):
        self.probe.tools.pop("ccache")
        self.assertTrue(self.run_doctor())
        self.assertTrue(self.line("ccache").startswith("optional"))

    def test_python(self):
        self.probe.python = None
        self.assertFalse(self.run_doctor())
        self.assertIn("no Python >= 3.11", self.line("Python"))
        # Without venv, le falls back to the base Python's own packages: a warning only.
        self.probe.python = ("/usr/bin/python3.12", (3, 12))
        self.probe.python_ok = False
        self.assertTrue(self.run_doctor())
        self.assertTrue(self.line("Python venv").startswith("optional"))

    def test_a_python_from_cmake_args_must_have_the_packages(self):
        self.probe.python_ok = False
        self.assertFalse(self.run_doctor(cmake_args=["-DPython3_EXECUTABLE=/opt/py/bin/python3"]))
        self.assertIn("/opt/py/bin/python3 (from cmake_args) isn't Python >= 3.11 with jinja2 and click", self.line("Python"))
        self.assertFalse(any("venv" in line for line in self.lines), "le doesn't make an environment then")


if __name__ == "__main__":
    unittest.main()
