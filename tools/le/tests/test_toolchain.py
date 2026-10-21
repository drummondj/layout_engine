"""le's build toolchain: the pinned pixi, the shared environment, and builds
inside it - with downloads and `pixi install` stubbed."""

import contextlib
import hashlib
import io
import os
import subprocess
import tarfile
import tempfile
import unittest
from pathlib import Path
from unittest import mock

from le import build, python_env, toolchain


def pixi_archive(content: bytes = b"#!/bin/sh\necho fake pixi\n") -> bytes:
    """A release tarball holding a `pixi` binary, as pixi publishes it."""
    data = io.BytesIO()
    with tarfile.open(fileobj=data, mode="w:gz") as tar:
        info = tarfile.TarInfo("pixi")
        info.size = len(content)
        tar.addfile(info, io.BytesIO(content))
    return data.getvalue()


def layout_engine_source(directory: Path, lock: str = "version: 7\n") -> Path:
    (directory / "toolchain").mkdir(parents=True)
    (directory / "toolchain" / "pixi.toml").write_text("[workspace]\nname = \"t\"\n")
    (directory / "toolchain" / "pixi.lock").write_text(lock)
    return directory


class ToolchainCase(unittest.TestCase):
    def setUp(self):
        tmp = tempfile.TemporaryDirectory()
        self.addCleanup(tmp.cleanup)
        self.root = Path(tmp.name)
        self.ca_file = self.root / "ca.pem"
        self.ca_file.write_text("certs")
        env = mock.patch.dict(os.environ, {"LE_CACHE_DIR": str(self.root / "cache"), "SSL_CERT_FILE": str(self.ca_file)})
        env.start()
        self.addCleanup(env.stop)
        self.archive = pixi_archive()
        self.downloads = []
        self.installs = []
        for target, replacement in (
            ("_download", self.fake_download),
            ("_install", self.fake_install),
            ("PIXI_SHA256", {"x86_64": hashlib.sha256(self.archive).hexdigest()}),
        ):
            patcher = mock.patch.object(toolchain, target, replacement)
            patcher.start()
            self.addCleanup(patcher.stop)
        patcher = mock.patch.object(toolchain.platform, "machine", lambda: "x86_64")
        patcher.start()
        self.addCleanup(patcher.stop)

    def fake_download(self, url):
        self.downloads.append(url)
        return self.archive

    def fake_install(self, pixi, root):
        self.installs.append(root)
        (root / ".pixi" / "envs" / "default" / "bin").mkdir(parents=True)

    def quietly(self, function, *args):
        with contextlib.redirect_stdout(io.StringIO()):
            return function(*args)


class TestPixi(ToolchainCase):
    def test_downloaded_once_checked_and_cached(self):
        pixi = self.quietly(toolchain.pixi_binary)
        self.assertEqual(pixi, self.root / "cache" / "pixi" / toolchain.PIXI_VERSION / "pixi")
        self.assertEqual(subprocess.run([str(pixi)], capture_output=True, text=True).stdout, "fake pixi\n", "it can be run")
        self.assertIn(f"/v{toolchain.PIXI_VERSION}/pixi-x86_64-unknown-linux-musl.tar.gz", self.downloads[0])
        self.assertEqual(self.quietly(toolchain.pixi_binary), pixi)
        self.assertEqual(len(self.downloads), 1, "cached, not downloaded again")

    def test_a_download_with_the_wrong_checksum_is_refused(self):
        self.archive = pixi_archive(b"something else")
        with self.assertRaises(toolchain.ToolchainError) as caught:
            self.quietly(toolchain.pixi_binary)
        self.assertIn("refusing to run it", str(caught.exception))
        self.assertFalse((self.root / "cache" / "pixi" / toolchain.PIXI_VERSION / "pixi").exists())

    def test_only_the_pinned_machines(self):
        with mock.patch.object(toolchain.platform, "machine", lambda: "riscv64"), self.assertRaises(toolchain.ToolchainError) as caught:
            toolchain.pixi_binary()
        self.assertIn("not riscv64", str(caught.exception))


