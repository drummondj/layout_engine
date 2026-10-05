import tempfile
import unittest
from pathlib import Path

from le import lock as lockfile
from le import project as projectfile


class TestProject(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.root = Path(self.tmp.name)

    def tearDown(self):
        self.tmp.cleanup()

    def write(self, text):
        (self.root / projectfile.PROJECT_FILE).write_text(text)

    def test_template_loads(self):
        self.write(projectfile.template("chip", {"github": "drummondj/layout_engine", "tag": "v0.2.0"}))
        p = projectfile.load(self.root)
        self.assertEqual(p.name, "chip")
        self.assertEqual(p.layout_engine.github, "drummondj/layout_engine")
        self.assertEqual(p.extensions, {})

    def test_sources_are_validated(self):
        base = projectfile.template("chip", {"path": "le"})
        bad = {
            'x = { github = "acme/x" }': "exactly one of tag or rev",
            'x = { github = "acme/x", tag = "v1" }': "needs a publisher",
            'x = { github = "acme/x", tag = "v1", publisher = "acme" }': "no keys in [trust]",
            'x = { github = "nope", tag = "v1", publisher = "acme" }': "owner/repo",
            'x = { path = "p", tag = "v1" }': "takes no tag",
            'x = { path = "p", github = "a/b", tag = "v1" }': "exactly one of github or path",
            'Bad = { path = "p" }': "snake_case",
        }
        for entry, message in bad.items():
            self.write(projectfile.set_entry(base, "extensions", entry.split(" = ", 1)[0], entry.split(" = ", 1)[1]))
            with self.assertRaises(projectfile.ProjectError, msg=entry) as raised:
                projectfile.load(self.root)
            self.assertIn(message, str(raised.exception))

    def test_edits_keep_comments_and_replace_entries(self):
        text = projectfile.template("chip", {"path": "le"})
        text = projectfile.set_entry(text, "extensions", "a", '{ path = "a" }')
        text = projectfile.set_entry(text, "extensions", "b", '{ path = "b" }')
        text = projectfile.set_entry(text, "extensions", "a", '{ path = "a2" }')
        self.assertIn("# startup", text)
        self.assertEqual(text.count("a = "), 1)
        self.assertIn('a = { path = "a2" }', text)
        text = projectfile.remove_entry(text, "extensions", "a")
        self.assertNotIn("a = ", text)
        self.assertIn('b = { path = "b" }', text)
        # A section missing entirely is appended.
        text = projectfile.set_entry(text.replace("[trust]\n", ""), "trust", "acme", '["ssh-ed25519 AAAA"]')
        self.assertTrue(text.rstrip().endswith('[trust]\nacme = ["ssh-ed25519 AAAA"]'))

    def test_lock_round_trips(self):
        lock = lockfile.Lock(
            project_hash="h",
            layout_engine_version="0.2.0",
            extension_api=1,
            layout_engine=lockfile.LockedSource("github:drummondj/layout_engine", rev="abc", signer="SHA256:x"),
            extensions=[
                lockfile.LockedExtension("a", "1.0.0", "script", lockfile.LockedSource("path:/p"), []),
                lockfile.LockedExtension("b", "2.0.0", "compiled", lockfile.LockedSource("github:acme/b", rev="def", unsigned=True), ["a"]),
            ],
        )
        lockfile.save(self.root, lock)
        self.assertEqual(lockfile.load(self.root), lock)


if __name__ == "__main__":
    unittest.main()
