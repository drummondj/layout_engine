import os
import tempfile
import unittest

from click.testing import CliRunner

from codegen.cli import cli


class TestCli(unittest.TestCase):
    def setUp(self):
        self.out = tempfile.TemporaryDirectory()

    def tearDown(self):
        self.out.cleanup()

    def run_cli(self, schema):
        return CliRunner().invoke(
            cli, ["--schema", schema, "--output", self.out.name, "--no-history"], catch_exceptions=False
        )

    def test_cli(self):
        result = self.run_cli("examples/solar_system.py")
        self.assertEqual(result.exit_code, 0, result.output)
        for name in ["top", "solar_system", "planet", "star", "root", "ids", "pool", "native_tables"]:
            self.assertTrue(os.path.exists(os.path.join(self.out.name, f"{name}.hpp")), name)

    def test_failure(self):
        self.assertEqual(self.run_cli("tests/schemas/validation_failures.py").exit_code, 1)


if __name__ == "__main__":
    unittest.main()
