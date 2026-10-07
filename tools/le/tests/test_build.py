"""The superbuild `le` generates, and switching an existing build tree over to it."""

import tempfile
import unittest
from pathlib import Path
from unittest import mock

from le import build


class TestSuperbuild(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.root = Path(self.tmp.name)
        self.source = self.root / "layout engine"  # a space, as in any user's path
        self.source.mkdir()

    def test_adds_layout_engine_with_add_subdirectory(self):
        directory = build.write_superbuild(self.source, self.root / "superbuild")
        text = (directory / "CMakeLists.txt").read_text()
        self.assertIn(f"add_subdirectory([[{self.source.resolve()}]] layout_engine)", text)
        self.assertIn("enable_testing()", text)
        self.assertNotIn("LE_EXTENSION_DIRS ", text.split("\n", 2)[2], "le passes the list when configuring")

    def test_an_unchanged_file_isnt_rewritten(self):
        path = build.write_superbuild(self.source, self.root / "superbuild") / "CMakeLists.txt"
        before = path.stat().st_mtime_ns
        build.write_superbuild(self.source, self.root / "superbuild")
        self.assertEqual(path.stat().st_mtime_ns, before)

    def test_a_build_tree_configured_from_the_source_itself_is_replaced(self):
        state = self.root / ".le"
        build_dir = state / "build"
        build_dir.mkdir(parents=True)
        (build_dir / "CMakeCache.txt").write_text(f"CMAKE_HOME_DIRECTORY:INTERNAL={self.source}\n")
        (build_dir / "stale.o").write_text("")
        commands = []

        def run(command, what):
            commands.append(command)
            if command[1] == "--install":
                (state / "bundle").mkdir()

        with mock.patch.object(build, "_run", run):
            build.build(self.source, [], state, "Release", 1, "0.3.0", None)
        self.assertFalse((build_dir / "stale.o").exists())
        self.assertEqual(commands[0][commands[0].index("-S") + 1], str(state / "superbuild"))


if __name__ == "__main__":
    unittest.main()
