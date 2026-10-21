"""The ssh-keygen le checks signatures with: the host's when it has -Y, else
le's own OpenSSH - with making that environment stubbed."""

import os
import subprocess
import tempfile
import unittest
from pathlib import Path
from unittest import mock

from le import openssh, releases, sources, toolchain


def fake_ssh_keygen(directory: Path, message: str) -> Path:
    """An ssh-keygen that answers `ssh-keygen -Y` the way an OpenSSH version does."""
    path = directory / "ssh-keygen"
    path.write_text(f"#!/bin/sh\necho '{message}' >&2\nexit 1\n")
    path.chmod(0o755)
    return path


class TestOpenSSH(unittest.TestCase):
    def setUp(self):
        tmp = tempfile.TemporaryDirectory()
        self.addCleanup(tmp.cleanup)
        self.root = Path(tmp.name)
        openssh.supports_signatures.cache_clear()
        self.addCleanup(openssh.supports_signatures.cache_clear)
        self.made = []

    def environment(self, directory, kind, what):
        self.made.append((directory, kind))
        made = toolchain.Toolchain(root=self.root / "env", pixi=self.root / "pixi", key="k")
        (made.prefix / "bin").mkdir(parents=True, exist_ok=True)
        (made.prefix / "bin" / "ssh-keygen").write_text("")
        return made

    def test_a_new_ssh_keygen_has_y_and_an_old_one_doesnt(self):
        new = fake_ssh_keygen(self.root, "option requires an argument -- Y")  # OpenSSH 9.6
        self.assertTrue(openssh.supports_signatures(str(new)))
        old = self.root / "old"
        old.mkdir()
        self.assertFalse(openssh.supports_signatures(str(fake_ssh_keygen(old, "unknown option -- Y"))))  # OpenSSH 8.0
        self.assertFalse(openssh.supports_signatures(str(self.root / "missing")))

    def test_the_hosts_when_it_can_sign(self):
        new = fake_ssh_keygen(self.root, "option requires an argument -- Y")
        with mock.patch.object(openssh.shutil, "which", lambda name: str(new)), mock.patch.object(toolchain, "environment", self.environment):
            self.assertEqual(openssh.ssh_keygen(), str(new))
        self.assertEqual(self.made, [], "nothing to make")

    def test_les_own_when_the_hosts_cant_or_there_is_none(self):
        old = fake_ssh_keygen(self.root, "unknown option -- Y")
        for which in (str(old), None):
            with mock.patch.object(openssh.shutil, "which", lambda name: which), mock.patch.object(toolchain, "environment", self.environment):
                self.assertEqual(openssh.ssh_keygen(), str(self.root / "env" / ".pixi" / "envs" / "default" / "bin" / "ssh-keygen"))
        self.assertEqual(self.made, [(openssh.MANIFEST, "openssh")] * 2)

    def test_when_le_cant_make_one_it_says_what_it_needs(self):
        def cant(*args):
            raise toolchain.ToolchainError("this machine has no CA certificates")

        with mock.patch.object(openssh.shutil, "which", lambda name: None), mock.patch.object(toolchain, "environment", cant):
            with self.assertRaises(openssh.OpenSSHError) as caught:
                openssh.ssh_keygen()
        self.assertIn("OpenSSH 8.1 or later", str(caught.exception))
        self.assertIn("no CA certificates", str(caught.exception))

    def test_les_openssh_is_locked(self):
        self.assertTrue((openssh.MANIFEST / "pixi.toml").is_file())
        lock = (openssh.MANIFEST / "pixi.lock").read_text()
        self.assertIn("/openssh-10.", lock)


class TestVerifyUsesIt(unittest.TestCase):
    def test_git_verifies_with_it(self):
        calls = []

        def git(*args, cwd=None, check=True):
            calls.append(args)
            return subprocess.CompletedProcess(args, 0, "", 'Good "git" signature for acme with ED25519 key SHA256:abc\n')

        with mock.patch.object(openssh, "ssh_keygen", lambda: "/le/openssh/bin/ssh-keygen"), mock.patch.object(sources, "git", git):
            sources.verify(Path("/repo"), "v1", True, Path("/signers"), "acme")
        self.assertIn("gpg.ssh.program=/le/openssh/bin/ssh-keygen", calls[0])

    def test_release_bundles_are_verified_with_it(self):
        commands = []
        with tempfile.TemporaryDirectory() as tmp:
            tarball = Path(tmp) / "bundle.tar.gz"
            tarball.write_bytes(b"data")
            with mock.patch.object(openssh, "ssh_keygen", lambda: "/le/openssh/bin/ssh-keygen"), \
                    mock.patch.object(releases.subprocess, "run", lambda command, **kwargs: commands.append(command) or subprocess.CompletedProcess(command, 0, "", "")):
                releases._verify(tarball, Path(tmp) / "bundle.sig", Path(tmp) / "signers", "release@layout-engine")
        self.assertEqual(commands[0][:3], ["/le/openssh/bin/ssh-keygen", "-Y", "verify"])


if __name__ == "__main__":
    unittest.main()
