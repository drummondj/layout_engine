"""Version ranges, the downgrade rule, and prebuilt release bundles for script-only projects."""

import contextlib
import io
import json
import os
import unittest
from unittest import mock

from le import build, cli, lock as lockfile, project as projectfile
from tests import helpers
from tests.test_install import Recorder


class TestReleases(unittest.TestCase):
    def setUp(self):
        self.github = helpers.FakeGithub()
        self.addCleanup(self.github.close)
        self.root = self.github.root
        keys = self.root / "keys"
        keys.mkdir()
        self.release_key = helpers.make_key(keys, "release")
        self.acme_key = helpers.make_key(keys, "acme")
        self.recorder = Recorder()
        patcher = mock.patch.object(build, "build", self.recorder)
        patcher.start()
        self.addCleanup(patcher.stop)
        self.project = self.root / "project"
        self.project.mkdir()
        (self.project / projectfile.PROJECT_FILE).write_text(
            projectfile.template("chip", {"github": "drummondj/layout_engine", "version": ">=0.3, <0.4"})
        )
        self.cwd = os.getcwd()
        os.chdir(self.project)
        self.addCleanup(os.chdir, self.cwd)
        self.assertEqual(self.le("trust", "acme", "@" + str(self.acme_key) + ".pub"), 0, self.last_error)

    def le(self, *args) -> int:
        err, out = io.StringIO(), io.StringIO()
        with contextlib.redirect_stderr(err), contextlib.redirect_stdout(out):
            code = cli.main(list(args))
        self.last_error, self.last_output = err.getvalue(), out.getvalue()
        return code

    def publish(self, name, prefix, version, compiled=False, key="acme"):
        work = self.root / "work" / name
        if not work.exists():
            work.mkdir(parents=True)
            helpers.run("git", "init", "-q", cwd=work)
        extra = 'cmake = "le_extension.cmake"' if compiled else ""
        helpers.write_script_extension(work, name, prefix, version=version, layout_engine=">=0.3, <0.4", extra_contents=extra)
        if compiled:
            (work / "le_extension.cmake").write_text("")
        helpers.git_commit_all(work, version)
        helpers.git_tag(work, f"v{version}", self.acme_key if key == "acme" else None)
        self.github.publish(work, f"acme/{name}")

    def test_a_script_project_uses_the_signed_release(self):
        helpers.fake_release(self.github, self.release_key, "0.3.0")
        self.publish("greet", "Greet", "1.0.0")
        self.assertEqual(self.le("add", "greet", "--github", "acme/greet", "--version", ">=1.0", "--publisher", "acme"), 0, self.last_error)
        self.assertEqual(self.recorder.calls, [], "nothing should be compiled")
        lock = lockfile.load(self.project)
        self.assertEqual(lock.layout_engine_bundle, "release")
        self.assertEqual(lock.layout_engine.tag, "v0.3.0")
        self.assertEqual(len(lock.release_sha256), 64)
        index = json.loads((self.project / ".le" / "extensions.json").read_text())
        self.assertEqual(index["layout_engine"], "0.3.0")
        self.assertEqual(index["extensions"][0]["dir"], "ext/greet")
        self.assertTrue((self.project / ".le" / "ext" / "greet" / "tcl" / "greet.tcl").is_file())
        # le shell would run the release's le_shell with the project's index.
        from le import install as installer

        command = installer.shell_command(self.project)
        self.assertTrue(command[0].endswith("releases/v0.3.0/bundle/le_shell"))
        self.assertEqual(command[1:], ["-extensions", str(self.project / ".le" / "extensions.json")])

    def test_bundle_of_a_release_project_carries_its_script_extensions(self):
        helpers.fake_release(self.github, self.release_key, "0.3.0")
        self.publish("greet", "Greet", "1.0.0")
        self.assertEqual(self.le("add", "greet", "--github", "acme/greet", "--version", ">=1.0", "--publisher", "acme"), 0, self.last_error)
        destination = self.root / "deploy"
        self.assertEqual(self.le("bundle", str(destination)), 0, self.last_error)
        self.assertTrue((destination / "le_shell").exists())
        self.assertTrue((destination / "ext" / "greet" / "tcl" / "greet.tcl").is_file())
        index = json.loads((destination / "extensions.json").read_text())
        self.assertEqual([(e["name"], e["dir"]) for e in index["extensions"]], [("greet", "ext/greet")])

    def test_a_tampered_or_unsigned_release_is_refused(self):
        tarball = helpers.fake_release(self.github, self.release_key, "0.3.0")
        with open(tarball, "ab") as f:
            f.write(b"tampered")
        self.assertEqual(self.le("install"), 1)
        self.assertIn("isn't signed by Layout Engine's release key", self.last_error)

    def test_a_compiled_extension_builds_from_source(self):
        helpers.fake_release(self.github, self.release_key, "0.3.0")
        self.publish("fast", "Fast", "1.0.0", compiled=True)
        self.assertEqual(self.le("add", "fast", "--github", "acme/fast", "--tag", "v1.0.0", "--publisher", "acme"), 0, self.last_error)
        self.assertEqual(len(self.recorder.calls), 1)
        self.assertEqual(lockfile.load(self.project).layout_engine_bundle, "source")

    def test_version_ranges_pick_the_highest_matching_signed_tag(self):
        helpers.fake_release(self.github, self.release_key, "0.3.0")
        for v in ("1.0.0", "1.2.0", "2.0.0"):
            self.publish("greet", "Greet", v)
        self.assertEqual(self.le("add", "greet", "--github", "acme/greet", "--version", ">=1.0, <2", "--publisher", "acme"), 0, self.last_error)
        self.assertEqual(lockfile.load(self.project).extension("greet").version, "1.2.0")
        self.assertEqual(self.le("add", "greet", "--github", "acme/greet", "--version", ">=3", "--publisher", "acme"), 1)
        self.assertIn("no vX.Y.Z tag matches >=3", self.last_error)

    def test_a_tag_must_match_its_manifest(self):
        helpers.fake_release(self.github, self.release_key, "0.3.0")
        self.publish("greet", "Greet", "1.0.0")
        work = self.root / "work" / "greet"
        helpers.git_tag(work, "v9.9.9", self.acme_key)  # same commit, manifest says 1.0.0
        self.github.publish(work, "acme/greet")
        self.assertEqual(self.le("add", "greet", "--github", "acme/greet", "--tag", "v9.9.9", "--publisher", "acme"), 1)
        self.assertIn("tag v9.9.9 holds a manifest saying version 1.0.0", self.last_error)

    def test_downgrades_need_allow_downgrade(self):
        helpers.fake_release(self.github, self.release_key, "0.3.0")
        for v in ("1.0.0", "1.2.0"):
            self.publish("greet", "Greet", v)
        self.assertEqual(self.le("add", "greet", "--github", "acme/greet", "--tag", "v1.2.0", "--publisher", "acme"), 0, self.last_error)
        self.assertEqual(self.le("add", "greet", "--github", "acme/greet", "--tag", "v1.0.0", "--publisher", "acme"), 1)
        self.assertIn("would downgrade greet 1.2.0 -> 1.0.0", self.last_error)
        self.assertEqual(self.le("install", "--allow-downgrade"), 0, self.last_error)
        self.assertEqual(lockfile.load(self.project).extension("greet").version, "1.0.0")

    def test_a_release_without_extension_support_is_refused(self):
        tarball = helpers.fake_release(self.github, self.release_key, "0.3.0", sign=False)
        # Rebuild the tarball without extensions.json, then sign it.
        import tarfile

        with tarfile.open(tarball, "w:gz") as archive:
            stub = self.root / "stub_le_shell"
            stub.write_text("#!/bin/sh\n")
            archive.add(stub, arcname="le_shell")
        helpers.run("ssh-keygen", "-q", "-Y", "sign", "-f", str(self.release_key), "-n", "layout_engine-release", str(tarball))
        self.assertEqual(self.le("install"), 1)
        self.assertIn("predates extension support", self.last_error)


if __name__ == "__main__":
    unittest.main()