class TestEnsure(ToolchainCase):
    def test_made_once_per_lock_and_shared(self):
        source = layout_engine_source(self.root / "le")
        made = self.quietly(toolchain.ensure, source)
        self.assertEqual(made.root, self.root / "cache" / "toolchain" / toolchain.key(source))
        self.assertEqual((made.root / "pixi.lock").read_text(), "version: 7\n")
        self.assertEqual(self.installs, [made.root])
        # Another checkout with the same toolchain/ shares it, without installing again.
        other = layout_engine_source(self.root / "other")
        self.assertEqual(self.quietly(toolchain.ensure, other).root, made.root)
        self.assertEqual(len(self.installs), 1)

    def test_a_different_lock_is_another_environment(self):
        first = self.quietly(toolchain.ensure, layout_engine_source(self.root / "le"))
        second = self.quietly(toolchain.ensure, layout_engine_source(self.root / "newer", lock="version: 7\nchanged\n"))
        self.assertNotEqual(first.root, second.root)
        self.assertEqual(len(self.installs), 2)

    def test_a_host_without_ca_certificates_is_told_how_to_get_them(self):
        os.environ["SSL_CERT_FILE"] = str(self.root / "missing.pem")
        with mock.patch.object(toolchain, "CA_STORES", ()), self.assertRaises(toolchain.ToolchainError) as caught:
            self.quietly(toolchain.ensure, layout_engine_source(self.root / "le"))
        self.assertIn("apt install ca-certificates", str(caught.exception))
        self.assertIn("dnf install ca-certificates", str(caught.exception))
        self.assertEqual(self.installs, [])

    def test_a_layout_engine_without_a_toolchain(self):
        (self.root / "old").mkdir()
        with self.assertRaises(toolchain.ToolchainError) as caught:
            toolchain.ensure(self.root / "old")
        self.assertIn("predates `le toolchain`", str(caught.exception))

    def test_commands_run_inside_it_with_its_compilers(self):
        made = self.quietly(toolchain.ensure, layout_engine_source(self.root / "le"))
        self.assertEqual(made.wrap(["cmake", "--build", "b"]),
                         [str(made.pixi), "run", "--manifest-path", str(made.root / "pixi.toml"), "--frozen", "--", "cmake", "--build", "b"])
        args = made.cmake_args()
        self.assertIn(f"-DCMAKE_CXX_COMPILER={made.prefix}/bin/x86_64-conda-linux-gnu-g++", args)
        self.assertIn(f"-DCMAKE_FIND_ROOT_PATH={made.prefix}", args)
        self.assertIn("-DCMAKE_FIND_ROOT_PATH_MODE_LIBRARY=ONLY", args)
        self.assertIn(f"-DPython3_EXECUTABLE={made.prefix}/bin/python3", args)


class TestBuildInIt(ToolchainCase):
    def setUp(self):
        super().setUp()
        self.source = layout_engine_source(self.root / "le")
        self.made = self.quietly(toolchain.ensure, self.source)
        (self.made.prefix / "bin" / "ccache").write_text("")
        self.commands = []
        patcher = mock.patch.object(build, "_run", lambda command, what, toolchain=None: self.commands.append((command, toolchain)))
        patcher.start()
        self.addCleanup(patcher.stop)

    def configure(self, toolchain_or_none, cmake_args=None):
        with mock.patch.object(python_env, "codegen_python", self.no_venv if toolchain_or_none else lambda *a: None):
            return self.quietly(build.configure, self.source, [], self.root / ".le", "Release", cmake_args, toolchain_or_none)

    def no_venv(self, *args):
        raise AssertionError("a toolchain build brings its own Python")

    def test_configure_uses_its_compilers_ccache_and_python(self):
        build_dir = self.configure(self.made, ["-DMY_OPTION=1"])
        command, wrapped_in = self.commands[0]
        self.assertIs(wrapped_in, self.made)
        self.assertLess(command.index(f"-DCMAKE_CXX_COMPILER={self.made.prefix}/bin/x86_64-conda-linux-gnu-g++"),
                        command.index("-DMY_OPTION=1"), "the project's cmake_args come last, so they can override")
        self.assertIn(f"-DCMAKE_CXX_COMPILER_LAUNCHER={self.made.prefix}/bin/ccache", command)
        self.assertEqual((build_dir / "le_toolchain.txt").read_text().strip(), self.made.key)

    def test_switching_toolchains_starts_the_build_tree_afresh(self):
        build_dir = self.configure(None)
        (build_dir / "CMakeCache.txt").write_text("")
        (build_dir / "stale.o").write_text("")
        self.configure(None)
        self.assertTrue((build_dir / "stale.o").exists(), "the same toolchain keeps the tree")
        self.configure(self.made)
        self.assertFalse((build_dir / "stale.o").exists())
        self.assertEqual((build_dir / "le_toolchain.txt").read_text().strip(), self.made.key)

    def test_a_tree_from_before_the_marker_counts_as_the_systems(self):
        build_dir = self.root / ".le" / "build"
        build_dir.mkdir(parents=True)
        (build_dir / "CMakeCache.txt").write_text("")
        (build_dir / "stale.o").write_text("")
        self.configure(self.made)
        self.assertFalse((build_dir / "stale.o").exists())

    def test_every_build_step_runs_inside_it(self):
        with mock.patch.dict(os.environ, {"LE_BUILD_JOBS": "4"}), \
                mock.patch.object(build, "configure", lambda *args: self.root / ".le" / "build"):
            self.quietly(build.build, self.source, [], self.root / ".le", "Release", None, None, None, self.made)
        self.assertEqual([wrapped for _, wrapped in self.commands], [self.made, self.made])
        self.assertEqual(self.commands[0][0][-2:], ["-j", "4"], "LE_BUILD_JOBS is the default job count")


class TestJobs(unittest.TestCase):
    def test_project_then_environment_then_every_cpu(self):
        with mock.patch.dict(os.environ, {"LE_BUILD_JOBS": "3"}):
            self.assertEqual(build.default_jobs(8), 8)
            self.assertEqual(build.default_jobs(None), 3)
        with mock.patch.dict(os.environ, {"LE_BUILD_JOBS": "lots"}):
            self.assertEqual(build.default_jobs(None), os.cpu_count() or 2)


if __name__ == "__main__":
    unittest.main()
