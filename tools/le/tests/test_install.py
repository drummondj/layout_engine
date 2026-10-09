"""Resolution, signatures and the lock, with the build itself replaced by a recorder."""

import contextlib
import io
import json
import os
import subprocess
import unittest
from pathlib import Path
from unittest import mock

from le import build, cli, install as installer, lock as lockfile, project as projectfile
from tests import helpers


class Recorder:
    """Stands in for build.build: remembers what it was asked to build and fakes a bundle."""

    def __init__(self):
        self.calls = []

    def __call__(self, le_source, extension_dirs, state_dir, build_type, jobs, startup, cmake_args=None):
        self.calls.append({"le_source": le_source, "extension_dirs": list(extension_dirs), "startup": startup, "cmake_args": cmake_args})
        bundle = state_dir / "bundle"
        bundle.mkdir(parents=True, exist_ok=True)
        (bundle / "le_shell").write_text("")
        (bundle / "extensions.json").write_text(json.dumps({"extensions": []}))
        return bundle


class TestInstall(unittest.TestCase):
    def setUp(self):
        self.github = helpers.FakeGithub()
        self.root = self.github.root
        self.keys = self.root / "keys"
        self.keys.mkdir()
        self.acme_key = helpers.make_key(self.keys, "acme")
        self.other_key = helpers.make_key(self.keys, "other")
        self.layout_engine = helpers.fake_layout_engine(self.root / "layout_engine")
        self.project = self.root / "project"
        self.recorder = Recorder()
        patcher = mock.patch.object(build, "build", self.recorder)
        patcher.start()
        self.addCleanup(patcher.stop)
        self.addCleanup(self.github.close)
        self.le("init", str(self.project), "--layout-engine-path", str(self.layout_engine))
        self.cwd = os.getcwd()
        os.chdir(self.project)
        self.addCleanup(os.chdir, self.cwd)

    def add(self, *args) -> int:
        """`le add`, then `le install` (add only edits le_project.toml)."""
        code = self.le("add", *args)
        return code if code != 0 else self.le("install")

    def le(self, *args) -> int:
        err = io.StringIO()
        with contextlib.redirect_stderr(err), contextlib.redirect_stdout(io.StringIO()):
            code = cli.main(list(args))
        self.last_error = err.getvalue()
        return code

    def publish_extension(self, name, prefix, tag="v1.0.0", key=None, **manifest):
        """A git repo for extension `name`, tagged (signed with `key`, if given) and pushed to fake GitHub as acme/<name>."""
        work = self.root / "work" / name
        if not work.exists():
            work.mkdir(parents=True)
            helpers.run("git", "init", "-q", cwd=work)
        helpers.write_script_extension(work, name, prefix, version=tag.lstrip("v"), **manifest)
        helpers.git_commit_all(work, tag)
        helpers.git_tag(work, tag, key)
        self.github.publish(work, f"acme/{name}")
        return work

    def trust_acme(self):
        self.assertEqual(self.le("trust", "acme", "@" + str(self.acme_key) + ".pub"), 0, self.last_error)

    def test_path_and_signed_github_extensions_install_in_dependency_order(self):
        self.trust_acme()
        self.publish_extension("base", "Base", key=self.acme_key)
        local = helpers.write_script_extension(self.root / "local", "local", "Local", deps='base = ">=1.0"')
        self.assertEqual(self.le("add", "base", "--github", "acme/base", "--tag", "v1.0.0", "--publisher", "acme"), 0, self.last_error)
        self.assertEqual(self.add("local", "--path", str(local)), 0, self.last_error)

        lock = lockfile.load(self.project)
        self.assertEqual([e.name for e in lock.extensions], ["base", "local"])
        base = lock.extension("base")
        self.assertEqual(base.locked.source, "github:acme/base")
        self.assertTrue(base.locked.signer.startswith("SHA256:"))
        self.assertEqual(lock.extension("local").dependencies, ["base"])
        self.assertEqual(lock.layout_engine_version, "0.2.0")
        built = self.recorder.calls[-1]
        self.assertEqual(built["extension_dirs"], [self.project / ".le" / "src" / "base", local.resolve()])
        self.assertTrue((self.project / ".le" / "src" / "base" / "le_extension.toml").is_file())

    def test_the_lock_is_replayed_and_a_moved_tag_is_refused(self):
        self.trust_acme()
        work = self.publish_extension("base", "Base", key=self.acme_key)
        self.assertEqual(self.add("base", "--github", "acme/base", "--tag", "v1.0.0", "--publisher", "acme"), 0, self.last_error)
        first = lockfile.load(self.project).extension("base").locked.rev

        # Re-point v1.0.0 at a new commit upstream: the locked install refuses it.
        (work / "tcl" / "base.tcl").write_text("proc base_hello {} { return changed }\n")
        helpers.git_commit_all(work, "change")
        helpers.run("git", "tag", "-d", "v1.0.0", cwd=work)
        helpers.git_tag(work, "v1.0.0", self.acme_key)
        self.github.publish(work, "acme/base")
        self.assertEqual(self.le("install"), 1)
        self.assertIn("the tag moved", self.last_error)

        # `le update` accepts it.
        self.assertEqual(self.le("update", "base"), 0, self.last_error)
        self.assertNotEqual(lockfile.load(self.project).extension("base").locked.rev, first)

    def test_unsigned_and_wrongly_signed_tags_are_refused(self):
        self.trust_acme()
        self.publish_extension("plain", "Plain")  # lightweight, unsigned tag
        self.publish_extension("forged", "Forged", key=self.other_key)
        self.assertEqual(self.add("plain", "--github", "acme/plain", "--tag", "v1.0.0", "--publisher", "acme"), 1)
        self.assertIn("isn't signed by a trusted key of acme", self.last_error)
        self.assertEqual(self.add("forged", "--github", "acme/forged", "--tag", "v1.0.0", "--publisher", "acme"), 1)
        self.assertIn("isn't signed by a trusted key of acme", self.last_error)
        self.assertEqual(self.recorder.calls, [])

    def test_a_missing_ssh_keygen_is_reported_clearly(self):
        self.trust_acme()
        self.publish_extension("base", "Base", key=self.acme_key)
        real_which = installer.sources.shutil.which
        with mock.patch.object(installer.sources.shutil, "which", lambda name: None if name == "ssh-keygen" else real_which(name)):
            self.assertEqual(self.add("base", "--github", "acme/base", "--tag", "v1.0.0", "--publisher", "acme"), 1)
        self.assertIn("needs ssh-keygen", self.last_error)

    def test_allow_unsigned_is_recorded(self):
        self.publish_extension("plain", "Plain")
        self.assertEqual(self.add("plain", "--github", "acme/plain", "--tag", "v1.0.0", "--allow-unsigned"), 0, self.last_error)
        self.assertIn("installed unsigned", self.last_error)
        self.assertTrue(lockfile.load(self.project).extension("plain").locked.unsigned)

    def test_a_rev_pin_needs_a_signed_commit(self):
        self.trust_acme()
        work = self.publish_extension("base", "Base", key=self.acme_key)
        (work / "x").write_text("x")
        helpers.git_commit_all(work, "unsigned")
        self.github.publish(work, "acme/base")
        rev = subprocess.run(["git", "rev-parse", "HEAD"], cwd=work, capture_output=True, text=True).stdout.strip()
        self.assertEqual(self.add("base", "--github", "acme/base", "--rev", rev, "--publisher", "acme"), 1)
        self.assertIn(f"commit {rev}", self.last_error)
        (work / "y").write_text("y")
        helpers.git_commit_all(work, "signed", signing_key=self.acme_key)
        self.github.publish(work, "acme/base")
        rev = subprocess.run(["git", "rev-parse", "HEAD"], cwd=work, capture_output=True, text=True).stdout.strip()
        self.assertEqual(self.add("base", "--github", "acme/base", "--rev", rev, "--publisher", "acme"), 0, self.last_error)

    def test_incompatible_and_unlisted_extensions_are_refused(self):
        too_new = helpers.write_script_extension(self.root / "too_new", "too_new", "TooNew", layout_engine=">=0.3")
        self.assertEqual(self.add("too_new", "--path", str(too_new)), 1)
        self.assertIn("needs layout_engine >=0.3", self.last_error)
        self.le("remove", "too_new")
        needy = helpers.write_script_extension(self.root / "needy", "needy", "Needy", deps='missing = ">=1"')
        self.assertEqual(self.add("needy", "--path", str(needy)), 1)
        self.assertIn("depends on missing: add it", self.last_error)
        self.le("remove", "needy")
        misnamed = helpers.write_script_extension(self.root / "misnamed", "real_name", "Real")
        self.assertEqual(self.add("other_name", "--path", str(misnamed)), 1)
        self.assertIn("its manifest names it real_name", self.last_error)

    def test_remove_refuses_a_dependency_still_in_use(self):
        base = helpers.write_script_extension(self.root / "base", "base", "Base")
        user = helpers.write_script_extension(self.root / "user", "user", "User", deps='base = ">=1.0"')
        self.assertEqual(self.le("add", "base", "--path", str(base)), 0)
        self.assertEqual(self.add("user", "--path", str(user)), 0, self.last_error)
        self.assertEqual(self.le("remove", "base"), 1)
        self.assertIn("user depends on base", self.last_error)
        self.assertEqual(self.le("remove", "user"), 0, self.last_error)
        self.assertEqual(self.le("remove", "base"), 0, self.last_error)

    def test_cmake_args_reach_the_build(self):
        text = (self.project / "le_project.toml").read_text().replace('type = "Release"', 'type = "Release"\ncmake_args = ["-DLE_ENABLE_TRACY=OFF"]')
        (self.project / "le_project.toml").write_text(text)
        self.assertEqual(self.le("install"), 0, self.last_error)
        self.assertEqual(self.recorder.calls[-1]["cmake_args"], ["-DLE_ENABLE_TRACY=OFF"])

    def test_cmake_args_cant_set_what_le_does(self):
        for bad in ('["-DLE_EXTENSION_DIRS=/x"]', '["-DCMAKE_BUILD_TYPE=Debug"]', '"-DX=1"', "[1]"):
            text = (self.project / "le_project.toml").read_text()
            original = text
            (self.project / "le_project.toml").write_text(text.replace('type = "Release"', f'type = "Release"\ncmake_args = {bad}'))
            self.assertEqual(self.le("install"), 1, bad)
            self.assertIn("cmake_args", self.last_error)
            (self.project / "le_project.toml").write_text(original)

    def test_bundle_copies_the_build_with_its_startup_script(self):
        (self.project / "init.tcl").write_text("puts hi\n")
        text = (self.project / "le_project.toml").read_text().replace("[project]\n", '[project]\nstartup = "init.tcl"\n')
        (self.project / "le_project.toml").write_text(text)
        destination = self.root / "deploy"
        self.assertEqual(self.le("bundle", str(destination)), 0, self.last_error)
        self.assertTrue((destination / "le_shell").is_file())
        self.assertEqual((destination / "init.tcl").read_text(), "puts hi\n")
        self.assertEqual(json.loads((destination / "extensions.json").read_text())["startup"], "init.tcl")
        self.assertEqual(self.le("bundle", str(destination)), 1, "not over a non-empty directory")
        self.assertIn("isn't an empty directory", self.last_error)

    def test_add_and_remove_only_edit_the_project(self):
        ext = helpers.write_script_extension(self.root / "quiet", "quiet", "Quiet")
        self.assertEqual(self.le("add", "quiet", "--path", str(ext)), 0, self.last_error)
        self.assertEqual(self.recorder.calls, [], "add doesn't build")
        self.assertIsNone(lockfile.load(self.project))
        self.assertEqual(self.le("remove", "quiet"), 0, self.last_error)
        self.assertEqual(self.recorder.calls, [], "nor does remove")
        self.assertEqual(self.le("add", "quiet", "--path", str(self.root / "nowhere")), 1, "a path without a manifest fails at once")
        self.assertNotIn("quiet", (self.project / "le_project.toml").read_text())

    def test_startup_and_staleness(self):
        ext = helpers.write_script_extension(self.root / "ext", "ext", "Ext")
        (self.project / "init.tcl").write_text("puts hi\n")
        text = (self.project / projectfile.PROJECT_FILE).read_text().replace('# startup = "init.tcl"', 'startup = "init.tcl"')
        (self.project / projectfile.PROJECT_FILE).write_text(text)
        self.assertEqual(self.add("ext", "--path", str(ext)), 0, self.last_error)
        self.assertEqual(self.recorder.calls[-1]["startup"], (self.project / "init.tcl").resolve())
        self.assertTrue(installer.is_current(self.project))
        # Editing a path extension makes the install stale.
        os.utime(ext / "tcl" / "ext.tcl", (1e10, 1e10))
        self.assertFalse(installer.is_current(self.project))

    def test_an_invalid_edit_is_rolled_back(self):
        before = (self.project / projectfile.PROJECT_FILE).read_text()
        self.assertEqual(self.le("add", "x", "--github", "acme/x", "--tag", "v1", "--publisher", "nobody"), 1)
        self.assertIn("no keys in [trust]", self.last_error)
        self.assertEqual((self.project / projectfile.PROJECT_FILE).read_text(), before)


if __name__ == "__main__":
    unittest.main()
