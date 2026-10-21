"""codegen's Python environment: its requirements, its cache key, reuse, the
project's override and the fallbacks - with venv creation stubbed."""

import contextlib
import io
import os
import tempfile
import unittest
from pathlib import Path
from unittest import mock

from le import build, python_env

PYPROJECT = """\
[tool.poetry.dependencies]
python = ">=3.11,<3.14"
jinja2 = "^3.1.4"
click = "^8.4.2"

[tool.poetry.group.dev.dependencies]
coverage = "^7.6.10"
"""


class TestRequirements(unittest.TestCase):
    def test_poetry_constraints_become_pip_requirements(self):
        for spec, expected in (
            ("^3.1.4", "x>=3.1.4,<4"),
            ("^0.2.3", "x>=0.2.3,<0.3"),
            ("^0.0.3", "x>=0.0.3,<0.0.4"),
            ("~1.2.3", "x>=1.2.3,<1.3"),
            ("~1", "x>=1,<2"),
            ("~=1.2", "x~=1.2"),
            ("1.2.3", "x==1.2.3"),
            (">=1.0, <2.0", "x>=1.0,<2.0"),
            ("*", "x"),
            ({"version": "^2.0"}, "x>=2.0,<3"),
        ):
            self.assertEqual(python_env.pip_requirement("x", spec), expected, spec)

    def test_read_from_codegen_pyproject_without_python_or_dev_dependencies(self):
        with tempfile.TemporaryDirectory() as tmp:
            source = Path(tmp)
            self.assertIsNone(python_env.requirements(source))
            (source / "codegen").mkdir()
            (source / "codegen" / "pyproject.toml").write_text(PYPROJECT)
            self.assertEqual(python_env.requirements(source), ["click>=8.4.2,<9", "jinja2>=3.1.4,<4"])


class TestCodegenPython(unittest.TestCase):
    def setUp(self):
        tmp = tempfile.TemporaryDirectory()
        self.addCleanup(tmp.cleanup)
        self.root = Path(tmp.name)
        env = mock.patch.dict(os.environ, {"LE_CACHE_DIR": str(self.root / "cache")})  # never the user's cache
        env.start()
        self.addCleanup(env.stop)
        self.source = self.root / "layout_engine"
        (self.source / "codegen").mkdir(parents=True)
        (self.source / "codegen" / "pyproject.toml").write_text(PYPROJECT)
        self.created = []
        self.fail_create = False
        self.base_has_packages = False
        for name, replacement in (
            ("base_python", lambda: ("/usr/bin/python3.12", (3, 12))),
            ("_create", self.fake_create),
            ("has_packages", lambda python, reqs: self.base_has_packages),
        ):
            patcher = mock.patch.object(python_env, name, replacement)
            patcher.start()
            self.addCleanup(patcher.stop)

    def fake_create(self, base, directory, reqs):
        self.created.append((base, directory, reqs))
        if self.fail_create:
            raise python_env.PythonEnvError("pip: no network")
        (directory / "bin").mkdir(parents=True)
        (directory / "bin" / "python").write_text("")
        (directory / python_env.READY).write_text("")

    def codegen_python(self, cmake_args=None):
        with contextlib.redirect_stdout(io.StringIO()), contextlib.redirect_stderr(io.StringIO()) as err:
            python = python_env.codegen_python(self.source, cmake_args)
        self.stderr = err.getvalue()
        return python

    def test_made_once_in_the_cache_then_reused(self):
        python = self.codegen_python()
        self.assertTrue(python.is_relative_to(self.root / "cache" / "python"), python)
        self.assertEqual(python.name, "python")
        self.assertEqual(len(self.created), 1)
        self.assertEqual(self.created[0][2], ["click>=8.4.2,<9", "jinja2>=3.1.4,<4"])
        self.assertEqual(self.codegen_python(), python)
        self.assertEqual(len(self.created), 1, "reused, not made again")

    def test_an_unfinished_one_is_made_again(self):
        python = self.codegen_python()
        (python.parent.parent / python_env.READY).unlink()
        self.assertEqual(self.codegen_python(), python)
        self.assertEqual(len(self.created), 2)

    def test_keyed_by_requirements_and_python_version(self):
        reqs = ["click>=8.4.2,<9", "jinja2>=3.1.4,<4"]
        same = python_env.env_dir(reqs, (3, 12))
        self.assertEqual(python_env.env_dir(list(reqs), (3, 12)), same)
        self.assertNotEqual(python_env.env_dir(reqs, (3, 11)), same)
        self.assertNotEqual(python_env.env_dir(["click>=8.5,<9", "jinja2>=3.1.4,<4"], (3, 12)), same)

    def test_the_projects_own_python_wins(self):
        self.assertIsNone(self.codegen_python(["-DPython3_EXECUTABLE=/opt/python/bin/python3"]))
        self.assertIsNone(self.codegen_python(["-DPython3_EXECUTABLE:FILEPATH=/opt/python/bin/python3"]))
        self.assertEqual(self.created, [])

    def test_a_source_tree_without_codegen_requirements_needs_none(self):
        (self.source / "codegen" / "pyproject.toml").unlink()
        self.assertIsNone(self.codegen_python())
        self.assertEqual(self.created, [])

    def test_falls_back_to_a_base_python_that_has_the_packages(self):
        self.fail_create = True
        self.base_has_packages = True
        self.assertEqual(self.codegen_python(), Path("/usr/bin/python3.12"))
        self.assertIn("warning", self.stderr)

    def test_says_how_to_point_at_a_python_when_it_cant(self):
        self.fail_create = True
        with self.assertRaises(python_env.PythonEnvError) as caught:
            self.codegen_python()
        message = str(caught.exception)
        self.assertIn("pip: no network", message)
        self.assertIn("-DPython3_EXECUTABLE=", message)
        self.assertIn("python3-venv", message)

    def test_configure_passes_it_to_cmake(self):
        commands = []
        with mock.patch.object(build, "_run", lambda command, what, toolchain=None: commands.append(command)), \
                contextlib.redirect_stdout(io.StringIO()):
            build.configure(self.source, [], self.root / ".le", "Release")
        python = [a for a in commands[0] if a.startswith("-DPython3_EXECUTABLE=")]
        self.assertEqual(len(python), 1)
        self.assertTrue(Path(python[0].split("=", 1)[1]).is_relative_to(self.root / "cache" / "python"))


class TestBasePython(unittest.TestCase):
    def test_the_running_python_is_used_when_not_frozen(self):
        python, version = python_env.base_python()
        self.assertGreaterEqual(version, python_env.MINIMUM)
        self.assertLess(version, python_env.BELOW)

    def test_none_suitable_is_reported(self):
        with mock.patch.object(python_env, "_FROZEN", True), mock.patch.object(python_env.shutil, "which", lambda name: None):
            with self.assertRaises(python_env.PythonEnvError) as caught:
                python_env.base_python()
        self.assertIn("-DPython3_EXECUTABLE=", str(caught.exception))


if __name__ == "__main__":
    unittest.main()
