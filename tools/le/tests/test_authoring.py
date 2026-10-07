"""new-extension, test, makemigration and check, with builds and le_shell replaced by recorders."""

import contextlib
import io
import json
import os
import re
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path
from unittest import mock

from le import authoring, build, cli, install as installer
from codegen import extension_manifest
from tests import helpers

REPO = Path(__file__).resolve().parents[3]


def le(*args) -> tuple:
    out, err = io.StringIO(), io.StringIO()
    with contextlib.redirect_stdout(out), contextlib.redirect_stderr(err):
        code = cli.main(list(args))
    return code, out.getvalue(), err.getvalue()


def files(directory: Path) -> dict:
    return {str(p.relative_to(directory)): p.read_text() for p in directory.rglob("*") if p.is_file()}


class TestNewExtension(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.root = Path(self.tmp.name)

    def test_compiled_is_hello_ext_renamed_with_no_history(self):
        directory = authoring.new_extension("acme_router", self.root / "acme_router", False, None, None)
        manifest = extension_manifest.load(directory)
        self.assertEqual((manifest.name, manifest.prefix, manifest.version), ("acme_router", "AcmeRouter", "0.1.0"))
        major, minor, _ = installer.own_layout_engine_version().split(".")
        self.assertEqual(manifest.layout_engine, f">={major}.{minor}, <{major}.{int(minor) + 1}")
        self.assertEqual(manifest.description, "acme_router: a Layout Engine extension")
        self.assertTrue(manifest.compiled)

        written = files(directory)
        self.assertIn("include/acme_router/acme_router.hpp", written)
        self.assertIn("tcl/acme_router.tcl", written)
        self.assertFalse(any(re.search("schema_history|migrations|golden|__pycache__", path) for path in written), sorted(written))
        self.assertFalse(any("hello" in text.lower() for text in written.values()))
        self.assertFalse(written["le_extension.toml"].startswith("#"), "the example's header comment is dropped")
        self.assertIn('VERSION = "0.1.0"', written["schema_ext.py"])
        self.assertIn('kSchemaVersion = "0.1.0";', written["tests/acme_router_test.cpp"])
        schema = written["schema_ext.py"]
        for expected in ('name="AcmeRouterPin"', 'name="ACME_ROUTER_PIN"', 'label="acmeRouterPin"', 'parent="acme_router_pins"'):
            self.assertIn(expected, schema)
        self.assertIn("le_create_acme_router_note", written["tests/acme_router_test.cpp"])
        self.assertIn("namespace acme_router", written["include/acme_router/acme_router.hpp"])

    def test_codegen_accepts_the_compiled_scaffold(self):
        directory = authoring.new_extension("acme_router", self.root / "acme_router", False, "Acme", None)
        result = subprocess.run(
            [sys.executable, "-m", "codegen.cli", "--schema", str(REPO / "src" / "database" / "schema.py"),
             "--output", str(self.root / "generated"), "--target", "render", "--extension", str(directory)],
            env={**os.environ, "PYTHONPATH": str(REPO / "codegen")}, capture_output=True, text=True)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        generated = (self.root / "generated" / "renderable_classes.hpp").read_text()
        self.assertIn("AcmePin", generated)
        self.assertIn("AcmeMarker", generated)

    def test_a_prefix_sets_class_and_command_names(self):
        directory = authoring.new_extension("acme_router", self.root / "acme_router", False, "Acme", None)
        written = files(directory)
        self.assertIn("include/acme_router/acme.hpp", written)
        self.assertIn('name="AcmeNote"', written["schema_ext.py"])
        self.assertIn('label="acmeMarker"', written["schema_ext.py"])
        self.assertIn("proc acme_add_library", written["tcl/acme_router.tcl"])

    def test_script_is_hello_script_renamed(self):
        directory = authoring.new_extension("my_checks", self.root / "my_checks", True, None, "Design checks")
        manifest = extension_manifest.load(directory)
        self.assertEqual((manifest.name, manifest.prefix), ("my_checks", "MyChecks"))
        self.assertEqual(manifest.description, "Design checks")
        self.assertFalse(manifest.compiled)
        written = files(directory)
        self.assertEqual(sorted(written), ["le_extension.toml", "tcl/my_checks.tcl", "tests/my_checks_test.tcl"])
        self.assertIn("proc my_checks_greet", written["tcl/my_checks.tcl"])
        self.assertIn('"Hello, $name!"', written["tcl/my_checks.tcl"], "plain words aren't identifiers")

    def test_a_name_containing_the_templates_words_is_renamed_once(self):
        directory = authoring.new_extension("hello_router", self.root / "hello_router", False, None, None)
        written = files(directory)
        self.assertIn("tcl/hello_router.tcl", written)
        self.assertNotIn("hello_router_router", "".join(written.values()))
        self.assertEqual(extension_manifest.load(directory).prefix, "HelloRouter")

    def test_refusals(self):
        for args, message in (
            (("Bad-Name", self.root / "a", False, None, None), "snake_case"),
            (("good", self.root / "b", False, "lower", None), "PascalCase"),
            (("good", self.root, False, None, None), "already exists"),
        ):
            with self.assertRaises(authoring.AuthoringError) as e:
                authoring.new_extension(*args)
            self.assertIn(message, str(e.exception))

    def test_cli(self):
        os.chdir(self.root)
        self.addCleanup(os.chdir, REPO)
        code, out, _ = le("new-extension", "acme", "--script")
        self.assertEqual(code, 0)
        self.assertIn("make this directory a project", out)
        self.assertIn("`le init --layout-engine-path <layout_engine checkout>`, then `le add acme --path acme`", out)
        self.assertTrue((self.root / "acme" / "le_extension.toml").is_file())
        code, _, err = le("new-extension", "acme", "--script")
        self.assertEqual(code, 1)
        self.assertIn("already exists", err)

    def test_cli_in_a_project_says_how_to_add_it(self):
        project = self.root / "project"
        project.mkdir()
        (project / "le_project.toml").write_text("")  # only found, not read
        os.chdir(project / ".")
        self.addCleanup(os.chdir, REPO)
        code, out, _ = le("new-extension", "acme", "--script", "--directory", "ext/acme")
        self.assertEqual(code, 0)
        self.assertIn("`le add acme --path ext/acme`", out)
        self.assertNotIn("le init", out)

    def test_snake_case_matches_codegen(self):
        from codegen.schema import to_snake_case
        for name in ("Acme", "AcmeRouter", "ABC", "My2Ext"):
            self.assertEqual(authoring.to_snake_case(name), to_snake_case(name))


DB_INFO = """file: /x.led
size: 100 bytes
container version: 1
schema version: {file} (fingerprint aa)
this build: {build} (fingerprint bb) - same schema
{extensions}  Library                          1
objects: 1
session: no"""


class TestCheckFindings(unittest.TestCase):
    def report(self, extensions, file="0.50.0", build="0.50.0"):
        return {f.name: (f.status, f.ok) for f in authoring.findings(DB_INFO.format(file=file, build=build, extensions=extensions))}

    def test_each_status(self):
        report = self.report(
            "extension same 1.0.0 (schema 0.2.0): this build has schema 0.2.0\n"
            "extension older 1.0.0 (schema 0.1.0): this build has schema 0.3.0\n"
            "extension newer 2.0.0 (schema 0.9.0): this build has schema 0.3.0\n"
            "extension gone 1.2.0 (schema 0.1.0): not in this build - the file can't be opened here\n")
        self.assertEqual(report, {
            "layout_engine": ("matches", True),
            "same": ("matches", True),
            "older": ("will migrate", True),
            "newer": ("too new", False),
            "gone": ("missing", False),
        })

    def test_core_schema(self):
        self.assertEqual(self.report("", file="0.49.0")["layout_engine"], ("will migrate", True))
        self.assertEqual(self.report("", file="0.51.0")["layout_engine"], ("too new", False))

    def test_missing_says_how_to_add_it(self):
        text = DB_INFO.format(file="0.50.0", build="0.50.0", extensions="extension gone 1.2.0 (schema 0.1.0): not in this build\n")
        missing = authoring.findings(text)[1]
        self.assertIn('le add gone --github OWNER/REPO --version ">=1.2.0"', missing.detail)

    def test_unexpected_output(self):
        with self.assertRaises(authoring.AuthoringError):
            authoring.findings("error: no such file")


class Recorder:
    """Stands in for build.build (and fakes a project build tree)."""

    def __call__(self, le_source, extension_dirs, state_dir, build_type, jobs, version, startup, cmake_args=None):
        bundle = state_dir / "bundle"
        bundle.mkdir(parents=True, exist_ok=True)
        (bundle / "le_shell").write_text("")
        (state_dir / "build").mkdir(exist_ok=True)
        (state_dir / "build" / "CMakeCache.txt").write_text("Python3_EXECUTABLE:FILEPATH=/opt/py/bin/python3\n")
        return bundle


class TestProjectCommands(unittest.TestCase):
    def setUp(self):
        self.github = helpers.FakeGithub()
        self.addCleanup(self.github.close)
        self.root = self.github.root
        patcher = mock.patch.object(build, "build", Recorder())
        patcher.start()
        self.addCleanup(patcher.stop)
        layout_engine = helpers.fake_layout_engine(self.root / "layout_engine", version=installer.own_layout_engine_version())
        self.project = self.root / "project"
        self.assertEqual(le("init", str(self.project), "--layout-engine-path", str(layout_engine))[0], 0)
        os.chdir(self.project)
        self.addCleanup(os.chdir, REPO)
        major, minor, _ = installer.own_layout_engine_version().split(".")
        self.ext = helpers.write_script_extension(self.root / "acme", "acme", "Acme", layout_engine=f">={major}.{minor}", extra_contents='tcl_tests = ["tests/acme_test.tcl"]')
        (self.ext / "tests").mkdir()
        (self.ext / "tests" / "acme_test.tcl").write_text("puts ok\n")
        self.commands = []

    def record(self, returncode=0):
        def run(command, *args, **kwargs):
            self.commands.append(command)
            return subprocess.CompletedProcess(command, returncode, "", "")
        return mock.patch.object(subprocess, "run", run)

    def test_test_builds_and_runs_ctest_in_a_source_build(self):
        self.assertEqual(le("add", "acme", "--path", str(self.ext))[0], 0)
        with self.record():
            code, _, err = le("test")
        self.assertEqual(code, 0, err)
        build_dir = str(self.project / ".le" / "build")
        self.assertEqual(self.commands[0][:6], ["cmake", "--build", build_dir, "--target", "acme_test_deps", "-j"])
        self.assertEqual(self.commands[1], ["ctest", "--test-dir", build_dir, "--output-on-failure", "--no-tests=error", "-R", r"^(acme)\."])

    def test_test_refuses_an_unknown_extension(self):
        self.assertEqual(le("add", "acme", "--path", str(self.ext))[0], 0)
        code, _, err = le("test", "nope")
        self.assertEqual(code, 1)
        self.assertIn("not in this project: nope (it has acme)", err)

    def test_test_runs_tcl_tests_through_a_release_le_shell(self):
        self.assertEqual(le("add", "acme", "--path", str(self.ext))[0], 0)
        lock_path = self.project / "le_project.lock"
        lock_path.write_text(lock_path.read_text().replace('bundle = "source"', 'bundle = "release"'))
        release_shell = mock.patch.object(installer, "shell_command", lambda root: ["le_shell", "-extensions", "index.json"])
        with self.record(returncode=1), release_shell:
            code, out, _ = le("test", "acme")
        self.assertEqual(code, 1)
        self.assertEqual(self.commands, [["le_shell", "-extensions", "index.json", str(self.ext / "tests" / "acme_test.tcl")]])
        self.assertIn("1 of 1 tests failed: acme_test.tcl", out)

    def test_makemigration_runs_codegen_against_the_project_source(self):
        compiled = self.root / "comp"
        authoring.new_extension("comp", compiled, False, None, None)
        self.assertEqual(le("add", "comp", "--path", str(compiled))[0], 0)
        with self.record():
            code, _, err = le("makemigration", "comp", "--name", "note_author", "--non-interactive")
        self.assertEqual(code, 0, err)
        command = self.commands[-1]
        self.assertEqual(command[:3], ["/opt/py/bin/python3", "-m", "codegen.cli"])
        self.assertIn(str(self.root / "layout_engine" / "src" / "database" / "schema.py"), command)
        self.assertEqual(command[command.index("--migrate-extension") + 1], "comp")
        self.assertEqual(command[command.index("--name") + 1], "note_author")
        self.assertEqual(command[command.index("--extension") + 1], str(compiled))
        self.assertIn("--non-interactive", command)

    def test_makemigration_refusals(self):
        self.assertEqual(le("add", "acme", "--path", str(self.ext), "--no-install")[0], 0)
        self.assertIn("nothing installed yet", le("makemigration", "acme", "--name", "x")[2])
        self.assertEqual(le("install")[0], 0)
        self.assertIn("has no schema", le("makemigration", "acme", "--name", "x")[2])
        self.assertIn("isn't in this project", le("makemigration", "other", "--name", "x")[2])

    def test_check_reports_and_fails_on_a_missing_extension(self):
        self.assertEqual(le("add", "acme", "--path", str(self.ext))[0], 0)
        led = self.root / "design.led"
        led.write_bytes(b"")
        output = DB_INFO.format(file="0.50.0", build="0.50.0", extensions="extension gone 1.0.0 (schema 0.1.0): not in this build\n")

        def run(command, *args, **kwargs):
            self.commands.append(command)
            script = Path(command[-1]).read_text()
            self.assertIn(str(led), script)
            return subprocess.CompletedProcess(command, 0, output, "")

        with mock.patch.object(subprocess, "run", run):
            code, out, _ = le("check", str(led))
        self.assertEqual(code, 1)
        self.assertTrue(any(line.split()[:2] == ["layout_engine", "matches"] for line in out.splitlines()), out)
        self.assertTrue(any(line.split()[:2] == ["gone", "missing"] for line in out.splitlines()), out)
        self.assertEqual(self.commands[-1][0], str(self.project / ".le" / "bundle" / "le_shell"))


if __name__ == "__main__":
    unittest.main()
