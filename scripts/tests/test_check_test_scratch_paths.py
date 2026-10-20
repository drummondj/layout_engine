"""check_test_scratch_paths: which test lines it flags."""

import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import check_test_scratch_paths as lint  # noqa: E402


class TestProblems(unittest.TestCase):
    def flagged(self, name, text):
        return len(lint.problems(Path(name), text))

    def test_cpp_needs_the_pid(self):
        self.assertEqual(self.flagged("a_test.cpp", 'auto p = std::filesystem::temp_directory_path() / "roundtrip.lef";'), 1)
        self.assertEqual(self.flagged("a_test.cpp", 'auto p = fs::temp_directory_path() / ("x_" + std::to_string(::getpid()));'), 0)
        self.assertEqual(self.flagged("a_test.cpp", 'char *p = tmpnam(nullptr);'), 1)
        self.assertEqual(self.flagged("a_test.cpp", '// temp_directory_path() is shared'), 0)
        self.assertEqual(self.flagged("a_test.cpp", 'auto p = le::test::scratch_path("roundtrip.lef");'), 0)
        self.assertEqual(self.flagged("a_test.cpp", 'provider.write_db("/tmp/a.led"); // only queued, never written'), 0)

    def test_tcl_needs_pid_or_tempfile(self):
        self.assertEqual(self.flagged("a_test.tcl", "set f /tmp/le_smoke_settings.json"), 1)
        self.assertEqual(self.flagged("a_test.tcl", "set f [file join $::env(TMPDIR) x.json]"), 1)
        self.assertEqual(self.flagged("a_test.tcl", 'set d [file join [expr {[info exists ::env(TMPDIR)] ? $::env(TMPDIR) : "/tmp"}] x_[pid]]'), 0)
        self.assertEqual(self.flagged("a_test.tcl", "close [file tempfile f x.json]"), 0)
        self.assertEqual(self.flagged("a_test.tcl", "# a comment about /tmp"), 0)

    def test_python_uses_tempfile(self):
        self.assertEqual(self.flagged("test_x.py", 'path = "/tmp/out.txt"'), 1)
        self.assertEqual(self.flagged("test_x.py", "path = tempfile.mkdtemp()"), 0)

    def test_only_test_sources_are_checked(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            for relative in ("src/io/tests/x_test.cpp", "src/io/reader.cpp", "src/_deps/tests/y_test.cpp"):
                (root / relative).parent.mkdir(parents=True, exist_ok=True)
                (root / relative).write_text('auto p = std::filesystem::temp_directory_path() / "x";\n')
            self.assertEqual([p.split(":")[0] for p in lint.check(root)], ["src/io/tests/x_test.cpp"])


if __name__ == "__main__":
    unittest.main()
